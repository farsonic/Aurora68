#!/usr/bin/env python3
"""End-to-end test of the MAME ``maciici_ps`` bridge machine (docs/06-mame-bridge.md).

Starts MAME headless, connects to the bridge socket and exercises the
motherboard through the protocol: VIA1 / RBV register reads (both I/O
aliases), VIA1 interrupt -> ipl, posted writes, a frame-buffer pattern that
must show up in the final snapshot, and a SCSI pseudo-DMA underflow that must
come back as a bus error.

    tools/bridge_e2e.py [--mame reference/mame/mamepsmac] [--seconds 20] [--attach]
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_client import BridgeClient, BusError, REPLY_FLAG_RESET, STATUS_BUSERROR  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VIA1 = 0x50F00000          # registers every 0x200
VIA1_ALIAS = 0x50000000    # A20-A23 ignored by the decoder
VIA1_ALIAS2 = 0x50F40000   # second VIA1 window in the IIci map
RBV = 0x50F26000
RBV_ALIAS = 0x50026000
RBV_DAC_ADDR = 0x50F24000
RBV_DAC_DATA = 0x50F24004
SCSI = 0x50F10000          # 5380 register n at + n*0x10 (even byte)
SCSI_DRQ = 0x50F06000      # pseudo-DMA data

VIA_REG_NAMES = ["ORB", "ORA", "DDRB", "DDRA", "T1CL", "T1CH", "T1LL", "T1LH",
                 "T2CL", "T2CH", "SR", "ACR", "PCR", "IFR", "IER", "ORA_NH"]

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, (" -- " + detail) if detail else ""))
    return cond


def wait_for_socket(path, proc, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(path):
            try:
                return BridgeClient(path).connect()
            except (ConnectionRefusedError, FileNotFoundError):
                pass
        if proc is not None and proc.poll() is not None:
            raise SystemExit("MAME exited early with %d" % proc.returncode)
        time.sleep(0.1)
    raise SystemExit("bridge socket %s did not appear" % path)


def run_tests(b):
    # --- reset handshake -------------------------------------------------
    r = b.poll()
    check("initial POLL reports reset requested (flags bit0)", r.flags & REPLY_FLAG_RESET, "flags=%02X ipl=%d" % (r.flags, r.ipl))
    r = b.reset_ack()
    r = b.poll()
    check("RESET_ACK clears the reset flag", not (r.flags & REPLY_FLAG_RESET), "flags=%02X" % r.flags)

    # --- VIA1 -------------------------------------------------------------
    regs = [b.read8(VIA1 + n * 0x200) for n in range(16)]
    print("  VIA1 @%08X: " % VIA1 + " ".join("%s=%02X" % (VIA_REG_NAMES[n], regs[n]) for n in range(16)))
    alias = [b.read8(VIA1_ALIAS + n * 0x200) for n in range(16)]
    alias2 = [b.read8(VIA1_ALIAS2 + n * 0x200) for n in range(16)]
    # timers tick between reads; compare the static registers only
    static = [0, 1, 2, 3, 6, 7, 10, 11, 12, 14]
    check("VIA1 alias 0x50000000 matches 0x50F00000", all(regs[n] == alias[n] for n in static))
    check("VIA1 alias 0x50F40000 matches 0x50F00000", all(regs[n] == alias2[n] for n in static))
    check("VIA1 IER reads 0x80 (no sources enabled) after reset", regs[14] == 0x80, "IER=%02X" % regs[14])
    check("VIA1 port A reads 0xC7 pattern (IIci PA6|PA2|PA1 + outputs)", (regs[1] & 0xC7) == 0xC7 or regs[1] == 0x00, "ORA=%02X" % regs[1])
    w = b.read16(VIA1 + 0xE * 0x200)
    check("VIA1 16-bit read duplicates the byte on both lanes", w == (regs[14] | (regs[14] << 8)), "%04X" % w)
    # timer 1 must be counting (emulated time advances while we talk)
    t1a = b.read8(VIA1 + 4 * 0x200) | (b.read8(VIA1 + 5 * 0x200) << 8)
    time.sleep(0.02)
    t1b = b.read8(VIA1 + 4 * 0x200) | (b.read8(VIA1 + 5 * 0x200) << 8)
    check("VIA1 T1 counter changes over 20 ms (emulated time follows wall clock)", t1a != t1b, "%04X -> %04X" % (t1a, t1b))

    # --- RBV (both aliases) -------------------------------------------------
    rbv = [b.read8(RBV + n) for n in range(0x14)]
    rbv_alias = [b.read8(RBV_ALIAS + n) for n in range(0x14)]
    print("  RBV  @%08X: " % RBV + " ".join("%02X" % v for v in rbv))
    check("RBV alias 0x50026000 matches 0x50F26000", rbv == rbv_alias)
    check("RBV reg 0x10 = video off | montype 6 (0x70)", rbv[0x10] == 0x70, "%02X" % rbv[0x10])
    check("RBV reg 0 reset value 0x4F", rbv[0] == 0x4F, "%02X" % rbv[0])
    ier = b.read8(RBV + 0x1C00)
    ifr = b.read8(RBV_ALIAS + 0x1A00)
    print("  RBV IER=%02X IFR=%02X" % (ier, ifr))
    check("RBV extended IER/IFR readable", True, "IER=%02X IFR=%02X" % (ier, ifr))

    # --- interrupts -> ipl ----------------------------------------------------
    r = b.poll()
    check("ipl is 0 with all VIA1 sources disabled", r.ipl == 0, "ipl=%d" % r.ipl)
    b.write8(VIA1 + 0xD * 0x200, 0x7F)        # clear IFR
    b.write8(VIA1 + 0xE * 0x200, 0x82)        # IER: enable CA1 (60.15 Hz VBL from RBV)
    time.sleep(0.05)                          # > 2 VBL periods of wall-clock time
    r = b.poll()
    ifr = b.read8(VIA1 + 0xD * 0x200)
    check("VIA1 CA1 (60 Hz) interrupt raises ipl 1", r.ipl == 1, "ipl=%d IFR=%02X" % (r.ipl, ifr))
    check("VIA1 IFR shows IRQ|CA1 (0x82)", (ifr & 0x82) == 0x82, "IFR=%02X" % ifr)
    b.write8(VIA1 + 0xE * 0x200, 0x02)        # disable CA1
    b.write8(VIA1 + 0xD * 0x200, 0x7F)        # ack
    r = b.poll()
    check("ipl drops to 0 after disabling/acknowledging", r.ipl == 0, "ipl=%d" % r.ipl)
    # ipl 2 via RBV (VIA2) slot/any interrupt is not provoked here; SCC would be ipl 4

    # --- RAM / frame buffer -----------------------------------------------------
    # CLUT: pen 0xFE (1bpp '0') white, pen 0xFF (1bpp '1') black, then video on, 1bpp
    b.write8(RBV_DAC_ADDR, 0xFE)
    for v in (0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00):
        b.write8(RBV_DAC_DATA, v)
    b.write8(RBV + 0x10, 0x00)
    check("RBV video enabled, 1bpp (reg 0x10 reads 0x30 = montype only)", b.read8(RBV + 0x10) == 0x30, "%02X" % b.read8(RBV + 0x10))

    # clear a bit more than we paint, with normal writes
    for a in range(0x400, 0x1000, 4):
        b.write32(a, 0)
    # paint 0x000-0x3FF black with POSTED writes (no replies)
    t0 = time.time()
    for a in range(0, 0x400, 4):
        b.write32(a, 0xFFFFFFFF, posted=True)
    # the next non-posted request orders after them
    v0, v1, v2 = b.read32(0), b.read32(0x3FC), b.read32(0x400)
    check("posted writes landed (0x00000000/0x3FC = FFFFFFFF, 0x400 untouched)", v0 == 0xFFFFFFFF and v1 == 0xFFFFFFFF and v2 == 0, "%08X %08X %08X (256 posted writes in %.1f ms)" % (v0, v1, v2, (time.time() - t0) * 1000))
    b.write16(0x402, 0xBEEF)
    b.write8(0x401, 0xAA)
    check("byte/word lanes big-endian in RAM", b.read32(0x400) == 0x00AABEEF, "%08X" % b.read32(0x400))
    b.write32(0x400, 0)
    check("unaligned 32-bit read", b.read32(0x3FE) == 0xFFFF0000, "%08X" % b.read32(0x3FE))

    # --- bus error: SCSI pseudo-DMA read underflow --------------------------------
    b.raise_on_berr = False
    # NB: do not use register 7 (start DMA initiator receive) here: with no target
    # asserting REQ MAME's ncr5380 re-arms a 0 ns state timer forever and emulated
    # time freezes (pre-existing MAME behaviour, same on the stock maciici).  The
    # helper enters READ_WAIT_DRQ on a register 6 write as well, and the 5380
    # ignores register 6 in initiator mode.
    b.write8(SCSI + 2 * 0x10, 0x02)           # 5380 mode: DMA
    b.write8(SCSI + 6 * 0x10, 0x00)           # helper -> READ_WAIT_DRQ
    v = b.read8(SCSI_DRQ)                     # FIFO empty, no DRQ -> underflow -> BERR pulse
    r = b.last
    check("SCSI pseudo-DMA underflow returns status 1 (bus error)", r.status == STATUS_BUSERROR, "status=%d data=%02X" % (r.status, v))
    b.write8(SCSI + 2 * 0x10, 0x00)           # back to non-DMA
    v = b.read8(SCSI + 2 * 0x10)
    check("access after the bus error is OK again", b.last.status == 0, "mode=%02X status=%d" % (v, b.last.status))
    b.raise_on_berr = True

    # a read of the ROM switch address still returns ROM data (overlay already off)
    rom0 = b.read32(0x40000000)
    check("ROM readable at 0x40000000 (first long of IIci ROM = checksum 368CADFE)", rom0 == 0x368CADFE, "%08X" % rom0)


def analyze_png(path):
    try:
        from PIL import Image
    except ImportError:
        print("  PIL not available: pip install pillow"); return False
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    def row_black(y):
        return sum(1 for x in range(w) if px[x, y] == (0, 0, 0))

    def row_white(y):
        return sum(1 for x in range(w) if px[x, y] == (255, 255, 255))

    top = [row_black(y) for y in range(0, 12)]
    mid = row_white(40)
    print("  snapshot %s: %dx%d, black pixels per row 0..11: %s, white pixels in row 40: %d" % (path, w, h, top, mid))
    ok = all(t == w for t in top) and mid == w
    check("final snapshot: rows 0-11 all black (0x400 bytes of 0xFF @ 80 bytes/row), row 40 white", ok)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mame", default=os.path.join(ROOT, "reference", "mame", "mamepsmac"))
    ap.add_argument("--rompath", default=os.path.join(ROOT, "mame", "roms"))
    ap.add_argument("--chd", default=os.path.join(ROOT, "images", "chd", "hd20.chd"))
    ap.add_argument("--snapdir", default="/tmp/psmac-e2e-snap")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--sock", default=os.environ.get("PSMAC_BRIDGE_SOCK", "/tmp/psmac-bridge-e2e.sock"),
                    help="socket path (private default so a concurrently running Emu68 setup on the standard path is not disturbed)")
    ap.add_argument("--attach", action="store_true", help="do not start MAME, connect to a running one")
    ap.add_argument("--extra", default="", help="extra MAME arguments")
    a = ap.parse_args()

    proc = None
    if not a.attach:
        os.makedirs(a.snapdir, exist_ok=True)
        cmd = [a.mame, "maciici_ps", "-rompath", a.rompath, "-video", "none", "-sound", "none", "-nothrottle",
               "-hard1", a.chd, "-snapshot_directory", a.snapdir, "-seconds_to_run", str(a.seconds),
               "-skip_gameinfo", "-nvram_directory", "/tmp/psmac-e2e-nvram", "-cfg_directory", "/tmp/psmac-e2e-cfg"] + a.extra.split()
        env = dict(os.environ, PSMAC_BRIDGE_SOCK=a.sock)
        print("starting: " + " ".join(cmd))
        proc = subprocess.Popen(cmd, env=env, cwd=os.path.dirname(a.mame), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    t0 = time.time()
    b = wait_for_socket(a.sock, proc)
    print("connected to %s after %.1f s" % (a.sock, time.time() - t0))
    try:
        run_tests(b)
    finally:
        b.close()

    if proc is not None:
        out, _ = proc.communicate(timeout=a.seconds + 60)
        print("MAME exited with %d; output:\n    " % proc.returncode + out.strip().replace("\n", "\n    "))
        # -seconds_to_run writes the final screenshot through open_next(): NNNN.png
        d = os.path.join(a.snapdir, "maciici_ps")
        pngs = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".png")) if os.path.isdir(d) else []
        if pngs:
            analyze_png(pngs[-1])
        else:
            check("final snapshot written", False, d + " has no png")

    failed = [r for r in results if not r[1]]
    print("\n%d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
