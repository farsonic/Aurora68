/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/piinput_test.c - .PiInput (mac/piinput.s) on a 68020 under qemu-m68k: events put into its ring as the kernel
 * puts them (alice_psmac.c), the driver's own drain routine run over them, and what it did to the Mac's low memory and
 * which events it posted checked. tools/piinput_m68k.sh assembles the driver with its low memory moved into lowmem[]
 * here and _PostEvent replaced by record_post. Written on 24 November 2026, docs/08 item 139: the owner's SE behaved
 * "like I have some key held down" (every Finder click a Shift- or Command-click) after a Bluetooth keyboard had been
 * used, Aurora68 PC's Control-Option-Command-P above all.
 *
 *   - Shift (and Command, Option, Control) let go after the key it was held for: out of KeyMap at once (before, the
 *     bit stayed until the driver's next posted event, and the SE's own mouse's clicks carried it);
 *   - a modifier the SE's own (ADB) keyboard holds in KeyMap is not taken away by a click from a Pi mouse;
 *   - Caps Lock's KeyMap bit follows its toggle at once;
 *   - mouse buttons: one mouseDown for the first button down, one mouseUp for the last up, however the bits change in
 *     between; the right button's Command is in KeyMap from the press to the release; mac.rightbutton=ctrl and none;
 *   - the feedback block (alice_psmac.h struct alice_input_fb) says what was posted, with MBState and the modifiers.
 */
#include "play_core_shim.h"
#include <stdint.h>
#include "alice_psmac.h"

unsigned char lowmem[0x1000] __attribute__((aligned(4)));
static unsigned char ring[4096] __attribute__((aligned(4)));
static unsigned char globals[256] __attribute__((aligned(4)));
extern const long t_offsets[7];
enum { O_RING, O_MODS, O_BUTTONS, O_CAPS, O_KMOWN, O_BTNMODS, O_SIZE };
extern void drain(void);

static int checks, fails;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, what); } } while (0)

/* _PostEvent as the driver calls it: what it posted, and the modifiers the ROM's would have read (KeyMap+6, MBState) */
static struct { long what, msg; unsigned kmw; unsigned mb; } posted[64];
static int nposted;
void record_post(long what, long msg)
{
	if (nposted < 64) { posted[nposted].what = what; posted[nposted].msg = msg; posted[nposted].kmw = (unsigned)(lowmem[0x17a] << 8 | lowmem[0x17b]); posted[nposted].mb = lowmem[0x172]; }
	nposted++;
}

static volatile struct alice_input_ring *R = (volatile struct alice_input_ring *)ring;
static volatile struct alice_input_fb *F = (volatile struct alice_input_fb *)(ring + ALICE_IN_FB_OFF);
static void put(uint32_t type, int32_t a, int32_t b, int32_t c)
{
	volatile struct alice_input_event *e = &R->ev[R->head % ALICE_IN_SLOTS];
	e->type = type; e->a = a; e->b = b; e->c = c;
	R->head++;
}
static void run(void)
{
	register void *a2 __asm__("a2") = globals;
	__asm__ volatile("jsr drain" : : "a"(a2) : "d0", "d1", "a0", "a1", "memory", "cc");
}
static uint16_t gw(int o) { return (uint16_t)(globals[t_offsets[o]] << 8 | globals[t_offsets[o] + 1]); }
static void reset(uint32_t options)
{
	memset(lowmem, 0, sizeof lowmem); memset(ring, 0, sizeof ring); memset(globals, 0, sizeof globals);
	R->magic = ALICE_IN_MAGIC; R->slots = ALICE_IN_SLOTS; R->options = options;
	*(uint32_t *)(globals + t_offsets[O_RING]) = (uint32_t)(uintptr_t)ring;
	globals[t_offsets[O_BUTTONS]] = 0x80;                /* "unknown", as Open leaves it */
	lowmem[0x172] = 0x80;                                /* MBState: up */
	*(uint16_t *)(lowmem + 0x834 + 4) = 342; *(uint16_t *)(lowmem + 0x834 + 6) = 512;   /* CrsrPin 0,0,342,512 */
	nposted = 0;
}
/* KeyMap's modifier keys: Command (code 0x37) byte 6 bit 7; Shift 0x38, Caps Lock 0x39, Option 0x3A, Control 0x3B: byte 7 bits 0-3 */
static int km(int code) { return (lowmem[0x174 + (code >> 3)] >> (code & 7)) & 1; }
#define SHIFT 0x38
#define CMD   0x37
#define CAPS  0x39
#define OPT   0x3a
#define CTRL  0x3b

