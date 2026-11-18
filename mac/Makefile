# Mac-side pieces for PiStorm-SE: 68k assembly (GNU binutils for m68k) packaged with tools/mac/*.py.
#   make            builds the drivers, the declaration ROM, the PiDisk extension and the PiNet, PiWiFi, PiBlue, PiStatus, PiPing and PiLog applications
#   The declaration ROM (build/psmac.declrom) carries the .PiDisk, .Network_EtherNet_PiStorm and .PiInput drivers.
#   make check     compares the struct offsets in enet.s with emu68/src/alice/alice_net.h
#   make install IMG=path/to/volume.img [PART=n]   copies the INIT into the System Folder of an HFS image
AS      = m68k-linux-gnu-as -m68020
OBJCOPY = m68k-linux-gnu-objcopy
PY      = python3
B       = build

all: $(B)/PiMach.bin $(B)/PiDisk.bin $(B)/PiNet.bin $(B)/PiWiFi.bin $(B)/PiBlue.bin $(B)/PiStatus.bin $(B)/PiPing.bin $(B)/PiScreens.bin $(B)/PiColor.bin $(B)/PiLog.bin $(B)/PiDepth.bin $(B)/WiFiMenu.bin $(B)/psmac.declrom $(B)/psmac_wifi.declrom $(B)/psmac_wifi_video.declrom declrom-sizes

$(B)/%.o: %.s
	@mkdir -p $(B)
	$(AS) -o $@ $<

$(B)/%.bin.raw: $(B)/%.o
	$(OBJCOPY) -O binary -j .text $< $@

$(B)/PiDisk.bin: $(B)/pidisk.bin.raw $(B)/pidisk_init.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkinit.py --drvr $(B)/pidisk.bin.raw --init $(B)/pidisk_init.bin.raw --name PiDisk -o $@

$(B)/PiMach.bin: $(B)/pimach.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkinit.py --init $(B)/pimach.bin.raw --name PiMach -o $@

# the same extension answering another machine type (for experiments): make pimach-variant MACH=11
pimach-variant: pimach.s
	@mkdir -p $(B)
	$(AS) --defsym MACH=$(MACH) -o $(B)/pimach$(MACH).o pimach.s
	m68k-linux-gnu-objcopy -O binary -j .text $(B)/pimach$(MACH).o $(B)/pimach$(MACH).bin.raw
	$(PY) ../tools/mac/mkinit.py --init $(B)/pimach$(MACH).bin.raw --name PiMach -o $(B)/PiMach$(MACH).bin

$(B)/PiNet.bin: $(B)/pinet.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/pinet.bin.raw --name PiNet --creator PINT -o $@

$(B)/PiWiFi.bin: $(B)/piwifi.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piwifi.bin.raw --name PiWiFi --creator PIWF -o $@

$(B)/PiBlue.bin: $(B)/piblue.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piblue.bin.raw --name PiBlue --creator PIBL -o $@

$(B)/PiStatus.bin: $(B)/pistatus.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/pistatus.bin.raw --name PiStatus --creator PIST -o $@

$(B)/PiScreens.bin: $(B)/piscreens.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piscreens.bin.raw --name PiScreens --creator PISC -o $@

$(B)/PiColor.bin: $(B)/picolor.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/picolor.bin.raw --name PiColor --creator PICO -o $@

# PiPing's peer: the address it pings (the testbed's host peer by default; make PEER=192.168.0.1 for a real gateway)
PEER ?= 192.168.68.1
PEER_HEX = $(shell printf '0x%02x%02x%02x%02x' $(subst ., ,$(PEER)))
.PHONY: peer-inc
$(B)/piping_peer.inc: peer-inc | $(B)
	@printf '\t.byte %d\n\t.ascii "Ping %s ... "\n' $(shell printf 'Ping %s ... ' "$(PEER)" | wc -c) "$(PEER)" > $@
$(B)/piping.o: piping.s $(B)/piping_peer.inc
	$(AS) --defsym PEER_IP=$(PEER_HEX) -I$(B) -o $@ $<
$(B)/PiPing.bin: $(B)/piping.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/mkapp.py --code $(B)/piping.bin.raw --name PiPing --creator PIPG -o $@

# the WiFi driver: the Ethernet source assembled for unit 0
$(B)/enet_wifi.o: enet.s
	@mkdir -p $(B)
	$(AS) --defsym WIFI=1 -o $@ $<

$(B)/psmac.declrom: $(B)/pidisk.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw -o $@

