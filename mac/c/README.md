# C programs for the classic Mac OS (mac/c)

The Mac-side drivers and the first applications in `mac/` are 68k assembly. Programs with real
user interfaces are easier in C, so this directory holds a small kit that turns ordinary
`m68k-linux-gnu-gcc` output into a classic Mac application, with no Mac compiler, no Apple
headers and no Retro68. PiLog (`mac/pilog.c`) is the first program built with it.

| File | What |
|---|---|
| `traps.txt` | The Toolbox calls the programs use, one line each: trap word, result kind, C prototype with a Pascal kind per argument |
| `../../tools/mac/mktraps.py` | Turns `traps.txt` into C-callable glue (`build/toolbox_glue.s`) and prototypes (`build/toolbox_gen.h`) |
| `toolbox.h` | The few structures (Rect, Point, EventRecord, TERec), constants and low-memory globals, plus the generated prototypes |
| `crt0.s` | The entry point: relocation, cache flush, `MaxApplZone`, the managers' Init calls, `main()`, `ExitToShell`. Also Pascal-to-C callback stubs and `mac_newptr` |
| `app.ld` | The link: everything at 0, `crt0.o` first, the BSS inside the image |
| `libc.c` | `memcpy`, `memset` and friends (GCC calls them by itself), and `pstr()` (a C string as a Pascal one) |
| `../../tools/mac/elf2app.py` | The ELF as an application: CODE 0 (jump table, room for QuickDraw's globals below A5), CODE 1 (the image and its relocation table), SIZE -1, any extra resources; a MacBinary file for `hcopy -m` |

## How it works

**No position-independent code.** The program is compiled `-fno-pic` and linked at address 0
with `--emit-relocs`. `elf2app.py` copies `.text`, `.data` and the zeroed `.bss` into one image
and appends a table of every 32-bit absolute field (an `R_68K_32` relocation against a section).
At startup `crt0.s` adds the image's load address to each field and flushes the instruction
cache (`_HWPriv` 1). The Segment Loader keeps CODE 1 locked while the program runs, and the
program never calls `UnloadSeg`, so the fixed-up image stays put. Riptide's `.IPP` driver uses
the same scheme for its `'DRVR'` (riptide `tools/elf2mac.py`).

**The Pascal calling convention.** Toolbox traps take their arguments on the stack, left to
right, with room for the result pushed first, and pop them themselves. GCC pushes right to left,
each argument in a 4-byte slot. Each generated glue routine re-pushes its arguments the Pascal
way: a long or pointer as 4 bytes, an INTEGER or CHAR as a word, a BOOLEAN as a byte in the high
half of a word. It then issues the trap word and returns the result in D0: a BOOLEAN as 0 or 1,
an INTEGER sign-extended. A pointer result goes in **A0 as well**, because GCC's m68k SVR4 ABI
reads pointer results from A0 (the first PiLog hung because NewWindow's result came back only in
D0). The glue keeps D2, which GCC expects kept and Pascal code may change.

**A5.** QuickDraw finds its globals through A5, so everything is compiled `-ffixed-a5`.

**Callbacks.** Routines the Toolbox calls (a control's action procedure) are Pascal routines.
`crt0.s` has small stubs that turn such a call into a C call and pop the Pascal arguments
afterwards. `pas_ctl_action` calls the program's `c_ctl_action(ctl, part)`.

**Structures.** m68k GCC aligns every field to 2 bytes, as the Toolbox's Pascal records do, so
`toolbox.h` declares them plainly. Static asserts check `TERec` and `EventRecord`.

## Adding a call

Add a line to `traps.txt`, e.g.

```
A9D2 l TENew(const Rect *dest:l, const Rect *view:l)
```

The trap word comes from Inside Macintosh. The result kind is `l`, `w`, `b` or `-` (none).
Each argument gets a kind after a colon. Register-based OS traps (NewPtr, FlushEvents) do not
fit this form and get hand-written glue in `crt0.s`.

## Building a program

See the PiLog rules in `mac/Makefile`: compile with the flags there (`-m68020 -O2 -ffreestanding
-fno-builtin -fno-pic -ffixed-a5`), link `crt0.o toolbox_glue.o libc.o` and the program with
`app.ld` and libgcc, then run `elf2app.py --name NAME --creator CODE`.

## Testing in the testbed

`tools/run-solo.sh` boots System 7.1 under QEMU with the program in the image (put it in
`System Folder:Startup Items` to have it start at boot). `mac.inputclick=SECONDS,X,Y[,o]` clicks
at a point of the main screen, as an Option-click with `,o`, and `SHOT=` takes screenshots.
