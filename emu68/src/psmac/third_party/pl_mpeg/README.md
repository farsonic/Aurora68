# pl_mpeg (vendored, unmodified)

`pl_mpeg.h` is the MPEG-1 video decoder, MP2 audio decoder and MPEG program stream demultiplexer of
https://github.com/phoboslab/pl_mpeg by Dominic Szablewski, taken verbatim on 22 November 2026 at commit
`c871f2be022ece7ef4f64230b4fb8e1fb9eb6023` (2025-12-30, "Fix corrupt slice check; close #64"; sha256 of the
file `3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9`). The file declares itself
`SPDX-License-Identifier: MIT` and the project's README calls it MIT licensed; the repository carries no
separate licence file, so `LICENSE` beside it is the standard MIT text with the author the file names.

It is the only third-party code in the video card's path (docs/16-video-card.md). The kernel compiles it
once, in `../psmac_plmpeg.c`, with `PLM_NO_STDIO` and the kernel's allocator, and calls it from
`../psmac_video.c` on core 1. To take a newer pl_mpeg, replace `pl_mpeg.h`, update the commit and checksum
here, rebuild the kernel and play a test movie in the testbed (`mac.videotest=`, docs/08 item 98).
