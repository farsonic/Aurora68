# Prior art (cloned under reference/)
- michalsc/Emu68 (MPL-2.0) — JIT 68040, bare metal; output `Emu68.img` (we rename to kernel8.img). No MMU emulation; Amiga map hardcoded in ps_protocol.c / M68k_Translator.c; AGENTS.md rules make Mac support a fork, not a PR.
- captain-amygdala/pistorm (MIT) — classic CPLD/GPIO protocol (ps_protocol.h): REG_DATA/ADDR_LO/ADDR_HI/STATUS, IPL via status bits, no BR/BG/BGACK, FC lines not handled.
- Frimples/pistorm-macintosh-se — Linux+Musashi SE core; ROM presented at 0 during reset; write-through only for video/sound (3x speed); virtual NCR 5380 target (`macse_scsi.c`, Lido formats, HD SC Setup rejects); 24-bit mask patch for 020/030; latest (Nov 2026, Discord): 68040 + 128 MB + Quadra 900 donor ROM + grey dithering.
- Frimples/MacSE-PiStorm-DirectSocket — EPM240 RTL with bus arbitration + /VPA; hardware validation pending.
- Frimples/MACSE_ROMINATOR — SE ROM with checksum disabled + bbraun ROM-disk driver appended (unfinished).
- Tinkerdifferent threads: "PiStorm on the Macintosh SE" (2085), "ROMINATOR ROM mod for SE" (2109).
- BMOW ROM-inator / bbraun plusrom: ROM disk driver model for a ROM DRVR + boot hook.
- Apple: Guide to Macintosh Family Hardware 2nd ed., Designing Cards and Drivers 2nd ed. (SE PDS pinout, video declaration ROMs, .ENET driver API).
