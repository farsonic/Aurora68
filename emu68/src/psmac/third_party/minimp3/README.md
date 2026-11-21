# minimp3 (vendored, unmodified)

`minimp3.h` is the MPEG audio decoder (layers I, II and III) of https://github.com/lieff/minimp3 by
lieff, taken verbatim on 21 November 2026 at commit `142823796ab8efa55e1bf11406028f3be77ecceb`
(sha256 of the file `f9592be69e8b94044e9da6f45f993c9708000abae38cf6f44b70c699de55b509`). Its author
dedicated it to the public domain under CC0 1.0 Universal; the text is in `LICENSE` beside it.

It is the only third-party code in PiPlay's path. The kernel compiles it once, in `../psmac_mp3.c`, with
`MINIMP3_NO_SIMD` (the scalar code: see that file for why) and calls `mp3dec_init` and
`mp3dec_decode_frame` from the sound card's stream channel in `../psmac_audio.c` on core 1. To take a
newer minimp3, replace `minimp3.h` and `LICENSE`, update the commit and checksum here, rebuild the
kernel and play the test files of `tools/mksounds.py` in the testbed (docs/09, "PiPlay").
