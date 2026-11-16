#!/usr/bin/env python3
"""rompatch.py - apply a JSON patch set to a Macintosh ROM image and fix the checksum.

usage: rompatch.py PATCHSET.json IN.rom OUT.rom [--dry-run] [--verbose]

The original ROM is never modified; the patched image is written to OUT.rom and
the Mac ROM checksum (32-bit sum of all 16-bit words from offset 4 to the end of
the file) is recomputed and stored at offset 0.

Patch set format (JSON object):
{
  "name":    "MacIIci timing calibration",       # free text, printed
  "base":    "0x40800000",                       # VMA of ROM offset 0 (for asm, table)
  "notes":   "...",                              # free text, printed (string or list)
  "symbols": {"cont": "0x40800850"},             # optional, passed to ld --defsym for
                                                 #   every "asm" item (external targets)
  "fill":    "0x00",                             # optional pad byte for size rounding
  "include": ["other.json"],                     # optional: apply these sets first
  "patches": [
    {"name": "...", "notes": "...",
     "at": 2114 | "0x842",                       # ROM file offset
     "expect": "70ff",                           # hex of the ORIGINAL bytes (verified)
     "bytes":  "70fe"},                          # hex of the replacement (same length)

    {"name": "...",
     "at": "0x8d2", "expect": "0000...",
     "asm": "not.w %d0\\n bne.s 1f\\n ...",       # GNU as (MIT syntax) source; string or
     "max_len": 14,                              #   list of lines; linked at base+at
     "symbols": {...}},                          # optional extra --defsym for this item
                                                 # result is padded to max_len with NOPs
    {"name": "...",
     "append": "stub.bin"},                      # append a file (relative to the JSON)
    {"name": "...",
     "append_asm": "...", "align": 16,           # assemble+link at base+current_size
     "symbols": {...}}                           # (after optional alignment) and append
  ]
}
An "expect" field on an append item is ignored. An appended ROM is padded to a
multiple of 64 KiB ("fill" byte, default 0x00) before the checksum is computed.
Addresses in "expect"/"at" are ROM FILE offsets, not VMAs; the printed table
shows both. Any "expect" mismatch aborts before anything is written.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

AS = os.environ.get("M68K_AS", "m68k-linux-gnu-as")
LD = os.environ.get("M68K_LD", "m68k-linux-gnu-ld")
OBJCOPY = os.environ.get("M68K_OBJCOPY", "m68k-linux-gnu-objcopy")
OBJDUMP = os.environ.get("M68K_OBJDUMP", "m68k-linux-gnu-objdump")
NOP = b"\x4e\x71"
ROUND = 64 * 1024


def to_int(v):
    if isinstance(v, int):
        return v
    return int(str(v), 0)


def unhex(s):
    return bytes.fromhex("".join(str(s).split()))


def checksum(d):
    n = len(d) - (len(d) & 1)
    return sum(struct.unpack(">%dH" % ((n - 4) // 2), d[4:n])) & 0xFFFFFFFF


def assemble(src, vma, symbols, workdir):
    """Assemble GNU as source and link it at `vma`; return the raw bytes."""
    if isinstance(src, list):
        src = "\n".join(src)
    s_path = os.path.join(workdir, "p.s")
    o_path = os.path.join(workdir, "p.o")
    ld_path = os.path.join(workdir, "p.ld")
    elf_path = os.path.join(workdir, "p.elf")
    bin_path = os.path.join(workdir, "p.bin")
    with open(s_path, "w") as f:
        f.write("\t.text\n" + src + "\n")
    subprocess.run([AS, "-m68030", "-o", o_path, s_path], check=True)
    # Link at the patch address.  The explicit output-section address (not
    # ". = addr") plus SUBALIGN(2) makes ld place the code exactly at `vma`
    # even when it is not a multiple of as's default 4-byte .text alignment
    # (otherwise ld rounds up / inserts NOP padding and PC-relative
    # references to external symbols come out wrong).
    with open(ld_path, "w") as f:
        f.write("SECTIONS { .text 0x%X : SUBALIGN(2) { *(.text) } "
                "/DISCARD/ : { *(*) } }\n" % vma)
    cmd = [LD, "-T", ld_path, "-e", "0x%X" % vma, "-o", elf_path, o_path]
    for k, v in (symbols or {}).items():
        cmd.append("--defsym=%s=0x%X" % (k, to_int(v)))
    subprocess.run(cmd, check=True)
    # Paranoia: confirm the linker really put .text at vma.
    hdr = subprocess.run([OBJDUMP, "-h", elf_path], check=True,
                         capture_output=True, text=True).stdout
    for line in hdr.splitlines():
        f = line.split()
        if len(f) >= 4 and f[1] == ".text":
            if int(f[3], 16) != vma:
                sys.exit("linker placed .text at 0x%s, wanted 0x%X" % (f[3], vma))
            break
    else:
        sys.exit("could not find .text in linker output")
    subprocess.run([OBJCOPY, "-O", "binary", "-j", ".text", elf_path, bin_path], check=True)
    with open(bin_path, "rb") as f:
        return f.read()


def load_patchset(path, seen=None):
    """Load a patch set; "include": ["other.json", ...] (paths relative to this
    file) prepends the included sets' patches and merges their symbols."""
    seen = seen or set()
    path = os.path.abspath(path)
    if path in seen:
        sys.exit("include cycle at %s" % path)
    seen.add(path)
    with open(path) as f:
        ps = json.load(f)
    patches, symbols = [], {}
    for inc in ps.get("include", []):
        sub = load_patchset(os.path.join(os.path.dirname(path), inc), seen)
        for p in sub.get("patches", []):
            if "append" in p and not os.path.isabs(p["append"]):
                p = dict(p, append=os.path.join(os.path.dirname(sub["_path"]), p["append"]))
            patches.append(p)
        symbols.update(sub.get("symbols", {}))
        ps.setdefault("base", sub.get("base"))
        ps.setdefault("fill", sub.get("fill"))
    symbols.update(ps.get("symbols", {}))
    ps["symbols"] = symbols
    ps["patches"] = patches + ps.get("patches", [])
    ps["_path"] = path
    return ps


