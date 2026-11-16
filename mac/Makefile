# Mac-side pieces for PiStorm-SE: 68k assembly (GNU binutils for m68k) packaged with tools/mac/*.py.
#   make            builds the drivers, the declaration ROM, the PiDisk extension and the PiNet, PiWiFi, PiBlue, PiStatus and PiPing applications
#   The declaration ROM (build/psmac.declrom) carries the .PiDisk, .Network_EtherNet_PiStorm and .PiInput drivers.
#   make check     compares the struct offsets in enet.s with emu68/src/alice/alice_net.h
#   make install IMG=path/to/volume.img [PART=n]   copies the INIT into the System Folder of an HFS image
AS      = m68k-linux-gnu-as -m68020
OBJCOPY = m68k-linux-gnu-objcopy
PY      = python3
B       = build

all: $(B)/PiDisk.bin $(B)/PiNet.bin $(B)/PiWiFi.bin $(B)/PiBlue.bin $(B)/PiStatus.bin $(B)/PiPing.bin $(B)/psmac.declrom

$(B)/%.o: %.s
	@mkdir -p $(B)
	$(AS) -o $@ $<

$(B)/%.bin.raw: $(B)/%.o
	$(OBJCOPY) -O binary -j .text $< $@

$(B)/PiDisk.bin: $(B)/pidisk.bin.raw $(B)/pidisk_init.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkinit.py --drvr $(B)/pidisk.bin.raw --init $(B)/pidisk_init.bin.raw --name PiDisk -o $@

$(B)/PiNet.bin: $(B)/pinet.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/pinet.bin.raw --name PiNet --creator PINT -o $@

$(B)/PiWiFi.bin: $(B)/piwifi.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piwifi.bin.raw --name PiWiFi --creator PIWF -o $@

$(B)/PiBlue.bin: $(B)/piblue.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piblue.bin.raw --name PiBlue --creator PIBL -o $@

$(B)/PiStatus.bin: $(B)/pistatus.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/pistatus.bin.raw --name PiStatus --creator PIST -o $@

$(B)/PiPing.bin: $(B)/piping.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piping.bin.raw --name PiPing --creator PIPG -o $@

# the WiFi driver: the Ethernet source assembled for unit 0
$(B)/enet_wifi.o: enet.s
	@mkdir -p $(B)
	$(AS) --defsym WIFI=1 -o $@ $<

$(B)/psmac.declrom: $(B)/pidisk.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw -o $@

# compare the struct offsets in enet.s with the header the kernel compiles (tools/mac/an_offsets.c)
check: ../tools/mac/an_offsets.c ../tools/mac/check_offsets.py ../emu68/src/alice/alice_net.h
	@mkdir -p $(B)
	cc -I ../emu68/src/alice -o $(B)/an_offsets ../tools/mac/an_offsets.c
	$(PY) ../tools/mac/check_offsets.py enet.s $(B)/an_offsets

disasm: $(B)/pidisk.o
	m68k-linux-gnu-objdump -d $(B)/pidisk.o | head -200

install: $(B)/PiDisk.bin $(B)/PiNet.bin $(B)/PiWiFi.bin $(B)/PiBlue.bin $(B)/PiStatus.bin $(B)/PiPing.bin $(B)/psmac.declrom
	$(PY) ../tools/mac/hfsput.py $(IMG) $(B)/PiDisk.bin "System Folder" $(PART)

clean:
	rm -rf $(B)
