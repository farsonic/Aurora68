#!/bin/sh
# Build the psmac MAME subtarget (stock maciici + maciici_ps bridge machine).
# Usage: tools/build_mame_psmac.sh [extra make args]      JOBS=-j2 to lower memory use
# Genie names a SOURCES build after the subtarget: the binary is reference/mame/psmac;
# a mamepsmac symlink is added for convenience.
set -e
cd "$(dirname "$0")/../reference/mame"
make SUBTARGET=psmac \
	SOURCES=src/mame/apple/maciici.cpp,src/mame/apple/maciici_ps.cpp \
	REGENIE=1 NOWERROR=1 TOOLS=0 ${JOBS:--j4} "$@"
ln -sf psmac mamepsmac
echo "built: $(pwd)/psmac (symlink mamepsmac)"
