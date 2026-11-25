# SPDX-License-Identifier: MPL-2.0
"""sexpr.py - read and write KiCad's S-expression files, standard library only.

KiCad's files are nested lists of atoms; an atom is a bare symbol (layer, yes, smd), a number, or a quoted
string. Here a list is a Python list, a bare symbol a Sym (a str subclass), a quoted string a plain str and
a number an int or float. dump() writes them back the way KiCad 8 indents its own files: a list whose
elements are all atoms on one line, anything else one child per line, tab-indented.
"""

import re


class Sym(str):
    """A bare (unquoted) atom."""
    __slots__ = ()


_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)


def parse(text):
    """Parse one top-level S-expression."""
    stack, cur, pos = [], None, 0
    root = None
    n = len(text)
    while pos < n:
        m = _TOKEN.match(text, pos)
        if not m:
            if text[pos:].strip() == '':
                break
            raise ValueError('bad S-expression at %d: %r' % (pos, text[pos:pos + 40]))
        pos = m.end()
        if m.group(1):
            new = []
            if cur is not None:
                cur.append(new)
                stack.append(cur)
            cur = new
        elif m.group(2):
            if cur is None:
                raise ValueError('unbalanced ) at %d' % pos)
            if stack:
                cur = stack.pop()
            else:
                root, cur = cur, None
        elif m.group(3) is not None:
            cur.append(m.group(3).replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\'))
        else:
            tok = m.group(4)
            try:
                v = int(tok) if re.fullmatch(r'-?\d+', tok) else float(tok) if re.fullmatch(r'-?\d*\.\d+(e-?\d+)?|-?\d+e-?\d+', tok) else Sym(tok)
            except ValueError:
                v = Sym(tok)
            cur.append(v)
    if cur is not None:
        raise ValueError('unbalanced ( at end')
    return root


def load(path):
    with open(path, encoding='utf-8') as f:
        return parse(f.read())


def fmt_atom(a):
    if isinstance(a, Sym):
        return str(a)
    if isinstance(a, bool):
        return 'yes' if a else 'no'
    if isinstance(a, int):
        return str(a)
    if isinstance(a, float):
        s = ('%.6f' % a).rstrip('0').rstrip('.')
        return '0' if s in ('-0', '') else s
    s = str(a).replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    return '"%s"' % s


def dump(node, indent=0):
    if not isinstance(node, list):
        return fmt_atom(node)
    if all(not isinstance(c, list) for c in node):
        return '(' + ' '.join(fmt_atom(c) for c in node) + ')'
    tabs = '\t' * indent
    head = []
    i = 0
    while i < len(node) and not isinstance(node[i], list):
        head.append(fmt_atom(node[i]))
        i += 1
    out = '(' + ' '.join(head)
    for c in node[i:]:
        out += '\n' + tabs + '\t' + dump(c, indent + 1)
    out += '\n' + tabs + ')'
    return out


def save(path, node):
    with open(path, 'w', encoding='utf-8') as f:
        f.write(dump(node) + '\n')


def find(node, key):
    """The first child list whose head is key."""
    for c in node:
        if isinstance(c, list) and c and c[0] == key:
            return c
    return None


def find_all(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def S(*items):
    """Build a list, turning leading str heads into Syms: S('at', 1, 2) -> (at 1 2)."""
    out = []
    for k, it in enumerate(items):
        if k == 0 and isinstance(it, str) and not isinstance(it, Sym):
            out.append(Sym(it))
        else:
            out.append(it)
    return out
