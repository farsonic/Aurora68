#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# stop-aux-net.sh: end a run of run-aux-net.sh early, and only that: QEMU first, then MAME; the script then copies
# its snapshots and gives the lock back by itself.
desc() { local p; for p in $(pgrep -P "$1"); do echo "$p"; desc "$p"; done; }
for r in $(pgrep -f "run-aux-net.sh"); do
  for p in $(desc "$r"); do case "$(ps -o comm= -p "$p")" in qemu-system-aar*) kill "$p";; esac; done
  sleep 2
  for p in $(desc "$r"); do case "$(ps -o comm= -p "$p")" in mamepsmac*|psmac*) kill "$p";; esac; done
  sleep 2
  for p in $(desc "$r"); do case "$(ps -o comm= -p "$p")" in mamepsmac*|psmac*) kill -9 "$p";; esac; done
done
