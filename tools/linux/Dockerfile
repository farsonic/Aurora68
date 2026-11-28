# SPDX-License-Identifier: MPL-2.0
#
# The build container for tools/linux/build-debian-image.sh (28 November 2026, docs/27-linux.md): Debian 13 with
# the m68k cross compiler for the Linux kernel, debootstrap and Debian Ports' signing keys for the root file
# system, the static qemu-m68k that runs the m68k programs during debootstrap's second stage, and e2fsprogs to
# make the ext4 image without a loop device. The same Dockerfile builds natively on arm64 (an Apple Silicon Mac
# under OrbStack or Docker Desktop) and on amd64.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        bc bison build-essential ca-certificates cpio curl debian-ports-archive-keyring debootstrap e2fsprogs \
        file flex gcc-m68k-linux-gnu binutils-m68k-linux-gnu kmod libelf-dev libssl-dev openssl patch \
        python3 qemu-user-static rsync xz-utils \
 && apt-get clean \
 && rm -f /var/lib/apt/lists/*_Packages /var/lib/apt/lists/*_Sources

# the steps themselves are tools/linux/build-in-container.sh, mounted at /aurora68 with the configuration and the
# patches when the container runs, so that a change to them needs no new image
WORKDIR /work
