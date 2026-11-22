# dr_flac (vendored, unmodified)

`dr_flac.h` is the FLAC decoder of https://github.com/mackron/dr_libs by David Reid, taken verbatim on
22 November 2026 from the release tag `flac-0.13.4` (commit `806ee9ada8ca9827b22f76827f739c290f37ba48`,
the file's header says "dr_flac - v0.13.4 - 2026-10-31"; sha256 of the file
`b8078eeb5d523502eb7964508cd7ec8450bc79d383bec90aa17fd9e2fefd9c24`). Its author offers it under the
choice of the public domain (the Unlicense) or MIT No Attribution; the text is in `LICENSE` beside it,
copied from the end of the file.

The kernel compiles it once, in `../psmac_flac.c`, with `DR_FLAC_NO_SIMD` (its NEON code assumes
little-endian lanes, and the kernel is big-endian), without stdio, wide characters or Ogg-FLAC, and with
its allocations going through the kernel's TLSF; the stream channel calls it on core 1 (`../psmac_audio.c`,
`../psmac_codec.h`). To take a newer dr_flac, replace `dr_flac.h` and `LICENSE`, update the tag, commit and
checksum here, then run `make -C mac check` (tools/codec_test.sh compares its output with ffmpeg's, sample
for sample) and play the test files in the testbed (docs/09, "Aurora68 Play").
