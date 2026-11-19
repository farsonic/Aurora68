# SPDX-License-Identifier: MPL-2.0
#
# The Aurora68 workspace's build environment: Ubuntu 24.04 with the cross compilers, QEMU 8.2 and
# the disk tools the four repositories use, as on the machine the work was done on. The
# repositories are not in the image: they live on the host and are bind-mounted at /work
# (dev/README.md). The image runs natively on Apple Silicon (linux/arm64) and on Intel (linux/amd64).
#
#   docker build -t aurora68-dev dev                                   the base
#   docker build -t aurora68-dev --build-arg EXTRAS="--servers --mame-deps --claude" dev
#
# EXTRAS are dev/setup-linux.sh options: --servers (Samba for Lagoon's tests), --mame-deps (the
# libraries for building MAME; MAME itself is built into the workspace with
# `dev/setup-linux.sh --mame /work/pistorm-se`), --amiga (vbcc and the NDK, amd64 only),
# --claude (Claude Code, for running it inside the container). BASE picks the base image (a
# registry mirror when Docker Hub refuses).
ARG BASE=ubuntu:24.04
FROM ${BASE}

ARG EXTRAS=""
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 TZ=UTC

COPY setup-linux.sh /usr/local/bin/aurora68-setup
# the "ubuntu" user of the base image (uid 1000, the uid Docker Desktop and OrbStack map bind
# mounts to) becomes "dev", with sudo: Claude Code does not run as root with its permission
# checks off, and a build as root leaves root-owned files in the workspace
RUN aurora68-setup $(echo "$EXTRAS" | sed 's/--claude//') \
 && usermod -l dev -d /home/dev -m ubuntu && groupmod -n dev ubuntu \
 && echo 'dev ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/dev && chmod 440 /etc/sudoers.d/dev \
 && apt-get clean && rm -f /var/lib/apt/lists/*_Packages /var/lib/apt/lists/*_Release /var/lib/apt/lists/*_InRelease

USER dev
WORKDIR /work
RUN git config --global --add safe.directory '*' \
 && git config --global init.defaultBranch main \
 && case "$EXTRAS" in *--claude*) curl -fsSL https://claude.ai/install.sh | bash ;; esac \
 && echo 'export PATH=$HOME/.local/bin:$PATH' >> /home/dev/.bashrc
ENV PATH=/home/dev/.local/bin:$PATH
CMD ["bash", "-l"]
