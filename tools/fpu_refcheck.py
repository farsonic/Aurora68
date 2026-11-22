#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""fpu_refcheck.py - the exact FPU's elementary functions against 80-digit decimal arithmetic.

22 November 2026 (issue #27). tools/fpu_test.c compares FSIN, FETOX and the rest with the host C library's
long double functions, which are themselves off by up to three units in the last place; that bounds the
exact FPU's error but does not say how good it is. `fpu_test --dump FILE` writes each argument and result;
this script recomputes every one with Python's decimal module at 80 significant digits (pi by Machin's
formula, the series written out here), rounds to the extended format (64-bit significand, to nearest) and
counts the results that are not the correctly rounded ones, and by how much.

    python3 tools/fpu_refcheck.py FILE        prints a line per function; exit status 1 if any is off by an ulp or more

Standard library only.
"""
import sys
from decimal import Decimal as D, getcontext

getcontext().prec = 90


def machin_pi():
    getcontext().prec += 10
    def atan_inv(k):
        x = D(1) / k
        x2 = x * x
        s, t, n, sign = x, x, 1, 1
        while True:
            t *= x2
            n += 2
            sign = -sign
            d = t / n
            if d < D(10) ** -(getcontext().prec + 2):
                break
            s += sign * d
        return s
    p = 4 * (4 * atan_inv(5) - atan_inv(239))
    getcontext().prec -= 10
    return +p


PI = machin_pi()
LN2 = D(2).ln()
LN10 = D(10).ln()


def sin_cos(x):
    """(sin x, cos x) by reduction mod pi/2 at this precision and the Taylor series"""
    getcontext().prec += 20 + max(0, int(abs(x).adjusted()))
    k = (x / (PI / 2)).to_integral_value()
    r = x - k * (PI / 2)
    r2 = r * r
    s, t, n = r, r, 1
    while True:
        t = -t * r2 / ((n + 1) * (n + 2))
        n += 2
        if t == 0 or abs(t) < D(10) ** -(getcontext().prec):
            break
        s += t
    c, t, n = D(1), D(1), 0
    while True:
        t = -t * r2 / ((n + 1) * (n + 2))
        n += 2
        if t == 0 or abs(t) < D(10) ** -(getcontext().prec):
            break
        c += t
    q = int(k) % 4
    sn, cs = [(s, c), (c, -s), (-s, -c), (-c, s)][q]
    getcontext().prec = 90
    return +sn, +cs


def atan(x):
    neg = x < 0
    a = abs(x)
    inv = a > 1
    if inv:
        a = 1 / a
    for _ in range(4):
        a = a / (1 + (1 + a * a).sqrt())
    a2 = a * a
    s, t, n, sign = a, a, 1, 1
    while True:
        t *= a2
        n += 2
        sign = -sign
        d = t / n
        if abs(d) < D(10) ** -95:
            break
        s += sign * d
    r = 16 * s
    if inv:
        r = PI / 2 - r
    return -r if neg else r


FUNCS = {
    'FSIN': lambda x: sin_cos(x)[0],
    'FCOS': lambda x: sin_cos(x)[1],
    'FTAN': lambda x: (lambda s, c: s / c)(*sin_cos(x)),
    'FATAN': atan,
    'FASIN': lambda x: atan(x / (1 - x * x).sqrt()) if abs(x) < 1 else (PI / 2 if x > 0 else -PI / 2),
    'FACOS': lambda x: PI / 2 - (atan(x / (1 - x * x).sqrt()) if abs(x) < 1 else (PI / 2 if x > 0 else -PI / 2)),
    'FETOX': lambda x: x.exp(),
    'FETOXM1': lambda x: x.exp() - 1 if abs(x) > D('1e-30') else x + x * x / 2,
    'FTWOTOX': lambda x: (x * LN2).exp(),
    'FTENTOX': lambda x: (x * LN10).exp(),
    'FLOGN': lambda x: x.ln(),
    'FLOG10': lambda x: x.ln() / LN10,
    'FLOG2': lambda x: x.ln() / LN2,
    'FLOGNP1': lambda x: (1 + x).ln(),
    'FSINH': lambda x: (x.exp() - (-x).exp()) / 2,
    'FCOSH': lambda x: (x.exp() + (-x).exp()) / 2,
    'FTANH': lambda x: (x.exp() - (-x).exp()) / (x.exp() + (-x).exp()),
    'FATANH': lambda x: ((1 + x) / (1 - x)).ln() / 2,
}


def value(se, m):
    e = se & 0x7fff
    if e == 0:
        e = 1                                    # SoftFloat's (and the x87's) convention, which the dump uses
    v = D(m) * D(2) ** (e - 16383 - 63)
    return -v if se & 0x8000 else v


def ulps(v, se, m):
    """how far the extended value (se, m) is from v, in units of its last place"""
    if v == 0:
        return 0 if m == 0 else 1e9
    got = value(se, m)
    a = abs(v)
    e = a.adjusted()                             # decimal exponent, to start the binary one near
    b = int(e * 3.321928) - 2
    while D(2) ** (b + 1) <= a:
        b += 1
    while D(2) ** b > a:
        b -= 1
    ulp = D(2) ** max(b - 63, -16445)
    return float(abs(got - v) / ulp)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.split('\n\n')[-2])
    stats = {}
    for line in open(sys.argv[1]):
        name, xse, xm, rse, rm = line.split()
        x = value(int(xse, 16), int(xm, 16))
        if name not in FUNCS:
            continue
        try:
            v = FUNCS[name](x)
        except Exception:
            continue
        u = ulps(v, int(rse, 16), int(rm, 16))
        s = stats.setdefault(name, [0, 0, 0.0])
        s[0] += 1
        if u > 0.5 + 1e-9:
            s[1] += 1
        s[2] = max(s[2], u)
    bad = 0
    for name, (n, off, worst) in stats.items():
        print(f'{name:8} {n:5} results, {off:3} not correctly rounded, worst {worst:.3f} ulp')
        if worst >= 1.0:
            bad = 1
    sys.exit(bad)


if __name__ == '__main__':
    main()
