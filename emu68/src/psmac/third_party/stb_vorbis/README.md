# stb_vorbis (vendored, unmodified)

`stb_vorbis.c` is the Ogg Vorbis decoder of https://github.com/nothings/stb by Sean Barrett and
contributors, version 1.25 (its first line), taken verbatim on 22 November 2026 at commit
`6b5799bae8a1fe3792691ef4b72ae26434c3e799` (sha256 of the file
`97a8febc738e6a29fafc8d8bc557c7f17ad3db7ba6544485f59395cc6008b764`). Its authors offer it under the
choice of the MIT licence or the public domain (the Unlicense); the text is in `LICENSE` beside it, copied
from the end of the file.

The kernel compiles it once, in `../psmac_ogg.c`: the pushdata interface only (no stdio, no pull-data
interface, no integer conversion), its memory from one buffer the glue gives it (so it never calls malloc
and needs no alloca on core 1's stack), the kernel's musl math functions (src/math) for its setup, and its
asserts compiled out. It decodes in single-precision floating point on core 1, as minimp3 does.

Known and harmless: UBSan on the host reports a signed shift in its CRC handling
(`data[i+25]<<24`, line 4515), which GCC and AArch64 evaluate as the intended bits; the file is kept as it
is. To take a newer stb_vorbis, replace `stb_vorbis.c` and `LICENSE`, update the version, commit and
checksum here, then run `make -C mac check` (tools/codec_test.sh compares it with ffmpeg's decoding) and
play the test files in the testbed (docs/09, "Aurora68 Play").
