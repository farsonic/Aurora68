# psmac — downstream fork of Emu68 for the Macintosh (Aurora68 project)

This tree is a fork of michalsc/Emu68 (MPL-2.0). It targets a Macintosh SE fitted with a
PiStorm, booting Apple ROMs (Mac SE 24-bit, Mac IIci 32-bit donor). It is NOT intended to be
merged upstream: it deliberately adds things upstream excludes (a Macintosh host memory map,
a 68030 MMU shadowing shim, write-through video RAM, a bridge transport for a MAME-hosted
motherboard, and a service core providing a mailbox for storage/network). Upstream's
contribution policy is kept verbatim in AGENTS.upstream.md for reference; it does not govern
this fork. Keep all fork-specific code under src/psmac/ and behind the PSMAC build variant so
the JIT core stays mergeable with upstream changes.
Licence: modified files remain MPL-2.0 and are published with the project.
