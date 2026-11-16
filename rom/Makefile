# ROM patch build.  Run from the repository root with `make -C rom` or from here.
# The original images in original/ are never modified.
#
#   make            build patched/MacIIci_psmac.rom and patched/MacSE_psmac.rom
#   make iici       build only the IIci ROM
#   make se         build only the SE ROM
#   make mame-lanes LANEDIR=../mame/roms_patched/maciici [ROM=patched/MacIIci_psmac.rom]
#                   split a (patched) IIci ROM into the 4 byte-lane files MAME wants
#   make check      dry-run both patch sets against the originals (verifies "expect")
#   make test       build the IIci calibration fall-through test ROMs (patched + unpatched,
#                   VIA T2 period stretched so MAME's 68030 hits the 65536-iteration limit)
#                   and split them into ../mame/roms_test_patched / roms_test_unpatched
#   make clean

PYTHON   ?= python3
ROMPATCH  = $(PYTHON) ../tools/rompatch.py
ROMLANES  = $(PYTHON) ../tools/romlanes.py

IICI_ORIG = original/MacIIci_368CADFE.rom
SE_ORIG   = original/MacSE_B2E362A8.rom
IICI_OUT  = patched/MacIIci_psmac.rom
SE_OUT    = patched/MacSE_psmac.rom

# Defaults for mame-lanes
ROM     ?= $(IICI_OUT)
LANEDIR ?= ../mame/roms_patched/maciici

TEST_P    = patched/MacIIci_test_slowtimer.rom
TEST_U    = patched/MacIIci_test_slowtimer_unpatched.rom

.PHONY: all iici se mame-lanes check test clean

all: iici se
iici: $(IICI_OUT)
se: $(SE_OUT)

$(IICI_OUT): $(IICI_ORIG) patches/iici.json ../tools/rompatch.py
	$(ROMPATCH) patches/iici.json $(IICI_ORIG) $@

$(SE_OUT): $(SE_ORIG) patches/se.json ../tools/rompatch.py
	$(ROMPATCH) patches/se.json $(SE_ORIG) $@

mame-lanes: $(ROM)
	$(ROMLANES) $(ROM) $(LANEDIR)

test: $(TEST_P) $(TEST_U)
	$(ROMLANES) $(TEST_P) ../mame/roms_test_patched/maciici
	$(ROMLANES) $(TEST_U) ../mame/roms_test_unpatched/maciici

$(TEST_P): $(IICI_ORIG) patches/iici.json patches/iici-test-slowtimer.json ../tools/rompatch.py
	$(ROMPATCH) patches/iici-test-slowtimer.json $(IICI_ORIG) $@

$(TEST_U): $(IICI_ORIG) patches/iici-test-slowtimer-unpatched.json ../tools/rompatch.py
	$(ROMPATCH) patches/iici-test-slowtimer-unpatched.json $(IICI_ORIG) $@

check:
	$(ROMPATCH) --dry-run patches/iici.json $(IICI_ORIG) /dev/null
	$(ROMPATCH) --dry-run patches/se.json $(SE_ORIG) /dev/null

clean:
	rm -f $(IICI_OUT) $(SE_OUT) $(TEST_P) $(TEST_U)
