#!/usr/bin/env python3
"""
campatch.py - re-aim / nudge the cockpit camera inside DATA.MIX in place
(companion to fovpatch.py; same in-place technique, no ReMixer needed).

Usage:
    python campatch.py                         centre the view: LocalInterest X -> 0 (default)
    python campatch.py --yaw 0.02              look slightly to the other side
    python campatch.py --yaw -0.04             put the original value back
    python campatch.py --shift-x -0.02         also move the camera 0.02 along X (see below)
    python campatch.py --path "E:\\...\\DATA.MIX"  patch a specific file (default: ./DATA.MIX)

What it changes (in DATA\\CAMERAS\\COCKPITCAMERAS.BML, every car's entry)
  * LocalInterest X  - the sideways part of the point the camera looks at.
    The game ships it as -0.04 for most cars, i.e. the cockpit view is turned a
    few degrees to one side.  0 means "look straight ahead".
  * LocalPos X (optional, --shift-x) - the camera's sideways position (the seat).

Values are written into the SAME number of characters as the original, because
that keeps the file the same size and lets the archive be patched in place exactly
like fovpatch.py does (the recompressed block is made the same size as the old
one).  An entry whose number would not fit in the same space is left alone and
reported.  A DATA.MIX.bak backup is made the first time.  Close the game first.
Existing changes (e.g. your FOV edit) are kept: only those two properties change.
"""
import argparse, os, random, shutil, struct, sys, zlib

DEFAULT_FILE = "DATA\\CAMERAS\\COCKPITCAMERAS.BML"


def key(name):
    return name.encode("utf-16-le") + b"\x00\x00"


def parse_table(mix):
    ver, count, tab, _ = struct.unpack_from("<IIII", mix, 0)
    if ver != 1 or not (0 < count < 1000000) or not (0 < tab < len(mix)):
        raise SystemExit("This doesn't look like a DATA.MIX this tool understands.")
    pos, recs = tab, []
    for _ in range(count):
        nlen = struct.unpack_from("<I", mix, pos)[0]
        raw = mix[pos + 4:pos + 4 + nlen]
        pos += 4 + nlen
        name = bytes(b ^ ((i * 58) & 0xFF) for i, b in enumerate(raw)).rstrip(b"\x00").decode("latin1")
        off, comp, unc, flag = struct.unpack_from("<IIIH", mix, pos)
        pos += 22
        recs.append((off, comp, unc, flag, name))
    return recs


def find_file(mix, recs, wanted):
    wanted = wanted.upper().replace("/", "\\")
    if not wanted.startswith("DATA\\"):
        wanted = "DATA\\" + wanted
    hits = [r for r in recs if r[4].upper() == wanted]
    if len(hits) != 1:
        raise SystemExit("Expected exactly one entry named %s, found %d." % (wanted, len(hits)))
    off, comp, unc, flag, name = hits[0]
    if flag != 1:
        raise SystemExit("That entry isn't stored in the compressed form this tool handles.")
    return off, comp, unc, zlib.decompress(mix[off:off + comp], -15), name


def fit(value, width):
    """Format value as text exactly `width` characters long, or None if impossible."""
    neg = value < 0 and round(abs(value), 6) != 0
    for dec in range(0, 7):
        s = "%.*f" % (dec, abs(value))
        if neg:
            s = "-" + s
        if len(s) == width:
            return s
    return None


def edit_property(data, prop, fn):
    """Call fn(old_string) -> new_string or None for each `prop` entry. Returns (bytes, changed, skipped)."""
    k = key(prop)
    d = bytearray(data)
    pos = changed = skipped = 0
    while True:
        i = d.find(k, pos)
        if i < 0:
            break
        j = i + len(k)
        # some properties have an extra 2-byte gap (00 00) before the length, some don't
        gap = 2 if struct.unpack_from("<H", d, j)[0] == 0 else 0
        length = struct.unpack_from("<I", d, j + gap)[0]
        if not (0 < length < 4096) or length % 2:
            raise SystemExit("Unexpected layout around a %s entry; refusing to patch." % prop)
        start, end = j + gap + 4, j + gap + 4 + length
        raw = bytes(d[start:end])
        text = raw.decode("utf-16-le", "replace").split("\x00")[0]
        new = fn(text)
        if new is None or new == text:
            skipped += (new is None)
        elif len(new) != len(text):
            skipped += 1
        else:
            nb = new.encode("utf-16-le")
            d[start:start + len(nb)] = nb
            changed += 1
        pos = end
    return bytes(d), changed, skipped


