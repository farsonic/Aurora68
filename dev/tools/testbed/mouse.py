#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mouse.py: move and click the testbed's Mac's pointer through QEMU's monitor (its USB mouse).

    mouse.py click 200 4                  the pointer to x 200, y 4 of the Pi's screen, then a click
    mouse.py at 330 253 down wait 1 move 0 52 up    press, drag 52 pixels down, release
    mouse.py dclick 40 60                 a double click
    mouse.py drag 100 100 300 200         press at one place, release at another
    mouse.py home                         the pointer to the top left corner (0, 0)

Commands, run in order: home; at X Y (home, then to X Y); move DX DY (relative); click [X Y];
dclick [X Y]; down; up; wait SECONDS; or any monitor command in quotes ("mouse_button 1").

QEMU's USB mouse is relative, so "at" first pushes the pointer into the top left corner and then
moves it in steps of 8 pixels, which the Mac's mouse acceleration takes one for one; positions are
then the Pi's screen pixels as dev/tools/testbed/shot.sh shows them. The monitor is
/tmp/qemu-mon.sock, or QEMU_MON.
"""
import os, socket, sys, time

STEP = 8


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        sys.exit(__doc__)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.environ.get('QEMU_MON', '/tmp/qemu-mon.sock'))
    s.settimeout(0.05)

    def drain():
        try:
            while s.recv(65536):
                pass
        except OSError:
            pass

    def cmd(c, pause=0.12):
        s.sendall((c + '\n').encode())
        time.sleep(pause)
        drain()

    def move(dx, dy):
        n = max(1, (max(abs(dx), abs(dy)) + STEP - 1) // STEP)
        sx = sy = 0
        for i in range(1, n + 1):
            tx, ty = dx * i // n, dy * i // n
            cmd('mouse_move %d %d' % (tx - sx, ty - sy), 0.06)
            sx, sy = tx, ty

    def at(x, y):
        move(-4000, -3000)
        move(x, y)

    def click(times=1):
        for _ in range(times):
            cmd('mouse_button 1', 0.1)
            cmd('mouse_button 0', 0.1)

    drain()
    i = 0
    nums = lambda k: [int(v) for v in argv[i + 1:i + 1 + k]]
    while i < len(argv):
        w = argv[i]
        if w == 'home':
            move(-4000, -3000); i += 1
        elif w == 'at':
            at(*nums(2)); i += 3
        elif w == 'move':
            move(*nums(2)); i += 3
        elif w in ('click', 'dclick'):
            n = 1 if w == 'click' else 2
            if len(argv) > i + 2 and all(v.isdigit() for v in argv[i + 1:i + 3]):
                at(*nums(2)); i += 2
            click(n); i += 1
        elif w == 'drag':
            x0, y0, x1, y1 = nums(4)
            at(x0, y0); cmd('mouse_button 1', 0.3); move(x1 - x0, y1 - y0); cmd('mouse_button 0', 0.3); i += 5
        elif w == 'down':
            cmd('mouse_button 1'); i += 1
        elif w == 'up':
            cmd('mouse_button 0'); i += 1
        elif w == 'wait':
            time.sleep(float(argv[i + 1])); i += 2
        else:
            cmd(w); i += 1


if __name__ == '__main__':
    main(sys.argv[1:])
