#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mkpage.py - turn tools/recovery/page.html into emu68/src/psmac/psmac_recovery_page.h.

22 November 2026. Aurora68 Recovery's web page (psmac_recovery.c serves it at /) was a C string literal
written by hand; to make it a tabbed page in a modern style it moved into page.html, edited as HTML, and
this script makes the header the kernel includes. Run it after editing the page:

    python3 tools/recovery/mkpage.py

Standard library only. The output is one C string, split into lines of the page, with backslashes, double
quotes and non-ASCII characters escaped (the page is UTF-8; the kernel serves the bytes as they are).
"""
import os, sys

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, 'page.html')
dst = os.path.join(here, '..', '..', 'emu68', 'src', 'psmac', 'psmac_recovery_page.h')

def c_line(b):
    out = []
    for ch in b:
        if ch == 0x5c: out.append('\\\\')
        elif ch == 0x22: out.append('\\"')
        elif ch == 0x0a: out.append('\\n')
        elif ch == 0x09: out.append('\\t')
        elif 0x20 <= ch < 0x7f: out.append(chr(ch))
        else: out.append('\\%03o' % ch)       # octal escapes cannot run on into the next character as \x can
    return '"' + ''.join(out) + '"'

data = open(src, 'rb').read()
lines = data.split(b'\n')
body = '\n'.join('    ' + c_line(l + b'\n') for l in lines if l or l is not lines[-1])
hdr = ('/* SPDX-License-Identifier: MPL-2.0\n'
       ' * psmac_recovery_page.h - Aurora68 Recovery\'s web page, made from tools/recovery/page.html by\n'
       ' * tools/recovery/mkpage.py. Do not edit: edit the page and run the script. */\n'
       'const char recovery_page[] =\n' + body + ';\n')
open(dst, 'w').write(hdr)
print(f'{dst}: {len(data)} bytes of page')
