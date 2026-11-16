# Mac-side pieces for PiStorm-SE: 68k assembly (GNU binutils for m68k) packaged with tools/mac/*.py.
#   make            builds build/PiDisk.bin (MacBinary INIT with the .PiDisk DRVR)
#   make install IMG=path/to/volume.img [PART=n]   copies the INIT into the System Folder of an HFS image
AS      = m68k-linux-gnu-as -m68020
OBJCOPY = m68k-linux-gnu-objcopy
PY      = python3
B       = build

all: $(B)/PiDisk.bin $(B)/PiNet.bin $(B)/psmac.declrom

$(B)/%.o: %.s
	@mkdir -p $(B)
	$(AS) -o $@ $<

$(B)/%.bin.raw: $(B)/%.o
	$(OBJCOPY) -O binary -j .text $< $@

$(B)/PiDisk.bin: $(B)/pidisk.bin.raw $(B)/pidisk_init.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkinit.py --drvr $(B)/pidisk.bin.raw --init $(B)/pidisk_init.bin.raw --name PiDisk -o $@

$(B)/PiNet.bin: $(B)/pinet.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/pinet.bin.raw --name PiNet --creator PINT -o $@

$(B)/psmac.declrom: $(B)/pidisk.bin.raw $(B)/enet.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --enet $(B)/enet.bin.raw -o $@

disasm: $(B)/pidisk.o
	m68k-linux-gnu-objdump -d $(B)/pidisk.o | head -200

install: $(B)/PiDisk.bin $(B)/PiNet.bin $(B)/psmac.declrom
	$(PY) ../tools/mac/hfsput.py $(IMG) $(B)/PiDisk.bin "System Folder" $(PART)

clean:
	rm -rf $(B)
