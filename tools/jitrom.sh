#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# tools/jitrom.sh - the JIT's changed-code test (tools/jitrom/jitrom.s, docs/08 item 134): builds the
# stand-in ROM, boots it on the solo board in QEMU (tools/run-solo.sh, so one QEMU at a time) and
# checks every answer it logs. No Apple ROM or System is needed; the declaration ROM is
# mac/build/psmac.declrom (make -C mac), which brings up the mailbox the program logs through.
#   usage: tools/jitrom.sh [extra kernel options]   e.g. tools/jitrom.sh mac.jitcheck=1
#   env: IMG (the kernel, default psmac-build/Emu68.img), LOGDIR (default /tmp/psmac-jitrom)
# Exit status 0 when cases 1-4 give what they expect and case 5 gives 2 with mac.jitcheck=1, or 1
# (the old code, the designed behaviour) without it.
set -e
cd "$(dirname "$0")/.."
LOGDIR=${LOGDIR:-/tmp/psmac-jitrom}; mkdir -p "$LOGDIR"
m68k-linux-gnu-as -m68040 -o "$LOGDIR/jitrom.o" tools/jitrom/jitrom.s
m68k-linux-gnu-ld -Ttext=0x40800000 -o "$LOGDIR/jitrom.elf" "$LOGDIR/jitrom.o"
m68k-linux-gnu-objcopy -O binary "$LOGDIR/jitrom.elf" "$LOGDIR/jitrom.rom"
head -c 1048576 /dev/zero > "$LOGDIR/empty.img"
ROM="$LOGDIR/jitrom.rom" HD="$LOGDIR/empty.img" LOGDIR="$LOGDIR" tools/run-solo.sh 40 wifi=off "$@" >/dev/null 2>&1 || true
CHECK=0; case " $* " in *" mac.jitcheck=1 "*) CHECK=1;; esac
python3 - "$LOGDIR/emu68.log" "$CHECK" <<'EOF'
import re, sys
log = open(sys.argv[1], 'rb').read().decode('latin-1')
check = sys.argv[2] == '1'
lines = [l for l in log.splitlines() if l.startswith('[MAC] JITROM: ')]
bad = 0
for l in lines:
    print(l)
    m = re.search(r'\(expect ([0-9A-F]{8})\) ([0-9A-F]{8})$', l)
    if m and m.group(1) != m.group(2):
        bad += 1; print('  ^ wrong')
    if l.startswith('[MAC] JITROM: 5 '):
        want = '00000002' if check else '00000001'
        if not l.endswith(want):
            bad += 1; print('  ^ wrong (%s expected %s mac.jitcheck=1)' % (want, 'with' if check else 'without'))
if not any(l.startswith('[MAC] JITROM: done') for l in lines):
    bad += 1; print('the program did not finish (log: %s)' % sys.argv[1])
for l in log.splitlines():
    if l.startswith('[PSMAC:JIT]'):
        print(l)
print('jitrom: %s' % ('passed' if not bad else '%d wrong' % bad))
sys.exit(1 if bad else 0)
EOF
