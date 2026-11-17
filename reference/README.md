# reference/: third-party trees used by the project

Only two things in this directory are committed: `mame-psmac.patch`, the project's changes to
MAME (the IIci machine with a bridged CPU and the bridge device, docs/06-mame-bridge.md), and
`mame-src/`, unmodified copies of the MAME 0.264 sources the machine inherits from
(`maciici.cpp`, `macadb.cpp`, `macrtc.cpp`, `asc.cpp`), kept so that a future MAME version can
be compared against what the bridge was written for. Everything else is recreated:

| Directory | Source | Why it is here |
|---|---|---|
| `mame/` | `git clone --branch mame0264 --depth 1 https://github.com/mamedev/mame`, then `git apply ../mame-psmac.patch` and the build in CLAUDE.md | the testbed motherboard (`mamepsmac maciici_ps`) and `chdman` |
| `Emu68/` | `https://github.com/michalsc/Emu68` | upstream of the kernel fork in `emu68/`; for diffs and merges |
| `Alice68/` | `https://github.com/farsonic/Alice68` | the service modules in `emu68/src/alice/` come from here, and the Amiga programs (Alice68-Card, Alice68-Network) are the models for the Macintosh ones (docs/11) |
| `pistorm/`, `pistorm-macintosh-se/`, `MacSE-PiStorm-DirectSocket/`, `pistorm-macse-scsi-addon/`, `macse-24bit-addressing/`, `MACSE_ROMINATOR/` | the PiStorm projects for the SE and the SE ROM work that came before (docs/02-prior-art.md names them) | hardware facts, bus protocol, the SE's address map |
