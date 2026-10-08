#!/usr/bin/env python3
"""
slrexe.py - all the SLRX64.exe patches in one script (clutch required for gear changes + 2D gauge in the
cockpit view).  Replaces clutchexe.py and gaugeexe.py (use this instead of them, not as well as them).
EXPERIMENTAL: written from reading the game code.

The script sets the exe to EXACTLY the features you ask for: anything you don't ask for is put back
to the original game code, so you can run it again with different options at any time.

Usage:
    python slrexe.py                       everything on  (clutch for both paths + 2D gauge in cockpit)
    python slrexe.py --no-gauge            clutch only
    python slrexe.py --no-clutch           gauge only
    python slrexe.py --clutch-mode seq     clutch check only for the "Manual" (sequential) path
    python slrexe.py --clutch-mode h       clutch check only for the H-shifter path
    python slrexe.py --always              require the clutch even if the game's ManualClutch flag is off
    python slrexe.py --threshold 0.5       how far the pedal must be pressed (0..1, default 0.25)
    python slrexe.py --undo                everything off (original game code)
    python slrexe.py --path "E:\\...\\SLRX64.exe"   patch a specific file (default: ./SLRX64.exe)

Features
  clutch  A gear change is ignored unless the clutch pedal is pressed (only when the game's own
          ManualClutch setting is on, unless --always).  Covers the sequential "Manual" option and
          the H-shifter.  New code lives in unused space at the end of .text.
  gauge   The 2D HUD gauge (speed / rpm / gear) is allowed to show in the cockpit camera, as it does
          in the Sim view.  It is still hidden if the HUD "Gauge" option is off.

A SLRX64.exe.bak backup is made the first time. Close the game first.
A Steam update / "Verify integrity of game files" restores the original exe; just run this again.
Only the 64-bit SLRX64.exe from this game build is supported (the bytes are verified first).
"""
import argparse, os, shutil, struct, sys

BASE = 0x140000000
# ---- clutch ---------------------------------------------------------------------------------------
H_SITE, H_BACK = 0x1409B3CCD, 0x1409B3CD5
S_SITE, S_SKIP, S_BACK = 0x1409B3CFB, 0x1409B3E11, 0x1409B3D01
CAVE_H, CAVE_S = 0x14112CE10, 0x14112CE40
CAVE_LEN = 0x60
H_ORIG = bytes.fromhex("488b4108c6413001")
S_ORIG = bytes.fromhex("0f8410010000")
H_CTX = (0x1409B3CC0, bytes.fromhex("488b411880b882000000007417"))
S_CTX = (0x1409B3CF0, bytes.fromhex("80793000488b51084c8bc1"))
# ---- gauge ----------------------------------------------------------------------------------------
G_SITE = 0x1402C9814
G_ORIG = bytes.fromhex("0fb64209")
G_NEW = bytes.fromhex("b0019090")
G_CTX = [(0x1402C97E7, bytes.fromhex("80b9e900000000")),
         (0x1402C9818, bytes.fromhex("8881e8000000"))]