# the same card for other screen sizes: the kernel takes psmac-WxH.declrom from the container when the
# second display has that size (mac.screen=WxH, or the VideoCore's own size)
RESES = 512x342 800x600 1024x768 1280x720 1280x1024 1920x1080
declrom-sizes: $(B)/pidisk.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw
	for r in $(RESES); do $(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw --res $$r -o $(B)/psmac-$$r.declrom; done

# the same card with the WiFi as its only network sResource: MacTCP and EtherTalk take the first
# Ethernet-type sResource they find, so this is how the WiFi path is tested in the testbed
# the network is the WiFi only (a Pi with no wired Ethernet, the bench): MacTCP's Ethernet link layer takes
# the first network card it finds, so the wired sResource must be absent for it to take the WiFi (docs/09)
$(B)/psmac_wifi_video.declrom: $(B)/pidisk.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw -o $@
$(B)/psmac_wifi.declrom: $(B)/pidisk.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw -o $@

# compare the struct offsets in enet.s with the header the kernel compiles (tools/mac/an_offsets.c)
check: ../tools/mac/an_offsets.c ../tools/mac/check_offsets.py ../emu68/src/alice/alice_net.h
	@mkdir -p $(B)
	cc -I ../emu68/src/alice -o $(B)/an_offsets ../tools/mac/an_offsets.c
	$(PY) ../tools/mac/check_offsets.py enet.s $(B)/an_offsets

disasm: $(B)/pidisk.o
	m68k-linux-gnu-objdump -d $(B)/pidisk.o | head -200

# ---- C programs (mac/c/README.md): PiLog ----
CC      = m68k-linux-gnu-gcc
LD      = m68k-linux-gnu-ld
CFLAGS  = -m68020 -O2 -std=gnu99 -ffreestanding -fno-builtin -fno-pic -fno-pie -ffixed-a5 \
          -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter --param=min-pagesize=0 -Ic -I$(B)
LIBGCC  = $(shell $(CC) -m68020 -print-libgcc-file-name)
C_KIT   = $(B)/crt0.o $(B)/toolbox_glue.o $(B)/libc.o
$(B)/toolbox_glue.s $(B)/toolbox_gen.h: c/traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)
	$(PY) ../tools/mac/mktraps.py c/traps.txt -s $(B)/toolbox_glue.s -H $(B)/toolbox_gen.h
$(B)/crt0.o: c/crt0.s
	@mkdir -p $(B)
	$(AS) -o $@ $<
$(B)/toolbox_glue.o: $(B)/toolbox_glue.s
	$(AS) -o $@ $<
$(B)/libc.o: c/libc.c c/toolbox.h $(B)/toolbox_gen.h
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/pilog.o: pilog.c c/toolbox.h $(B)/toolbox_gen.h
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/pilog.elf: $(C_KIT) $(B)/pilog.o c/app.ld
	$(LD) -T c/app.ld --emit-relocs --no-warn-rwx-segments -z noexecstack -o $@ $(C_KIT) $(B)/pilog.o $(LIBGCC)
$(B)/pidepth.o: pidepth.c c/toolbox.h $(B)/toolbox_gen.h
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/pidepth.elf: $(C_KIT) $(B)/pidepth.o c/app.ld
	$(LD) -T c/app.ld --emit-relocs --no-warn-rwx-segments -z noexecstack -o $@ $(C_KIT) $(B)/pidepth.o $(LIBGCC)
$(B)/PiDepth.bin: $(B)/pidepth.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/elf2app.py $< --name "PiDepth 8" --creator PIDP --size 64 -o $@
$(B)/PiLog.bin: $(B)/pilog.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/elf2app.py $< --name PiLog --creator PILG --size 256 -o $@

# ---- the WiFi menu: an INIT in C (mac/wifimenu, tools/mac/elf2init.py) ----
$(B)/wifimenu_traps.txt: c/traps.txt wifimenu/traps.txt
	@mkdir -p $(B)
	cat c/traps.txt wifimenu/traps.txt > $@
$(B)/wm/toolbox_glue.s $(B)/wm/toolbox_gen.h: $(B)/wifimenu_traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)/wm
	$(PY) ../tools/mac/mktraps.py $< -s $(B)/wm/toolbox_glue.s -H $(B)/wm/toolbox_gen.h
$(B)/wm/entry.o: wifimenu/entry.s
	@mkdir -p $(B)/wm
	$(AS) -o $@ $<
$(B)/wm/toolbox_glue.o: $(B)/wm/toolbox_glue.s
	$(AS) -o $@ $<
$(B)/wm/wifimenu.o: wifimenu/wifimenu.c wifimenu/menus.h c/toolbox.h $(B)/wm/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/wm,$(CFLAGS)) -c -o $@ $<
$(B)/wm/btmenu.o: wifimenu/btmenu.c wifimenu/menus.h c/toolbox.h $(B)/wm/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/wm,$(CFLAGS)) -c -o $@ $<
$(B)/wm/libc.o: c/libc.c c/toolbox.h $(B)/wm/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/wm,$(CFLAGS)) -c -o $@ $<
$(B)/wifimenu.elf: $(B)/wm/entry.o $(B)/wm/toolbox_glue.o $(B)/wm/wifimenu.o $(B)/wm/btmenu.o $(B)/wm/libc.o wifimenu/init.ld
	$(LD) -T wifimenu/init.ld --emit-relocs --gc-sections --no-warn-rwx-segments -z noexecstack -o $@ $(B)/wm/entry.o $(B)/wm/toolbox_glue.o $(B)/wm/wifimenu.o $(B)/wm/btmenu.o $(B)/wm/libc.o $(LIBGCC)
$(B)/WiFiMenu.bin: $(B)/wifimenu.elf ../tools/mac/elf2init.py ../tools/mac/macelf.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/elf2init.py $< --name "Wi-Fi & Bluetooth" --creator PSWM -o $@

install: $(B)/PiMach.bin $(B)/PiDisk.bin $(B)/PiNet.bin $(B)/PiWiFi.bin $(B)/PiBlue.bin $(B)/PiStatus.bin $(B)/PiPing.bin $(B)/PiScreens.bin $(B)/PiColor.bin $(B)/psmac.declrom
	$(PY) ../tools/mac/hfsput.py $(IMG) $(B)/PiDisk.bin "System Folder" $(PART)
	$(PY) ../tools/mac/hfsput.py $(IMG) $(B)/PiMach.bin "System Folder" $(PART)

clean:
	rm -rf $(B)