def pad_nops(code, max_len, what):
    if len(code) > max_len:
        sys.exit("%s: assembled code is %d bytes, max_len is %d" % (what, len(code), max_len))
    if (max_len - len(code)) & 1:
        sys.exit("%s: cannot NOP-pad an odd number of bytes" % what)
    return code + NOP * ((max_len - len(code)) // 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("patchset")
    ap.add_argument("rom_in")
    ap.add_argument("rom_out")
    ap.add_argument("--dry-run", action="store_true", help="verify and print, write nothing")
    ap.add_argument("--verbose", "-v", action="store_true", help="print assembled bytes")
    args = ap.parse_args()

    ps = load_patchset(args.patchset)
    json_dir = os.path.dirname(os.path.abspath(args.patchset))
    with open(args.rom_in, "rb") as f:
        rom = bytearray(f.read())
    orig_len = len(rom)
    base = to_int(ps.get("base") or 0)
    fill = to_int(ps.get("fill") or 0) & 0xFF
    gsyms = dict(ps.get("symbols", {}))

    print("patch set : %s" % ps.get("name", os.path.basename(args.patchset)))
    print("input     : %s (%d bytes, checksum %08X, %s)" % (
        args.rom_in, orig_len, struct.unpack(">I", rom[:4])[0],
        "OK" if checksum(rom) == struct.unpack(">I", rom[:4])[0] else "BAD"))
    notes = ps.get("notes")
    if notes:
        for line in ([notes] if isinstance(notes, str) else notes):
            print("note      : %s" % line)

    # Pass 1: verify every "expect" against the ORIGINAL image before touching it.
    for i, p in enumerate(ps.get("patches", [])):
        if "at" not in p:
            continue
        at = to_int(p["at"])
        exp = unhex(p.get("expect", ""))
        if not exp:
            sys.exit("patch %d (%s): 'expect' is required for in-place patches" % (i, p.get("name")))
        if at + len(exp) > orig_len:
            sys.exit("patch %d (%s): offset 0x%X beyond ROM" % (i, p.get("name"), at))
        got = bytes(rom[at:at + len(exp)])
        if got != exp:
            sys.exit("patch %d (%s): expect mismatch at 0x%X: ROM has %s, expected %s"
                     % (i, p.get("name"), at, got.hex(), exp.hex()))
        if "bytes" in p and len(unhex(p["bytes"])) != len(exp):
            sys.exit("patch %d (%s): 'bytes' length differs from 'expect'" % (i, p.get("name")))
        if "asm" in p and to_int(p.get("max_len", len(exp))) != len(exp):
            sys.exit("patch %d (%s): max_len must equal len(expect)" % (i, p.get("name")))

    # Pass 2: apply.
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, p in enumerate(ps.get("patches", [])):
            name = p.get("name", "patch %d" % i)
            syms = dict(gsyms)
            syms.update(p.get("symbols", {}))
            if "append" in p or "append_asm" in p:
                align = to_int(p.get("align", 2))
                while len(rom) % align:
                    rom.append(fill)
                at = len(rom)
                if "append" in p:
                    path = os.path.join(json_dir, p["append"])
                    with open(path, "rb") as f:
                        data = f.read()
                    kind = "append file"
                else:
                    data = assemble(p["append_asm"], base + at, syms, tmp)
                    kind = "append asm"
                rom += data
                rows.append((at, base + at, len(data), kind, name, b"", data))
            else:
                at = to_int(p["at"])
                exp = unhex(p["expect"])
                if "bytes" in p:
                    new = unhex(p["bytes"])
                    kind = "bytes"
                else:
                    new = pad_nops(assemble(p["asm"], base + at, syms, tmp), len(exp), name)
                    kind = "asm"
                rom[at:at + len(new)] = new
                rows.append((at, base + at, len(new), kind, name, exp, new))

    if len(rom) != orig_len:
        while len(rom) % ROUND:
            rom.append(fill)
    old_sum = struct.unpack(">I", rom[:4])[0]
    new_sum = checksum(rom)
    rom[:4] = struct.pack(">I", new_sum)

    print()
    print("%-8s %-10s %5s %-11s %s" % ("offset", "address", "len", "kind", "name"))
    for at, vma, ln, kind, name, exp, new in rows:
        print("%08X %08X %5d %-11s %s" % (at, vma, ln, kind, name))
        if exp:
            print("%s old: %s" % (" " * 36, exp.hex()))
        if exp or args.verbose:
            print("%s new: %s" % (" " * 36, new.hex() if len(new) <= 64 else new[:64].hex() + "..."))
    print()
    print("output    : %s (%d bytes%s), checksum %08X -> %08X" % (
        args.rom_out, len(rom),
        "" if len(rom) == orig_len else ", grown from %d" % orig_len, old_sum, new_sum))
    if args.dry_run:
        print("dry run, nothing written")
        return
    os.makedirs(os.path.dirname(os.path.abspath(args.rom_out)), exist_ok=True)
    with open(args.rom_out, "wb") as f:
        f.write(rom)


if __name__ == "__main__":
    main()
