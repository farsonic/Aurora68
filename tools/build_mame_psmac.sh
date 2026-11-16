#!/bin/sh
# Build the psmac MAME subtarget (maciici + maciici_ps bridge machine).
# Usage: tools/build_mame_psmac.sh [extra make args]   (e.g. -j2)
set -e
cd "$(dirname "$0")/../reference/mame"
exec make SUBTARGET=psmac \
	SOURCES=src/mame/apple/maciici.cpp,src/mame/apple/maciici_ps.cpp \
	REGENIE=1 NOWERROR=1 TOOLS=0 ${JOBS:--j4} "$@"