def patch(data, yaw, shift):
    report = {}

    def interest(t):
        p = t.split(",")
        if len(p) != 3:
            return None
        x = fit(yaw, len(p[0]))
        if x is None:
            return None
        return ",".join([x, p[1], p[2]])

    def posfn(t):
        p = t.split(",")
        if len(p) != 3:
            return None
        try:
            v = float(p[0]) + shift
        except ValueError:
            return None
        x = fit(round(v, 4), len(p[0]))
        if x is None:
            return None
        return ",".join([x, p[1], p[2]])

    data, c, s = edit_property(data, "LocalInterest", interest)
    report["LocalInterest"] = (c, s)
    if shift:
        data, c, s = edit_property(data, "LocalPos", posfn)
        report["LocalPos"] = (c, s)
    return data, report


def candidates(plain, target):
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
        c = zlib.compressobj(rnd.choice((9, 9, 8)), zlib.DEFLATED, -15, rnd.choice((8, 9)), rnd.choice(strategies))
        out, prev = b"", 0
        for cp in cuts:
            out += c.compress(plain[prev:cp]) + c.flush(zlib.Z_SYNC_FLUSH)
            prev = cp
        out += c.compress(plain[prev:]) + c.flush()
        if len(out) == target:
            yield out


def best_shorter(plain, target):
    best = None
    for lvl in (9, 8):
        for mem in (8, 9):
            c = zlib.compressobj(lvl, zlib.DEFLATED, -15, mem, zlib.Z_DEFAULT_STRATEGY)
            out = c.compress(plain) + c.flush()
            if len(out) <= target and (best is None or len(out) > len(best)):
                best = out
    return best


def main():
    ap = argparse.ArgumentParser(description="Re-aim the cockpit camera inside DATA.MIX in place.")
    ap.add_argument("--yaw", type=float, default=0.0,
                    help="LocalInterest X (default 0 = straight ahead; original is -0.04)")
    ap.add_argument("--shift-x", type=float, default=0.0,
                    help="move camera sideways by this much (added to LocalPos X); default 0 = don't touch")
    ap.add_argument("--path", default="DATA.MIX", help="path to DATA.MIX (default: ./DATA.MIX)")
    ap.add_argument("--file", default=DEFAULT_FILE, help="camera file inside the archive")
    args = ap.parse_args()
    path = args.path
    if not os.path.isfile(path):
        raise SystemExit("File not found: %s" % path)

    with open(path, "rb") as f:
        mix = f.read()
    off, comp, unc, data, name = find_file(mix, parse_table(mix), args.file)
    print("%s: slot at 0x%X, %d bytes" % (name, off, comp))

    new_plain, rep = patch(data, args.yaw, args.shift_x)
    for prop, (c, s) in rep.items():
        print("  %s: %d changed, %d left alone (value would not fit / different format)" % (prop, c, s))
    if all(c == 0 for c, _ in rep.values()):
        print("Nothing to change (already set?). Nothing was written.")
        return

    stream = next((s for s in candidates(new_plain, comp) if zlib.decompress(s, -15) == new_plain), None)
    padded = False
    if stream is None:
        stream = best_shorter(new_plain, comp)
        if stream is None:
            raise SystemExit("Could not fit the patched file into its slot. Nothing was changed.")
        stream += b"\x00" * (comp - len(stream))
        padded = True
        print("Note: exact-size stream not found; padded with zeros (same as fovpatch.py does).")

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

    with open(path, "rb") as f:
        check = f.read()
    ok = zlib.decompress(check[off:off + comp], -15) == new_plain
    same = check[:off] == mix[:off] and check[off + comp:] == mix[off + comp:] and len(check) == len(mix)
    if not (ok and same):
        raise SystemExit("Verification failed. Restore the backup: %s" % bak)
    print("Done. Cockpit camera: yaw %s%s%s" % (args.yaw, ", shift-x %s" % args.shift_x if args.shift_x else "",
                                               " (padded)" if padded else ""))


if __name__ == "__main__":
    main()