int main(void)
{
	if (t_offsets[O_SIZE] > (long)sizeof globals) { printf("piinput_test: globals larger than the test's block\n"); return 1; }

	/* Shift held for a capital A, let go after the A: KeyMap's Shift goes with it */
	reset(0);
	put(ALICE_IN_KEY, 0, 0, 0x02);                       /* Shift down (the modifier byte alone) */
	run();
	CHECK(km(SHIFT), "Shift down on a Pi keyboard: in KeyMap at once");
	put(ALICE_IN_KEY, 0x04, 1, 0x02); put(ALICE_IN_KEY, 0x04, 0, 0x02);   /* A down, up, Shift still held */
	run();
	CHECK(nposted == 2 && posted[0].what == 3 && posted[1].what == 4, "keyDown and keyUp posted");
	CHECK(nposted >= 1 && (posted[0].msg & 0xff) == 'A' && (posted[0].kmw & 1), "a capital A, with Shift in KeyMap");
	put(ALICE_IN_KEY, 0, 0, 0x00);                       /* Shift up */
	run();
	CHECK(!km(SHIFT), "Shift let go after the letter: out of KeyMap (it stayed before 24 November 2026)");
	CHECK(gw(O_KMOWN) == 0, "nothing left that the driver owns");

	/* Aurora68 PC's capture key, Control-Option-Command-P, as a Bluetooth keyboard reports it: P let go first */
	reset(0);
	put(ALICE_IN_KEY, 0, 0, 0x01); put(ALICE_IN_KEY, 0, 0, 0x05); put(ALICE_IN_KEY, 0, 0, 0x0d);   /* Control, Option, Command */
	put(ALICE_IN_KEY, 0x13, 1, 0x0d); put(ALICE_IN_KEY, 0x13, 0, 0x0d);                            /* P down, up */
	run();
	CHECK(km(CTRL) && km(OPT) && km(CMD), "Control, Option and Command held: in KeyMap");
	put(ALICE_IN_KEY, 0, 0, 0x0c); put(ALICE_IN_KEY, 0, 0, 0x08); put(ALICE_IN_KEY, 0, 0, 0x00);
	run();
	CHECK(!km(CTRL) && !km(OPT) && !km(CMD), "all three let go: none left in KeyMap");
	put(ALICE_IN_MOUSE, 0, 0, 1); put(ALICE_IN_MOUSE, 0, 0, 0);
	nposted = 0; run();
	CHECK(nposted == 2 && posted[0].what == 1 && posted[0].kmw == 0, "the next click is a plain one");

	/* the SE's own keyboard holds Shift (the ROM's ADB driver set KeyMap's bit): a click with a Pi mouse keeps it */
	reset(0);
	lowmem[0x174 + (SHIFT >> 3)] |= 1 << (SHIFT & 7);
	put(ALICE_IN_MOUSE, 0, 0, 1); put(ALICE_IN_MOUSE, 0, 0, 0);
	run();
	CHECK(km(SHIFT), "the ADB keyboard's Shift is not taken away by a Pi mouse's click");
	CHECK(nposted == 2 && (posted[0].kmw & 1), "the click is a Shift-click, as the user holds it");
	put(ALICE_IN_KEY, 0, 0, 0x02); put(ALICE_IN_KEY, 0, 0, 0x00);   /* a Pi keyboard's Shift pressed and let go meanwhile */
	run();
	CHECK(km(SHIFT), "nor by a Pi keyboard's Shift let go: it was not that keyboard's");

	/* Caps Lock: its KeyMap bit follows the toggle at once */
	reset(0);
	put(ALICE_IN_KEY, 0x39, 1, 0); put(ALICE_IN_KEY, 0x39, 0, 0);
	run();
	CHECK(km(CAPS) && nposted == 0, "Caps Lock on: in KeyMap, nothing posted");
	put(ALICE_IN_KEY, 0x39, 1, 0); put(ALICE_IN_KEY, 0x39, 0, 0);
	run();
	CHECK(!km(CAPS), "Caps Lock off: out of KeyMap");

	/* the buttons: left, right added and let go, left up: one mouseDown and one mouseUp */
	reset(0);
	put(ALICE_IN_MOUSE, 0, 0, 1); put(ALICE_IN_MOUSE, 2, 0, 3); put(ALICE_IN_MOUSE, 2, 0, 1); put(ALICE_IN_MOUSE, 0, 0, 0);
	run();
	CHECK(nposted == 2 && posted[0].what == 1 && posted[1].what == 2, "one mouseDown, one mouseUp (it was three downs before)");
	CHECK(nposted == 2 && posted[0].mb == 0x00 && posted[1].mb == 0x80, "MBState down, then up");
	CHECK(lowmem[0x172] == 0x80 && !km(CMD), "after it: MBState up, no Command");
	/* right button: Command from the press to the release */
	reset(0);
	put(ALICE_IN_MOUSE, 0, 0, 2);
	run();
	CHECK(nposted == 1 && posted[0].what == 1 && (posted[0].kmw & 0x8000) && km(CMD), "right button: a Command-click, Command in KeyMap while held");
	put(ALICE_IN_KEY, 0x04, 1, 0); put(ALICE_IN_KEY, 0x04, 0, 0);
	run();
	CHECK(km(CMD), "a key meanwhile does not take the button's Command away");
	put(ALICE_IN_MOUSE, 0, 0, 0);
	run();
	CHECK(!km(CMD) && lowmem[0x172] == 0x80 && gw(O_BTNMODS) == 0, "released: Command out, MBState up");
	/* mac.rightbutton=ctrl, none */
	reset(ALICE_IN_OPT_RBCTRL);
	put(ALICE_IN_MOUSE, 0, 0, 2); run();
	CHECK(km(CTRL) && !km(CMD), "mac.rightbutton=ctrl: a Control-click");
	put(ALICE_IN_MOUSE, 0, 0, 0); run();
	CHECK(!km(CTRL), "and Control out again");
	reset(ALICE_IN_OPT_RBNONE);
	put(ALICE_IN_MOUSE, 0, 0, 2); run();
	CHECK(nposted == 1 && posted[0].what == 1 && posted[0].kmw == 0, "mac.rightbutton=none: a plain click");
	put(ALICE_IN_MOUSE, 0, 0, 0); run();
	/* the middle button: Option */
	reset(0);
	put(ALICE_IN_MOUSE, 0, 0, 4); run();
	CHECK(km(OPT), "the middle button: an Option-click");
	put(ALICE_IN_MOUSE, 0, 0, 0); run();
	CHECK(!km(OPT), "and Option out again");
	/* a movement without button information (-1) changes nothing */
	reset(0);
	put(ALICE_IN_MOUSE, 5, 5, 1); put(ALICE_IN_MOUSE, 5, 5, -1); run();
	CHECK(nposted == 1 && lowmem[0x172] == 0x00, "a movement without buttons keeps the button down");
	CHECK((int16_t)(lowmem[0x82e] << 8 | lowmem[0x82f]) == 10, "RawMouse moved by both");

	/* the feedback block */
	reset(0);
	put(ALICE_IN_MOUSE, 0, 0, 2); run();
	{
		uint32_t v = F->res[0], k = F->km[0];
		CHECK((v >> 24) == 1 && ((v >> 16) & 0xff) == 0x00 && (v & 0x100), "feedback: mouseDown, MBState 00, Command asked for");
		CHECK((k >> 16) & 0x8000 && (k & 0x100), "feedback: Command in KeyMap, the driver's own");
		CHECK(F->taken == 1 && F->buttons == 2 && F->btnmods == 0x100, "feedback: one taken, the buttons, their Command");
	}
	put(ALICE_IN_KEY, 0, 0, 0x02); run();
	CHECK((F->res[1] >> 24) == 0 && F->mods == 0x200 && (F->km[1] >> 16 & 1), "feedback: a modifier alone, nothing posted, Shift in KeyMap");

	printf("piinput_test (68020): %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
