#!/usr/bin/env python3
"""
fovpatch.py - change the cockpit camera field of view inside DATA.MIX in place.
No unpacking or repacking with ReMixer needed.

Usage:
    python fovpatch.py 70                      patch DATA.MIX in the current folder
    python fovpatch.py 70 "E:\\...\\DATA.MIX"    patch a specific file
    python fovpatch.py 55                      put the original value back
    python fovpatch.py 70 DATA.MIX --file CAMERAS\\BONNETCAMERAS.BML
                                               patch a different camera file instead

What it does
  * DATA.MIX stores every file as a raw-deflate block; a table at the end lists each
    file's offset and sizes.
  * It finds the camera file by name (default DATA\\CAMERAS\\COCKPITCAMERAS.BML; names in
    the table are lightly scrambled, the tool unscrambles them), changes every
    "DegFocal" value, recompresses it, and writes it back into the SAME
    slot. The compressed size is made exactly equal to the original, so no other
    offset or size in the archive changes.
  * A backup (DATA.MIX.bak) is made the first time. Close the game first.
"""
import argparse
import os
import random
import shutil
import struct
import sys
import zlib

KEY = "DegFocal".encode("utf-16-le") + b"\x00\x00"   # property name, UTF-16 + terminator


def parse_table(mix):
    ver, count, tab, _ = struct.unpack_from("<IIII", mix, 0)
    if ver != 1 or not (0 < count < 1000000) or not (0 < tab < len(mix)):
        raise SystemExit("This doesn't look like a DATA.MIX this tool understands.")
    pos = tab
    recs = []
    for _ in range(count):
        nlen = struct.unpack_from("<I", mix, pos)[0]
        raw = mix[pos + 4:pos + 4 + nlen]
        pos += 4 + nlen
        # name bytes are XORed with a fixed keystream: byte i ^ ((i * 58) & 0xFF)
        name = bytes(b ^ ((i * 58) & 0xFF) for i, b in enumerate(raw)).rstrip(b"\x00").decode("latin1")
        off, comp, unc, flag = struct.unpack_from("<IIIH", mix, pos)
        pos += 22
        recs.append((off, comp, unc, flag, name))
    return recs


def count_entries(data):
    return data.count(KEY)


def find_file(mix, recs, wanted):
    wanted = wanted.upper().replace("/", "\\")
    if not wanted.startswith("DATA\\"):
        wanted = "DATA\\" + wanted
    hits = [r for r in recs if r[4].upper() == wanted]
    if len(hits) != 1:
        raise SystemExit("Expected exactly one entry named %s in the archive, found %d." % (wanted, len(hits)))
    off, comp, unc, flag, name = hits[0]
    if flag != 1:
        raise SystemExit("That entry isn't stored in the compressed form this tool handles.")
    data = zlib.decompress(mix[off:off + comp], -15)
    n = count_entries(data)
    if n < 1:
        raise SystemExit("%s contains no DegFocal entries." % name)
    return n, off, comp, unc, data, name


def patch_values(data, value):
    text = str(value)
    if not (1 <= len(text) <= 3):
        raise SystemExit("Value must be 1 to 3 digits (e.g. 70).")
    w = text.encode("utf-16-le")
    d = bytearray(data)
    pos = 0
    n = 0
    while True:
        i = d.find(KEY, pos)
        if i < 0:
            break
        j = i + len(KEY)
        if d[j + 2:j + 6] != b"\x08\x00\x00\x00":
            raise SystemExit("Unexpected layout around a DegFocal entry; refusing to patch.")
        d[j + 6:j + 14] = b"\x00" * 8
        d[j + 6:j + 6 + len(w)] = w
        n += 1
        pos = j + 14
    return bytes(d), n


def candidates(plain, target):
    """Yield compressed streams of exactly 'target' bytes that inflate to 'plain'."""
    strategies = [zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED]
    for lvl in (9, 8):
        for mem in (8, 9, 7, 6):
            for st in strategies:
                for wb in (-15, -14, -13):
                    c = zlib.compressobj(lvl, zlib.DEFLATED, wb, mem, st)
                    out = c.compress(plain) + c.flush()
                    if len(out) == target:
                        yield out
    rnd = random.Random(1)
    for _ in range(20000):
        k = rnd.choice((1, 1, 2, 3))
        cuts = sorted(rnd.sample(range(1, len(plain)), k))
        c = zlib.compressobj(rnd.choice((9, 9, 8)), zlib.DEFLATED, -15,
                             rnd.choice((8, 9)), rnd.choice(strategies))
        out = b""
        prev = 0
        for cp in cuts:
            out += c.compress(plain[prev:cp]) + c.flush(zlib.Z_SYNC_FLUSH)
            prev = cp
        out += c.compress(plain[prev:]) + c.flush()
        if len(out) == target:
            yield out


def best_shorter(plain, target):
    """Fallback: the shortest stream that fits in the slot (caller pads with zeros)."""
    best = None
    for lvl in (9, 8):
        for mem in (8, 9):
            c = zlib.compressobj(lvl, zlib.DEFLATED, -15, mem, zlib.Z_DEFAULT_STRATEGY)
            out = c.compress(plain) + c.flush()
            if len(out) <= target and (best is None or len(out) > len(best)):
                best = out
    return best


def main():
    ap = argparse.ArgumentParser(description="Change camera field of view inside DATA.MIX in place.")
    ap.add_argument("value", type=int, help="new value, 1-3 digits (original is 55)")
    ap.add_argument("path", nargs="?", default="DATA.MIX", help="path to DATA.MIX (default: ./DATA.MIX)")
    ap.add_argument("--file", default="CAMERAS\\COCKPITCAMERAS.BML",
                    help="camera file inside the archive (default: CAMERAS\\COCKPITCAMERAS.BML)")
    args = ap.parse_args()
    value, path = args.value, args.path
    if not os.path.isfile(path):
        raise SystemExit("File not found: %s" % path)

    with open(path, "rb") as f:
        mix = f.read()
    recs = parse_table(mix)
    n, off, comp, unc, data, name = find_file(mix, recs, args.file)
    print("%s: %d DegFocal entries, slot at 0x%X, %d bytes" % (name, n, off, comp))

    new_plain, count = patch_values(data, value)
    stream = None
    for s in candidates(new_plain, comp):
        if zlib.decompress(s, -15) == new_plain:
            stream = s
            break
    padded = False
    if stream is None:
        stream = best_shorter(new_plain, comp)
        if stream is None:
            raise SystemExit("Could not fit the patched file into its slot. Nothing was changed.")
        stream = stream + b"\x00" * (comp - len(stream))
        padded = True
        print("Note: exact-size stream not found; padded with zeros (should be fine, but if the "
              "game misbehaves, restore DATA.MIX.bak).")

    bak = path + ".bak"
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print("Backup written: %s" % bak)

    try:
        with open(path, "r+b") as f:
            f.seek(off)
            f.write(stream)
    except PermissionError:
        raise SystemExit("Can't write to the file. Close the game first.")

    # verify by reading it back
    with open(path, "rb") as f:
        check = f.read()
    ok = zlib.decompress(check[off:off + comp], -15) == new_plain
    same_elsewhere = check[:off] == mix[:off] and check[off + comp:] == mix[off + comp:] and len(check) == len(mix)
    if not (ok and same_elsewhere):
        raise SystemExit("Verification failed. Restore the backup: %s" % bak)
    print("Done: %d entries in %s now set to %d%s." % (count, name, value, " (padded)" if padded else ""))


if __name__ == "__main__":
    main()
