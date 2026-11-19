#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""keys.py: type into the testbed's Mac through QEMU's monitor (its USB keyboard).

    keys.py "Hello from the Mac." @ret          text, then Return
    keys.py @cmd-o                             Command-O (the Mac's Command key is QEMU's meta_l)
    keys.py @cmd-shift-s @esc @tab @up @f1     any key or chord; @opt- and @ctrl- for Option, Control
    keys.py --delay 0.3 "slowly"               seconds between keys (default 0.15)

A word starting with @ is one key or chord in QEMU's sendkey names ("@shift-right", "@pgup",
"@end", "@kp_decimal"); cmd, opt and option are shorthands for meta_l and alt. Everything else is
typed character by character, capitals and shifted punctuation with Shift, as a US keyboard
types them (the Mac's KCHR does the rest). The monitor is /tmp/qemu-mon.sock, or QEMU_MON.

Keys reach the Mac only once .PiInput runs (the Finder is up) and only with a USB keyboard on
QEMU's bus: dev/tools/testbed/boot.sh adds one unless --no-input.
"""
import os, socket, sys, time

PLAIN = {' ': 'spc', '\n': 'ret', '\t': 'tab', '.': 'dot', ',': 'comma', '-': 'minus', '=': 'equal',
         '/': 'slash', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent',
         '[': 'bracket_left', ']': 'bracket_right'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9',
           ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash',
           ':': 'semicolon', '"': 'apostrophe', '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave_accent'}
ALIAS = {'cmd': 'meta_l', 'command': 'meta_l', 'opt': 'alt', 'option': 'alt', 'return': 'ret', 'enter': 'ret',
         'escape': 'esc', 'space': 'spc', 'delete': 'backspace', 'del': 'delete'}


def chord(k):
    return '-'.join(ALIAS.get(p.lower(), p) for p in k.split('-'))


def keyname(c):
    if c in PLAIN:
        return PLAIN[c]
    if c in SHIFTED:
        return 'shift-' + SHIFTED[c]
    if c.isupper():
        return 'shift-' + c.lower()
    if c.isalnum() and c.isascii():
        return c
    sys.exit('keys.py: no key for %r (use @name)' % c)


def main(argv):
    delay = 0.15
    if argv[:1] == ['--delay']:
        delay = float(argv[1]); argv = argv[2:]
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

    def send(k):
        s.sendall(('sendkey %s\n' % k).encode())
        time.sleep(delay)
        drain()

    drain()
    for a in argv:
        if a.startswith('@') and len(a) > 1:
            send(chord(a[1:]))
        else:
            for c in a:
                send(keyname(c))


if __name__ == '__main__':
    main(sys.argv[1:])
