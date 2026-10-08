#!/usr/bin/env python3
"""
hidecockpit.py - hide the driver (arms/hands) AND the steering wheel in DATA.MIX, in place.
One script for what alwayshide.py + hidewheel2.py + hidewheel3.py did separately.
(Companion to campatch.py; same in-place technique, no ReMixer needed.)

Usage:
    python hidecockpit.py                 apply all three edits
    python hidecockpit.py --undo          put everything back
    python hidecockpit.py --path "E:\\...\\DATA.MIX"   patch a specific file (default: ./DATA.MIX)

What it changes
  1. DATA\\GRAPHIC\\DRIVER\\DRIVERBEHAVIOURCONFIG.BML : driver visibility class -> "always hidden"
  2. DATA\\GRAPHIC\\CAR\\DRIVING.BML                  : steering wheel rotation class -> "always hidden"
  3. DATA\\GRAPHIC\\CAR\\CARBEHAVIOURCONFIG.BML        : wheel renamed in the BaseCar visibility list
                                                       (STEERING_WHEEL -> STEERING_WHEEX)
  Everything is checked before anything is written.  If a changed file no longer fits its old
  slot, it is stored at the end of DATA.MIX and its table entry repointed (a note is printed).
  Your other edits (FOV, camera yaw) are kept.

A DATA.MIX.bak backup is made the first time (if none exists). Close the game first.
"""
import argparse, os, random, shutil, struct, sys, zlib

PATCHES = []   # (file inside archive, function, label) - filled in below


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


def table_entry_pos(mix, wanted):
    """File position of the (offset, compressed, uncompressed, flag) fields of one table entry."""
    ver, count, tab, _ = struct.unpack_from("<IIII", mix, 0)
    pos = tab
    wanted = wanted.upper().replace("/", "\\")
    if not wanted.startswith("DATA\\"):
        wanted = "DATA\\" + wanted
    for _ in range(count):
        nlen = struct.unpack_from("<I", mix, pos)[0]
        raw = mix[pos + 4:pos + 4 + nlen]
        pos += 4 + nlen
        name = bytes(b ^ ((i * 58) & 0xFF) for i, b in enumerate(raw)).rstrip(b"\x00").decode("latin1")
        if name.upper() == wanted:
            return pos
        pos += 22
    raise SystemExit("Table entry not found.")


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


OLD_CLS_driver = "g_uiCLSID_DRIVER_VISIBILITY_BEHAVIOUR"
NEW_CLS_driver = "g_uiCLSID_ALWAYS_HIDDEN_BEHAVIOUR"


def set_class_driver(data, restore):
    """Change the behaviour class of 'SetDriverVisibility' from DRIVER_VISIBILITY to ALWAYS_HIDDEN
    (the class the game itself uses to keep the notepad hidden). Same slot size, padded with zeros.
    Returns (new_bytes, changed, found)."""
    d = bytearray(data)
    src, dst = (NEW_CLS_driver, OLD_CLS_driver) if restore else (OLD_CLS_driver, NEW_CLS_driver)
    ks = src.encode("utf-16-le")
    kd = dst.encode("utf-16-le")
    if not d.count(ks):
        if d.count(kd):
            return bytes(d), 0, 1      # already in the requested state
        raise SystemExit("Could not find the driver visibility entry. Nothing was changed.")
    if d.count(ks) != 1 and not restore:
        raise SystemExit("Expected exactly one %s entry, found %d. Nothing was changed." % (src, d.count(ks)))
    if restore:
        # the original driver slot is 76 bytes; find our padded copy by its length field
        i = -1
        j = 0
        while True:
            j = d.find(ks, j)
            if j < 0:
                break
            if struct.unpack_from("<I", d, j - 4)[0] == 76:
                i = j
                break
            j += 2
        if i < 0:
            return bytes(d), 0, 1      # nothing patched, so already original
        d[i:i + 76] = kd + b"\x00" * (76 - len(kd))
        return bytes(d), 1, 1
    i = d.find(ks)
    length = struct.unpack_from("<I", d, i - 4)[0]
    if length != 76:
        raise SystemExit("Unexpected layout around the class entry; refusing to patch.")
    d[i:i + length] = kd + b"\x00" * (length - len(kd))
    return bytes(d), 1, 1



OLD_CLS_wheel = "g_uiCLSID_STATIC_ROTATION_BEHAVIOUR"
NEW_CLS_wheel = "g_uiCLSID_ALWAYS_HIDDEN_BEHAVIOUR"
BEHAVIOUR_wheel = "SteeringWheelRotation"
SLOT_wheel = 72   # byte size of the class-name slot in the original file


def set_class_wheel(data, restore):
    """Change the class of the 'SteeringWheelRotation' behaviour (the one that turns the car's
    STEERING_WHEEL object) from STATIC_ROTATION to ALWAYS_HIDDEN, zero-padded to the same slot size.
    The gearstick and handbrake use the same rotation class and are left alone.
    Returns (new_bytes, changed, found)."""
    d = bytearray(data)
    old = OLD_CLS_wheel.encode("utf-16-le")
    new = NEW_CLS_wheel.encode("utf-16-le")
    name = BEHAVIOUR_wheel.encode("utf-16-le")
    src, dst = (new, old) if restore else (old, new)

    def hits_for(pattern):
        out, j = [], 0
        while True:
            j = d.find(pattern, j)
            if j < 0:
                return out
            if struct.unpack_from("<I", d, j - 4)[0] == SLOT_wheel and d.find(name, j, j + 300) > 0:
                out.append(j)
            j += 2

    todo = hits_for(src)
    if not todo:
        if hits_for(dst):
            return bytes(d), 0, 1      # already in the requested state
        raise SystemExit("Could not find the steering wheel rotation entry. Nothing was changed.")
    if len(todo) != 1:
        raise SystemExit("Expected exactly one steering wheel entry, found %d. Nothing was changed." % len(todo))
    i = todo[0]
    d[i:i + SLOT_wheel] = dst + b"\x00" * (SLOT_wheel - len(dst))
    return bytes(d), 1, 1



