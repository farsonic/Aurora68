#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# shell.sh: a shell, or one command, in the workspace's build container.
#
# usage: dev/shell.sh                 an interactive shell in /work (the workspace)
#        dev/shell.sh COMMAND...      run COMMAND there, in the folder that matches the current one
#        dev/shell.sh --stop          stop and remove the container (the workspace is not touched)
#   env: WORKSPACE (default: the folder above this Aurora68 checkout), IMAGE (aurora68-dev),
#        NAME (the container's name, aurora68)
#
# The container is started once and kept: `docker run -d ... sleep infinity`, then `docker exec`
# for every shell or command. It sees the workspace at /work (a bind mount: the files are the
# Mac's, edited from either side), keeps its home folder (Claude Code's login and settings) in
# the volume aurora68-home, and reaches the LAN through Docker's own routing (Docker Desktop and
# OrbStack both route a container's outbound traffic to the Mac's LAN, so the Pi at 192.168.x.y
# answers from inside). `--init` reaps the QEMUs and pinet peers the testbed starts.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
WORKSPACE=${WORKSPACE:-$(cd "$here/../.." && pwd)}
IMAGE=${IMAGE:-aurora68-dev}
NAME=${NAME:-aurora68}

if [ "${1:-}" = --stop ]; then docker rm -f "$NAME" >/dev/null && echo "stopped $NAME"; exit 0; fi

if [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" != true ]; then
	docker rm -f "$NAME" >/dev/null 2>&1 || true
	docker run -d --init --name "$NAME" --hostname aurora68-dev \
		-v "$WORKSPACE":/work -v aurora68-home:/home/dev \
		-w /work "$IMAGE" sleep infinity >/dev/null
	echo "started container $NAME ($IMAGE), $WORKSPACE at /work" >&2
fi

# the folder inside that matches the current one outside, if it is in the workspace
case "$PWD/" in
"$WORKSPACE"/*) rel=${PWD#"$WORKSPACE"}; dir=/work$rel ;;
*) dir=/work ;;
esac
tty=; [ -t 0 ] && [ -t 1 ] && tty=-it
if [ $# -eq 0 ]; then
	exec docker exec $tty -w "$dir" "$NAME" bash -l
else
	exec docker exec $tty -w "$dir" "$NAME" bash -lc '"$@"' bash "$@"
fi
