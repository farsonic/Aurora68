# Berkeley SoftFloat 3e

The exact FPU of the Macintosh build (`../../psmac_fpu.c`, issue #27, `docs/17-exact-fpu.md`) does its
arithmetic with John R. Hauser's Berkeley SoftFloat, Release 3e (2018 January 20).

- **Source:** http://www.jhauser.us/arithmetic/SoftFloat-3e.zip, fetched 22 November 2026,
  SHA-256 `21130ce885d35c1fe73fc1e1bf2244178167e05c6747cad5f450cc991714c746`.
- **Licence:** BSD 3-clause, `COPYING.txt` (unchanged from the package). `README.txt` is the package's own.

## What is here
- Every `.c` file of the package's `source/` directory that its 64-bit GCC build
  (`build/Linux-x86_64-GCC/Makefile`, `SOFTFLOAT_FAST_INT64`) compiles, except the 16-bit half-precision
  ones and the `extF80M_*`/`f128M_*` pointer forms, which the FPU does not use: 178 files, **unmodified**.
  `files.cmake` lists them for the kernel's build (a static library, so only what is called is linked)
  and for `tools/fpu_test.sh`.
- `include/`: the package's headers, unmodified.
- `platform.h`: written here (SoftFloat expects each build to supply one): the options of the 64-bit GCC
  build, with `LITTLEENDIAN` taken from the compiler, since the kernel is big-endian and the host tests
  are not.
- `68881/`: the "specialization" (SoftFloat's word for the choices IEEE 754 leaves to the machine),
  made from the package's `8086-SSE` one, as the Motorola 68881 and 68882 make them. The differences:
  - `specialize.h`: the default NaN is the 68881's, every fraction bit set and the sign clear
    (`7FFF FFFFFFFF FFFFFFFF` in extended; `7FFFFFFF` single, `7FFFFFFFFFFFFFFF` double, likewise in
    128 bits); tininess is detected before rounding; a positive overflow converting to an integer gives
    the largest positive integer.
  - `s_propagateNaNExtF80UI.c`, `s_propagateNaNF128UI.c`, `s_propagateNaNF64UI.c`,
    `s_propagateNaNF32UI.c`: when either operand is a NaN, the first one (the 68k instruction's
    destination) is returned if it is a NaN, else the other, quieted; a signalling operand raises the
    invalid flag (which the FPU reports as SNAN, not OPERR). The 8086 rule (the larger significand) is
    not the 68881's.
  - the other files of `68881/` are `8086-SSE`'s, unmodified.

The changes to the specialization were made by `tools/softfloat-68881.py`, so that they can be applied
again to a later SoftFloat: it replaces the defaults named above and the bodies of the four propagation
functions, and adds a comment to `specialize.h`.

## Building
The kernel's `CMakeLists.txt` builds these files as the library `softfloat` with `-mgeneral-regs-only`
(core 0's floating-point registers are the 68k's) and with three warnings the package gives quieted
(`-Wno-unused-label -Wno-implicit-fallthrough -Wno-sign-compare`), so that it is compiled exactly as its
author wrote it.

## Updating
Fetch the newer package, copy the same set of files over these (the list in `files.cmake`), compare the
new `8086-SSE` specialization with the old one and carry the changes above into `68881/`, then run
`tools/fpu_test.sh` (the arithmetic against an x86-64's x87 in every rounding mode and precision, and the
elementary functions against 80-digit decimal arithmetic).