OLD_NAME_list = "STEERING_WHEEL"
NEW_NAME_list = "STEERING_WHEEX"   # same length; matches no object in any car model


def set_class_list(data, restore):
    """In the 'BaseCar' item of CARBEHAVIOURCONFIG.BML, rename the wheel object so the car's
    visibility behaviours stop touching it (they keep switching it back on).
    Returns (new_bytes, changed, found)."""
    d = bytearray(data)
    old = OLD_NAME_list.encode("utf-16-le")
    new = NEW_NAME_list.encode("utf-16-le")
    src, dst = (new, old) if restore else (old, new)

    def hits_for(pattern):
        out, j = [], 0
        while True:
            j = d.find(pattern, j)
            if j < 0:
                return out
            # a whole logical-object-name value: its length field is 32 bytes (14 chars + padding)
            if struct.unpack_from("<I", d, j - 4)[0] == 32 and d[j + len(pattern):j + len(pattern) + 2] == b"\x00\x00":
                out.append(j)
            j += 2

    todo = hits_for(src)
    if not todo:
        if hits_for(dst):
            return bytes(d), 0, 1      # already in the requested state
        raise SystemExit("Could not find the steering wheel object entry. Nothing was changed.")
    if len(todo) != 1:
        raise SystemExit("Expected exactly one steering wheel object entry, found %d. Nothing was changed." % len(todo))
    i = todo[0]
    d[i:i + len(dst)] = dst
    return bytes(d), 1, 1



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



PATCHES = [
    ("DATA\\GRAPHIC\\DRIVER\\DRIVERBEHAVIOURCONFIG.BML", set_class_driver, "driver / hands"),
    ("DATA\\GRAPHIC\\CAR\\DRIVING.BML", set_class_wheel, "steering wheel behaviour"),
    ("DATA\\GRAPHIC\\CAR\\CARBEHAVIOURCONFIG.BML", set_class_list, "steering wheel visibility list"),
]


def build_stream(new_plain, comp):
    """Return (stream, relocate, padded)."""
    stream = next((s for s in candidates(new_plain, comp) if zlib.decompress(s, -15) == new_plain), None)
    if stream is not None:
        return stream, False, False
    stream = best_shorter(new_plain, comp)
    if stream is not None:
        return stream + b"\x00" * (comp - len(stream)), False, True
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return c.compress(new_plain) + c.flush(), True, False


def write_entry(path, name, new_plain, bak):
    with open(path, "rb") as f:
        mix = f.read()
    off, comp, unc, data, _ = find_file(mix, parse_table(mix), name)
    stream, relocate, padded = build_stream(new_plain, comp)
    if relocate:
        print("  Note: patched data is %d bytes (slot is %d); storing it at the end of the file instead."
              % (len(stream), comp))
    elif padded:
        print("  Note: exact-size stream not found; padded with zeros.")
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print("Backup written: %s" % bak)
    tpos = table_entry_pos(mix, name)
    try:
        with open(path, "r+b") as f:
            if relocate:
                f.seek(len(mix))
                f.write(stream)
                f.seek(tpos)
                f.write(struct.pack("<III", len(mix), len(stream), unc))
            else:
                f.seek(off)
                f.write(stream)
    except PermissionError:
        raise SystemExit("Can't write to the file. Close the game first.")
    with open(path, "rb") as f:
        check = f.read()
    off2, comp2, unc2, data2, _ = find_file(check, parse_table(check), name)
    if relocate:
        same = check[:tpos] == mix[:tpos] and check[tpos + 12:len(mix)] == mix[tpos + 12:]
    else:
        same = check[:off] == mix[:off] and check[off + comp:] == mix[off + comp:] and len(check) == len(mix)
    if data2 != new_plain or not same:
        raise SystemExit("Verification failed for %s. Restore the backup: %s" % (name, bak))


def main():
    ap = argparse.ArgumentParser(description="Hide the driver/hands and the steering wheel, in place.")
    ap.add_argument("--undo", action="store_true", help="undo: restore everything")
    ap.add_argument("--path", default="DATA.MIX", help="path to DATA.MIX (default: ./DATA.MIX)")
    args = ap.parse_args()
    path = args.path
    if not os.path.isfile(path):
        raise SystemExit("File not found: %s" % path)
    bak = path + ".bak"

    # 1) work out every change first; nothing is written if any of them looks wrong
    with open(path, "rb") as f:
        mix = f.read()
    recs = parse_table(mix)
    todo = []
    for name, fn, label in PATCHES:
        off, comp, unc, data, _ = find_file(mix, recs, name)
        new_plain, changed, found = fn(data, args.undo)
        print("%-32s %s" % (label, "will change" if changed else "already %s" % ("original" if args.undo else "patched")))
        if changed:
            todo.append((name, new_plain, label))
    if not todo:
        print("Nothing to do. Nothing was written.")
        return

    # 2) write them
    for name, new_plain, label in todo:
        print("Writing %s ..." % label)
        write_entry(path, name, new_plain, bak)
    print("Done. %s." % ("Everything restored" if args.undo else "Driver/hands and steering wheel hidden"))


if __name__ == "__main__":
    main()