def pe_sections(b):
    e = struct.unpack_from("<I", b, 0x3C)[0]
    if b[e:e + 4] != b"PE\0\0":
        raise SystemExit("Not a PE file.")
    nsec = struct.unpack_from("<H", b, e + 6)[0]
    opt = struct.unpack_from("<H", b, e + 20)[0]
    secs = []
    for i in range(nsec):
        o = e + 24 + opt + i * 40
        name = b[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, ro = struct.unpack_from("<IIII", b, o + 8)
        secs.append((name, va, vs, ro, rs))
    return secs


def va2off(secs, va):
    rva = va - BASE
    for name, sva, vs, ro, rs in secs:
        if sva <= rva < sva + max(vs, rs):
            return ro + rva - sva
    raise SystemExit("Address %#x is not inside the file." % va)


def rel32(src_end, dst):
    return struct.pack("<i", dst - src_end)


def build_caves(threshold, use_flag, do_h, do_s):
    thr = struct.pack("<f", threshold)
    if not (0 < threshold < 1):
        raise SystemExit("--threshold must be between 0 and 1.")
    out = {}
    if do_h:
        c = bytearray()
        if use_flag:
            c += bytes.fromhex("80b88100000000")          # cmp byte [rax+0x81],0   (ManualClutch)
            c += b"\x74\x00"                              # je normal   (patched below)
            je_at = len(c) - 1
        c += bytes.fromhex("488b4108")                    # mov rax,[rcx+8]
        c += bytes.fromhex("81b898020000") + thr        # cmp dword [rax+0x298],threshold
        c += b"\x7c\x00"                                  # jl skip
        jl_at = len(c) - 1
        cont = len(c)
        c += bytes.fromhex("c6413001")                    # mov byte [rcx+0x30],1
        c += b"\xe9" + rel32(CAVE_H + len(c) + 5, H_BACK)  # jmp back
        if use_flag:
            c[je_at] = len(c) - (je_at + 1)
            c += bytes.fromhex("488b4108")                # normal: mov rax,[rcx+8]
            c += b"\xeb" + bytes([(cont - (len(c) + 2)) & 0xFF])   # jmp cont
        c[jl_at] = len(c) - (jl_at + 1)
        c += b"\xc3"                                      # skip: ret
        out[CAVE_H] = bytes(c)
    if do_s:
        c = bytearray()
        c += b"\x0f\x84" + rel32(CAVE_S + 6, S_SKIP)      # je skip   (original instruction)
        if use_flag:
            c += bytes.fromhex("488b4118")                # mov rax,[rcx+0x18]
            c += bytes.fromhex("80b88100000000")          # cmp byte [rax+0x81],0
            c += b"\x74\x00"                              # je go
            je_at = len(c) - 1
        c += bytes.fromhex("81ba98020000") + thr          # cmp dword [rdx+0x298],threshold
        c += b"\x0f\x8c" + rel32(CAVE_S + len(c) + 6, S_SKIP)   # jl skip
        if use_flag:
            c[je_at] = len(c) - (je_at + 1)
        c += b"\xe9" + rel32(CAVE_S + len(c) + 5, S_BACK)  # go: jmp back
        out[CAVE_S] = bytes(c)
    for va, code in out.items():
        if len(code) > 0x30 and va == CAVE_H:
            raise SystemExit("internal error: cave too large")
    return out


def main():
    ap = argparse.ArgumentParser(description="SLRX64.exe patches: clutch for gear changes + 2D gauge in cockpit.")
    ap.add_argument("--no-clutch", action="store_true", help="leave the clutch feature off")
    ap.add_argument("--no-gauge", action="store_true", help="leave the gauge feature off")
    ap.add_argument("--clutch-mode", choices=["seq", "h", "both"], default="both")
    ap.add_argument("--always", action="store_true", help="clutch: ignore the game's ManualClutch flag")
    ap.add_argument("--threshold", type=float, default=0.25, help="clutch: pedal threshold 0..1")
    ap.add_argument("--undo", action="store_true", help="turn everything off (original code)")
    ap.add_argument("--path", default="SLRX64.exe")
    args = ap.parse_args()
    path = args.path
    if not os.path.isfile(path):
        raise SystemExit("File not found: %s" % path)
    with open(path, "rb") as f:
        b = bytearray(f.read())
    secs = pe_sections(b)

    want_clutch = not args.no_clutch and not args.undo
    want_gauge = not args.no_gauge and not args.undo

    oH, oS = va2off(secs, H_SITE), va2off(secs, S_SITE)
    oCH = va2off(secs, CAVE_H)
    oG = va2off(secs, G_SITE)
    # make sure this is the right build
    for va, ctx in [H_CTX, S_CTX] + G_CTX:
        o = va2off(secs, va)
        if bytes(b[o:o + len(ctx)]) != ctx:
            raise SystemExit("This doesn't look like the SLRX64.exe this tool was written for "
                             "(code at %#x differs). Nothing was changed." % va)
    # every patch site must be either original or one of our patches
    if not (bytes(b[oH:oH + 8]) == H_ORIG or b[oH] == 0xE9):
        raise SystemExit("Unexpected bytes at the H-shifter site. Nothing was changed.")
    if not (bytes(b[oS:oS + 6]) == S_ORIG or b[oS] == 0xE9):
        raise SystemExit("Unexpected bytes at the sequential site. Nothing was changed.")
    if bytes(b[oG:oG + 4]) not in (G_ORIG, G_NEW):
        raise SystemExit("Unexpected bytes at the gauge site. Nothing was changed.")
    patched_now = b[oH] == 0xE9 or b[oS] == 0xE9
    if not patched_now and bytes(b[oCH:oCH + CAVE_LEN]) != b"\0" * CAVE_LEN:
        raise SystemExit("The free space at the end of .text is not empty. Nothing was changed.")

    before = bytes(b)
    # 1) back to original everywhere
    b[oH:oH + 8] = H_ORIG
    b[oS:oS + 6] = S_ORIG
    b[oCH:oCH + CAVE_LEN] = b"\0" * CAVE_LEN
    b[oG:oG + 4] = G_ORIG
    # 2) apply what was asked for
    if want_clutch:
        do_h = args.clutch_mode in ("h", "both")
        do_s = args.clutch_mode in ("seq", "both")
        for va, code in build_caves(args.threshold, not args.always, do_h, do_s).items():
            o = va2off(secs, va)
            b[o:o + len(code)] = code
        if do_h:
            b[oH:oH + 8] = b"\xe9" + rel32(H_SITE + 5, CAVE_H) + b"\x90\x90\x90"
        if do_s:
            b[oS:oS + 6] = b"\xe9" + rel32(S_SITE + 5, CAVE_S) + b"\x90"
    if want_gauge:
        b[oG:oG + 4] = G_NEW

    if want_clutch:
        cl = "clutch ON (%s, threshold %.2f%s)" % (args.clutch_mode, args.threshold,
                                                  ", ignoring ManualClutch flag" if args.always else "")
    else:
        cl = "clutch off"
    summary = "%s; gauge %s" % (cl, "ON" if want_gauge else "off")

    if bytes(b) == before:
        print("Already set: %s. Nothing was written." % summary)
        return
    bak = path + ".bak"
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print("Backup written: %s" % bak)
    try:
        with open(path, "r+b") as f:
            for off, ln in ((oH, 8), (oS, 6), (oCH, CAVE_LEN), (oG, 4)):
                f.seek(off)
                f.write(bytes(b[off:off + ln]))
    except PermissionError:
        raise SystemExit("Can't write to the file. Close the game first.")
    with open(path, "rb") as f:
        check = f.read()
    if check != bytes(b):
        raise SystemExit("Verification failed. Restore the backup: %s" % bak)
    print("Done. %s." % ("Original game code restored" if args.undo else summary))


if __name__ == "__main__":
    main()
