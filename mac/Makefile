# Mac-side pieces for Aurora68: 68k assembly (GNU binutils for m68k) packaged with tools/mac/*.py.
#   make            builds the drivers, the declaration ROMs, the extensions and the applications (build/Aurora68-*.bin:
#                   the programs are "Aurora68 Play", "Aurora68 Display"... since 21 November 2026, PiPlay, PiDisplay... before;
#                   their icons come from tools/mac/mkicons.py)
#   The declaration ROM (build/psmac.declrom) carries the .PiDisk, .Network_EtherNet_PiStorm and .PiInput drivers,
#   and PiDisk's boot record (pidisk_boot.s), which lets an image on the card be the startup disk.
#   make check     compares the struct offsets in enet.s with emu68/src/alice/alice_net.h; host tests (USB frames, EDID, PiPlay's radio parser and station list,
#                  the kernel's FAT/exFAT reader and its HFS view: tools/fatfs_test.sh, tools/hfsview_test.sh; the bus check's
#                  verdicts against a simulated board with faults: tools/buscheck_test.c)
#   make install IMG=path/to/volume.img [PART=n]   the extensions into the System Folder of an HFS image, the programs into its Aurora68 folder
AS      = m68k-linux-gnu-as -m68020
OBJCOPY = m68k-linux-gnu-objcopy
PY      = python3
B       = build
ICONS   = ../tools/mac/mkicons.py

all: $(B)/psmac-se.declrom $(B)/Aurora68-Machine.bin $(B)/Aurora68-Disk.bin $(B)/Aurora68-Network.bin $(B)/Aurora68-WiFi.bin $(B)/Aurora68-Bluetooth.bin $(B)/Aurora68-Status.bin $(B)/Aurora68-Ping.bin $(B)/Aurora68-Screens.bin $(B)/Aurora68-Colour.bin $(B)/Aurora68-Log.bin $(B)/Aurora68-Depth.bin $(B)/Aurora68-WiFi-Bluetooth.bin $(B)/Aurora68-ScreenSetup.bin $(B)/Aurora68-Display.bin $(B)/Aurora68-Sound.bin $(B)/Aurora68-Slots.bin $(B)/Aurora68-Play.bin $(B)/Aurora68-Agent.bin $(B)/psmac.declrom $(B)/psmac_wifi.declrom $(B)/psmac_wifi_video.declrom declrom-sizes declrom-wifi-sizes declrom-se-sizes

$(B)/%.o: %.s
	@mkdir -p $(B)
	$(AS) -o $@ $<

$(B)/%.bin.raw: $(B)/%.o
	$(OBJCOPY) -O binary -j .text $< $@

$(B)/Aurora68-Disk.bin: $(B)/pidisk.bin.raw $(B)/pidisk_init.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkinit.py --drvr $(B)/pidisk.bin.raw --init $(B)/pidisk_init.bin.raw --name "Aurora68 Disk" --icons disk -o $@

$(B)/Aurora68-Machine.bin: $(B)/pimach.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkinit.py --init $(B)/pimach.bin.raw --name "Aurora68 Machine" --icons machine -o $@

# the sound card as the Sound Manager 3 output device (docs/09, "Sound through HDMI"). Its Mac name was
# "Sound PiStorm", which sorts after "Sound Manager"; "Aurora68 Sound" (21 November 2026) sorts before it,
# so the extension waits for the Sound Manager 3.0 by itself (mac/pisound.s, "Waiting for the Sound Manager")
$(B)/Aurora68-Sound.bin: $(B)/pisound.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkinit.py --init $(B)/pisound.bin.raw --name "Aurora68 Sound" --icons sound -o $@

# the exact FPU's check from the 68k's side, for the testbed (issue #27; not in the release): make fputest
.PHONY: fputest fputest-speed
fputest: $(B)/Aurora68-FPUTest.bin
$(B)/fputest.o: fputest.s
	@mkdir -p $(B)
	$(AS) -m68881 -o $@ $<
fputest-speed:
	@mkdir -p $(B)
	$(AS) -m68881 --defsym SPEEDONLY=1 -o $(B)/fputest-speed.o fputest.s
	$(OBJCOPY) -O binary -j .text $(B)/fputest-speed.o $(B)/fputest-speed.bin.raw
	$(PY) ../tools/mac/mkinit.py --init $(B)/fputest-speed.bin.raw --name "Aurora68 FPU Test" --icons machine -o $(B)/Aurora68-FPUTest-speed.bin
$(B)/Aurora68-FPUTest.bin: $(B)/fputest.bin.raw ../tools/mac/mkinit.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkinit.py --init $(B)/fputest.bin.raw --name "Aurora68 FPU Test" --icons machine -o $@

# the same extension answering another machine type (for experiments): make pimach-variant MACH=11
pimach-variant: pimach.s
	@mkdir -p $(B)
	$(AS) --defsym MACH=$(MACH) -o $(B)/pimach$(MACH).o pimach.s
	m68k-linux-gnu-objcopy -O binary -j .text $(B)/pimach$(MACH).o $(B)/pimach$(MACH).bin.raw
	$(PY) ../tools/mac/mkinit.py --init $(B)/pimach$(MACH).bin.raw --name "Aurora68 Machine" --icons machine -o $(B)/Aurora68-Machine$(MACH).bin

$(B)/Aurora68-Network.bin: $(B)/pinet.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/pinet.bin.raw --name "Aurora68 Network" --creator PINT --icons network -o $@

$(B)/Aurora68-WiFi.bin: $(B)/piwifi.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/piwifi.bin.raw --name "Aurora68 Wi-Fi" --creator PIWF --icons wifi -o $@

$(B)/Aurora68-Bluetooth.bin: $(B)/piblue.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/piblue.bin.raw --name "Aurora68 Bluetooth" --creator PIBL --icons bluetooth -o $@

$(B)/Aurora68-Status.bin: $(B)/pistatus.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/pistatus.bin.raw --name "Aurora68 Status" --creator PIST --icons status -o $@

$(B)/Aurora68-Screens.bin: $(B)/piscreens.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/piscreens.bin.raw --name "Aurora68 Screens" --creator PISC --icons screens -o $@

$(B)/Aurora68-Colour.bin: $(B)/picolor.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/picolor.bin.raw --name "Aurora68 Colour" --creator PICO --icons colour -o $@

# DisposeTest: a pointer and a handle disposed of once too often above 16 MB (docs/08 item 78). A test,
# not part of `all`: it fills a few bytes of memory that are not its own for the length of one call
.PHONY: DisposeTest
DisposeTest: $(B)/DisposeTest.bin
$(B)/DisposeTest.bin: $(B)/disptest.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/disptest.bin.raw --name DisposeTest --creator PIDT -o $@

# PiPing's peer: the address it pings (the testbed's host peer by default; make PEER=192.168.0.1 for a real gateway)
PEER ?= 192.168.68.1
PEER_HEX = $(shell printf '0x%02x%02x%02x%02x' $(subst ., ,$(PEER)))
.PHONY: peer-inc
$(B)/piping_peer.inc: peer-inc | $(B)
	@printf '\t.byte %d\n\t.ascii "Ping %s ... "\n' $(shell printf 'Ping %s ... ' "$(PEER)" | wc -c) "$(PEER)" > $@
$(B)/piping.o: piping.s $(B)/piping_peer.inc
	$(AS) --defsym PEER_IP=$(PEER_HEX) -I$(B) -o $@ $<
$(B)/Aurora68-Ping.bin: $(B)/piping.bin.raw ../tools/mac/mkapp.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/mkapp.py --code $(B)/piping.bin.raw --name "Aurora68 Ping" --creator PIPG --icons ping -o $@

# the WiFi driver: the Ethernet source assembled for unit 0
$(B)/enet_wifi.o: enet.s
	@mkdir -p $(B)
	$(AS) --defsym WIFI=1 -o $@ $<

$(B)/psmac.declrom: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw -o $@

# the same card for other screen sizes: the kernel takes psmac-WxH.declrom from the container when the
# second display has that size (mac.screen=WxH, or the VideoCore's own size)
# 2560x1440 (20 November 2026): on a Pi 3 at 30 Hz only (CVT reduced blanking, hdmi_cvt in config.txt;
# 60 Hz needs a 241 MHz pixel clock, docs/10 "2560 by 1440"); its big frame buffer region is 15 MiB
RESES = 512x342 800x600 1024x768 1280x720 1280x1024 1920x1080 1920x1200 2560x1440
declrom-sizes: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw
	for r in $(RESES); do $(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw --res $$r -o $(B)/psmac-$$r.declrom; done
# and each size once more with the WiFi as the only network card, like psmac_wifi_video.declrom below: a
# Pi 3 has no driven wired Ethernet, and MacTCP and Riptide take the first Ethernet-type sResource they
# find. The size stays at the end of the name (psmac-wifi-1920x1080.declrom), where the kernel reads it
declrom-wifi-sizes: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw
	for r in $(RESES); do $(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw --res $$r -o $(B)/psmac-wifi-$$r.declrom; done

# The SE board's card with two screens (docs/10, "Two screens on the SE"): the SE's own 512x342 one-bit
# screen and an HDMI screen of each size, named psmac-se-WxH.declrom (the kernel takes the one for the
# HDMI size on mac.board=se). The HDMI screen's driver is pivideo.s assembled once more with SCREEN2.
# No WiFi-only variants: the kernel leaves the wired card out by itself where there is no wired port.
SE_RESES = 640x480 800x600 1024x768 1280x720 1280x1024 1600x1200 1920x1080 1920x1200 2560x1440
$(B)/pivideo2.o: pivideo.s
	@mkdir -p $(B)
	$(AS) --defsym SCREEN2=1 -o $@ $<
declrom-se-sizes: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw $(B)/pivideo2.bin.raw
	for r in $(SE_RESES); do $(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --se --video $(B)/pivideo.bin.raw --video2 $(B)/pivideo2.bin.raw --res $$r -o $(B)/psmac-se-$$r.declrom; done

# One two-screen file for every HDMI size (20 November 2026): the kernel rewrites its HDMI screen for the size
# config.txt's HDMI mode gives (or mac.screen=), so psmac-se.declrom (aurora68-se.declrom in the release) is all
# a container needs; the sized ones above stay for containers that name a size. Its own size, 1024x768, is
# only what it says before the kernel rewrites it.
$(B)/psmac-se.declrom: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw $(B)/pivideo2.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --enet $(B)/enet.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --se --video $(B)/pivideo.bin.raw --video2 $(B)/pivideo2.bin.raw --res 1024x768 -o $@

# the same card with the WiFi as its only network sResource: MacTCP and EtherTalk take the first
# Ethernet-type sResource they find, so this is how the WiFi path is tested in the testbed
# the network is the WiFi only (a Pi with no wired Ethernet, the bench): MacTCP's Ethernet link layer takes
# the first network card it finds, so the wired sResource must be absent for it to take the WiFi (docs/09)
$(B)/psmac_wifi_video.declrom: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw $(B)/pivideo.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw --video $(B)/pivideo.bin.raw -o $@
$(B)/psmac_wifi.declrom: $(B)/pidisk.bin.raw $(B)/pidisk_boot.bin.raw $(B)/enet_wifi.bin.raw $(B)/piinput.bin.raw ../tools/mac/mkdeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkdeclrom.py --pidisk $(B)/pidisk.bin.raw --boot $(B)/pidisk_boot.bin.raw --wifi $(B)/enet_wifi.bin.raw --input $(B)/piinput.bin.raw -o $@

# compare the struct offsets in enet.s with the header the kernel compiles (tools/mac/an_offsets.c)
# the video card's declaration ROM (docs/16-video-card.md) is compiled into the kernel rather than served
# from a file: this regenerates its header after a change to tools/mac/mkvideodeclrom.py
video-declrom: ../tools/mac/mkvideodeclrom.py ../tools/mac/declrom.py
	$(PY) ../tools/mac/mkvideodeclrom.py -o ../emu68/src/psmac/psmac_video_declrom.h
.PHONY: video-declrom

check: ../tools/mac/an_offsets.c ../tools/mac/check_offsets.py ../emu68/src/alice/alice_net.h
	@mkdir -p $(B)
	cc -I ../emu68/src/alice -o $(B)/an_offsets ../tools/mac/an_offsets.c
	$(PY) ../tools/mac/check_offsets.py enet.s $(B)/an_offsets
	cc -std=c99 -Wall -I ../emu68/src/psmac -o $(B)/usbnet_frame_test ../tools/usbnet_frame_test.c ../emu68/src/psmac/usbnet_frame.c
	$(B)/usbnet_frame_test
	cc -std=c99 -Wall -Wextra -Werror -I ../emu68/src/psmac -o $(B)/edid_test ../tools/edid_test.c ../emu68/src/psmac/psmac_edid.c
	$(B)/edid_test > $(B)/edid_test.log && tail -1 $(B)/edid_test.log
	cc -std=c99 -O1 -Wall -Wextra -Werror -I ../emu68/src/psmac -o $(B)/buscheck_test ../tools/buscheck_test.c ../emu68/src/psmac/psmac_buscheck_core.c
	$(B)/buscheck_test > $(B)/buscheck_test.log && tail -1 $(B)/buscheck_test.log
	cc -std=c99 -Wall -Wextra -Werror -o $(B)/icy_test ../tools/icy_test.c piplay/icy.c
	$(B)/icy_test
	cc -std=c99 -Wall -Wextra -Werror -o $(B)/stations_test ../tools/stations_test.c piplay/stations.c
	cc -std=c99 -Wall -Wextra -Werror -o $(B)/libparse_test ../tools/libparse_test.c piplay/libparse.c
	$(B)/stations_test
	$(B)/libparse_test
	cc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -o $(B)/devices_test ../tools/devices_test.c piplay/devices.c
	$(B)/devices_test
	sh ../tools/codec_test.sh $(abspath $(B))
	sh ../tools/fatfs_test.sh
	sh ../tools/hfsview_test.sh

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
$(B)/Aurora68-Depth.bin: $(B)/pidepth.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2app.py $< --name "Aurora68 Depth 8" --creator PIDP --size 64 --icons depth -o $@
$(B)/Aurora68-Log.bin: $(B)/pilog.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2app.py $< --name "Aurora68 Log" --creator PILG --size 256 --icons log -o $@

# ---- Aurora68 IO Test: the SE's floppy and modem port from the Mac, reported in the Pi's log (mac/iotest,
# docs/08 item 93). A test program, not in "all" nor in the release: make iotest. Its own OS-call glue
# (iotest/glue.s), so crt0.s and the other programs stay as they are.
iotest: $(B)/Aurora68-IOTest.bin
$(B)/it/glue.o: iotest/glue.s
	@mkdir -p $(B)/it
	$(AS) -o $@ $<
$(B)/it/iotest.o: iotest/iotest.c c/toolbox.h $(B)/toolbox_gen.h
	@mkdir -p $(B)/it
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/iotest.elf: $(C_KIT) $(B)/it/glue.o $(B)/it/iotest.o c/app.ld
	$(LD) -T c/app.ld --emit-relocs --no-warn-rwx-segments -z noexecstack -o $@ $(C_KIT) $(B)/it/glue.o $(B)/it/iotest.o $(LIBGCC)
$(B)/Aurora68-IOTest.bin: $(B)/iotest.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py
	$(PY) ../tools/mac/elf2app.py $< --name "Aurora68 IO Test" --creator A68T --size 1024 -o $@

# ---- PiPlay: MP3, AIFF and WAV on the sound card's stream channel, an application in C (mac/piplay) ----
# its Toolbox list is c/traps.txt and piplay/traps.txt together, its glue in build/pp/, so that the other
# C programs' binaries do not change; it includes the kernel's psmb.h for the stream block
$(B)/piplay_traps.txt: c/traps.txt piplay/traps.txt
	@mkdir -p $(B)
	cat c/traps.txt piplay/traps.txt > $@
$(B)/pp/toolbox_glue.s $(B)/pp/toolbox_gen.h: $(B)/piplay_traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)/pp
	$(PY) ../tools/mac/mktraps.py $< -s $(B)/pp/toolbox_glue.s -H $(B)/pp/toolbox_gen.h
$(B)/pp/toolbox_glue.o: $(B)/pp/toolbox_glue.s
	$(AS) -o $@ $<
$(B)/pp/fs.o: piplay/fs.s
	@mkdir -p $(B)/pp
	$(AS) -o $@ $<
$(B)/pp/libc.o: c/libc.c c/toolbox.h $(B)/pp/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -c -o $@ $<
$(B)/pp/net.o: piplay/net.s
	@mkdir -p $(B)/pp
	$(AS) -o $@ $<
PP_H = c/toolbox.h $(B)/pp/toolbox_gen.h ../emu68/src/psmac/psmb.h piplay/icy.h piplay/radio.h piplay/stations.h \
       piplay/library.h piplay/fetch.h piplay/libparse.h piplay/media.h piplay/devices.h
$(B)/pp/piplay.o: piplay/piplay.c $(PP_H)
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -I../emu68/src/psmac -c -o $@ $<
# the internet radio: Riptide's .IPP6 (IPP6.h, its interface header) or MacTCP and the resolver (radio.c), the stations' answers taken apart (icy.c, also
# built for the host by `make check`: tools/icy_test.c)
$(B)/pp/radio.o: piplay/radio.c $(PP_H) piplay/IPP6.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -I../emu68/src/psmac -c -o $@ $<
$(B)/pp/icy.o: piplay/icy.c piplay/icy.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -c -o $@ $<
# the station list: the built-in one, the file's groups (the Radio menu's submenus), the defaults a file
# lacks; also built for the host by `make check` (tools/stations_test.c)
$(B)/pp/stations.o: piplay/stations.c piplay/stations.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -c -o $@ $<
# the Library (22 November 2026, docs/08 item 98): the window, the queue and Now Playing (library.c), the second
# connection for lists and covers and the UDP probe (fetch.c), the server's answers taken apart (libparse.c, also
# built for the host by `make check`: tools/libparse_test.c)
$(B)/pp/library.o: piplay/library.c $(PP_H)
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -I../emu68/src/psmac -c -o $@ $<
$(B)/pp/fetch.o: piplay/fetch.c $(PP_H) piplay/IPP6.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -I../emu68/src/psmac -c -o $@ $<
# FLAC, Ogg Vorbis and Apple Lossless files (22 November 2026, docs/08 item 105): what a file is, its tags, the bytes
# for the card's decoders and seeking (media.c, also built for the host by `make check`: tools/codec_test.sh)
$(B)/pp/media.o: piplay/media.c piplay/media.h piplay/icy.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -c -o $@ $<
# the music server's devices (docs/08 item 105): announce, state, commands, the library's generation (devices.c, also
# built for the host by `make check`: tools/devices_test.c)
$(B)/pp/devices.o: piplay/devices.c piplay/devices.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -c -o $@ $<
$(B)/pp/libparse.o: piplay/libparse.c piplay/libparse.h
	$(CC) $(subst -I$(B),-I$(B)/pp,$(CFLAGS)) -c -o $@ $<
PP_OBJ = $(B)/crt0.o $(B)/pp/toolbox_glue.o $(B)/pp/libc.o $(B)/pp/fs.o $(B)/pp/net.o $(B)/pp/piplay.o $(B)/pp/radio.o $(B)/pp/icy.o $(B)/pp/stations.o \
         $(B)/pp/library.o $(B)/pp/fetch.o $(B)/pp/libparse.o $(B)/pp/media.o $(B)/pp/devices.o
$(B)/piplay.elf: $(PP_OBJ) c/app.ld
	$(LD) -T c/app.ld --emit-relocs --no-warn-rwx-segments -z noexecstack -o $@ $(PP_OBJ) $(LIBGCC)
# the Finder's resources (signature, bundle, file references for the application, its MP3, AIFF and WAV
# documents, its preferences and '****', icons: tools/mac/mkicons.py): files dropped on its icon reach it
# as the application's files (AppParmHandle)
$(B)/Aurora68-Play.bin: $(B)/piplay.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2app.py $< --name "Aurora68 Play" --creator PIPY --size 512 --icons play --version 1.3 -o $@

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
$(B)/Aurora68-WiFi-Bluetooth.bin: $(B)/wifimenu.elf ../tools/mac/elf2init.py ../tools/mac/macelf.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2init.py $< --name "Aurora68 Wi-Fi & Bluetooth" --creator PSWM --icons menus -o $@

# ---- the Aurora68 Agent: an INIT in C (mac/piagent), files and programs for the remote control (docs/13) ----
# entry.s carries its own glue for the File Manager, Process Manager and Apple Event calls; c/traps.txt only
# for toolbox.h's prototypes. It includes the kernel's psmb.h for the agent's block in the card window.
# 'sysz' 64 KB: System 6's system heap does not grow by itself, and the INIT (about 22 KB, loaded and then
# copied) was not run at all on System 6.0.8 without it.
$(B)/ag/toolbox_glue.s $(B)/ag/toolbox_gen.h: c/traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)/ag
	$(PY) ../tools/mac/mktraps.py $< -s $(B)/ag/toolbox_glue.s -H $(B)/ag/toolbox_gen.h
$(B)/ag/entry.o: piagent/entry.s
	@mkdir -p $(B)/ag
	$(AS) -o $@ $<
$(B)/ag/agent.o: piagent/agent.c c/toolbox.h $(B)/ag/toolbox_gen.h ../emu68/src/psmac/psmb.h
	$(CC) $(subst -I$(B),-I$(B)/ag,$(CFLAGS)) -I../emu68/src/psmac -c -o $@ $<
$(B)/ag/libc.o: c/libc.c c/toolbox.h $(B)/ag/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/ag,$(CFLAGS)) -c -o $@ $<
$(B)/piagent.elf: $(B)/ag/entry.o $(B)/ag/agent.o $(B)/ag/libc.o wifimenu/init.ld
	$(LD) -T wifimenu/init.ld --emit-relocs --gc-sections --no-warn-rwx-segments -z noexecstack -o $@ $(B)/ag/entry.o $(B)/ag/agent.o $(B)/ag/libc.o $(LIBGCC)
$(B)/Aurora68-Agent.bin: $(B)/piagent.elf ../tools/mac/elf2init.py ../tools/mac/macelf.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2init.py $< --name "Aurora68 Agent" --creator PIAG --icons agent --sysz 65536 -o $@

# ---- PiScrn: the SE board's two screens arranged at the first boot, an INIT in C (mac/piscrn) ----
$(B)/piscrn_traps.txt: c/traps.txt piscrn/traps.txt pidisplay/traps.txt
	@mkdir -p $(B)
	cat c/traps.txt piscrn/traps.txt pidisplay/traps.txt > $@
$(B)/ps/toolbox_glue.s $(B)/ps/toolbox_gen.h: $(B)/piscrn_traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)/ps
	$(PY) ../tools/mac/mktraps.py $< -s $(B)/ps/toolbox_glue.s -H $(B)/ps/toolbox_gen.h
$(B)/ps/entry.o: piscrn/entry.s
	@mkdir -p $(B)/ps
	$(AS) -o $@ $<
$(B)/ps/toolbox_glue.o: $(B)/ps/toolbox_glue.s
	$(AS) -o $@ $<
$(B)/ps/piscrn.o: piscrn/piscrn.c pidisplay/pdcommon.h c/toolbox.h $(B)/ps/toolbox_gen.h ../emu68/src/psmac/psmb.h
	$(CC) $(subst -I$(B),-I$(B)/ps,$(CFLAGS)) -Ipidisplay -I../emu68/src/psmac -c -o $@ $<
# the keep-or-revert question after a new HDMI mode (PiDisplay, docs/10): PiScrn asks it at startup
$(B)/ps/pdcommon.o: pidisplay/pdcommon.c pidisplay/pdcommon.h c/toolbox.h $(B)/ps/toolbox_gen.h ../emu68/src/psmac/psmb.h
	$(CC) $(subst -I$(B),-I$(B)/ps,$(CFLAGS)) -Ipidisplay -I../emu68/src/psmac -c -o $@ $<
$(B)/ps/dlgglue.o: pidisplay/dlgglue.s
	@mkdir -p $(B)/ps
	$(AS) -o $@ $<
$(B)/ps/libc.o: c/libc.c c/toolbox.h $(B)/ps/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/ps,$(CFLAGS)) -c -o $@ $<
$(B)/piscrn.elf: $(B)/ps/entry.o $(B)/ps/toolbox_glue.o $(B)/ps/piscrn.o $(B)/ps/pdcommon.o $(B)/ps/dlgglue.o $(B)/ps/libc.o wifimenu/init.ld
	$(LD) -T wifimenu/init.ld --emit-relocs --gc-sections --no-warn-rwx-segments -z noexecstack -o $@ $(B)/ps/entry.o $(B)/ps/toolbox_glue.o $(B)/ps/piscrn.o $(B)/ps/pdcommon.o $(B)/ps/dlgglue.o $(B)/ps/libc.o $(LIBGCC)
$(B)/Aurora68-ScreenSetup.bin: $(B)/piscrn.elf ../tools/mac/elf2init.py ../tools/mac/macelf.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2init.py $< --name "Aurora68 Screen Setup" --creator PISR --icons setup -o $@

# ---- PiSlots: the NuBus cards the Slot Manager knows about, an application in C (mac/pislots) ----
$(B)/pislots_traps.txt: c/traps.txt pislots/traps.txt
	@mkdir -p $(B)
	cat c/traps.txt pislots/traps.txt > $@
$(B)/sl/toolbox_glue.s $(B)/sl/toolbox_gen.h: $(B)/pislots_traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)/sl
	$(PY) ../tools/mac/mktraps.py $< -s $(B)/sl/toolbox_glue.s -H $(B)/sl/toolbox_gen.h
$(B)/sl/toolbox_glue.o: $(B)/sl/toolbox_glue.s
	$(AS) -o $@ $<
$(B)/sl/pislots.o: pislots/pislots.c c/toolbox.h $(B)/sl/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/sl,$(CFLAGS)) -c -o $@ $<
$(B)/sl/libc.o: c/libc.c c/toolbox.h $(B)/sl/toolbox_gen.h
	$(CC) $(subst -I$(B),-I$(B)/sl,$(CFLAGS)) -c -o $@ $<
$(B)/pislots.elf: $(B)/crt0.o $(B)/sl/toolbox_glue.o $(B)/sl/libc.o $(B)/sl/pislots.o c/app.ld
	$(LD) -T c/app.ld --emit-relocs --no-warn-rwx-segments -z noexecstack -o $@ $(B)/crt0.o $(B)/sl/toolbox_glue.o $(B)/sl/libc.o $(B)/sl/pislots.o $(LIBGCC)
$(B)/Aurora68-Slots.bin: $(B)/pislots.elf ../tools/mac/elf2app.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2app.py $< --name "Aurora68 Slots" --creator PISL --size 192 --icons slots -o $@
# ---- PiDisplay: the HDMI screen's monitor, resolution and rotation, an application in C (mac/pidisplay, docs/10) ----
PD_CFLAGS = $(subst -I$(B),-I$(B)/pd,$(CFLAGS)) -Ipidisplay -I../emu68/src/psmac
$(B)/pidisplay_traps.txt: c/traps.txt pidisplay/traps.txt
	@mkdir -p $(B)
	cat c/traps.txt pidisplay/traps.txt > $@
$(B)/pd/toolbox_glue.s $(B)/pd/toolbox_gen.h: $(B)/pidisplay_traps.txt ../tools/mac/mktraps.py
	@mkdir -p $(B)/pd
	$(PY) ../tools/mac/mktraps.py $< -s $(B)/pd/toolbox_glue.s -H $(B)/pd/toolbox_gen.h
$(B)/pd/toolbox_glue.o: $(B)/pd/toolbox_glue.s
	$(AS) -o $@ $<
$(B)/pd/glue.o: pidisplay/glue.s
	@mkdir -p $(B)/pd
	$(AS) -o $@ $<
$(B)/pd/dlgglue.o: pidisplay/dlgglue.s
	@mkdir -p $(B)/pd
	$(AS) -o $@ $<
$(B)/pd/libc.o: c/libc.c c/toolbox.h $(B)/pd/toolbox_gen.h
	$(CC) $(PD_CFLAGS) -c -o $@ $<
$(B)/pd/pdcommon.o: pidisplay/pdcommon.c pidisplay/pdcommon.h c/toolbox.h $(B)/pd/toolbox_gen.h ../emu68/src/psmac/psmb.h
	$(CC) $(PD_CFLAGS) -c -o $@ $<
$(B)/pd/pidisplay.o: pidisplay/pidisplay.c pidisplay/pdcommon.h c/toolbox.h $(B)/pd/toolbox_gen.h ../emu68/src/psmac/psmb.h
	$(CC) $(PD_CFLAGS) -c -o $@ $<
PD_OBJS = $(B)/crt0.o $(B)/pd/toolbox_glue.o $(B)/pd/libc.o $(B)/pd/glue.o $(B)/pd/dlgglue.o $(B)/pd/pdcommon.o $(B)/pd/pidisplay.o
$(B)/pidisplay.elf: $(PD_OBJS) c/app.ld
	$(LD) -T c/app.ld --emit-relocs --no-warn-rwx-segments -z noexecstack -o $@ $(PD_OBJS) $(LIBGCC)
# Balloon Help for the four menus (System 7) and a 'vers' for the Finder's Get Info
$(B)/pd/pd-hmnu-128.bin: pidisplay/balloons.txt ../tools/mac/mkhmnu.py
	@mkdir -p $(B)/pd
	$(PY) ../tools/mac/mkhmnu.py pidisplay/balloons.txt -o $(B)/pd/pd
	$(PY) ../tools/mac/mkhmnu.py --vers 1.0 "1.0, Aurora68 (21 November 2026)" -o $(B)/pd/pd-vers.bin
$(B)/Aurora68-Display.bin: $(B)/pidisplay.elf $(B)/pd/pd-hmnu-128.bin ../tools/mac/elf2app.py ../tools/mac/rsrc.py $(ICONS)
	$(PY) ../tools/mac/elf2app.py $< --name "Aurora68 Display" --creator PIDY --size 384 --icons display \
	    --rsrc hmnu:128:$(B)/pd/pd-hmnu-128.bin --rsrc hmnu:129:$(B)/pd/pd-hmnu-129.bin --rsrc hmnu:130:$(B)/pd/pd-hmnu-130.bin \
	    --rsrc hmnu:131:$(B)/pd/pd-hmnu-131.bin --rsrc vers:1:$(B)/pd/pd-vers.bin -o $@

# the whole set onto a startup disk: the extensions into the System Folder, the programs in their folders
# (tools/mac/aurora68-layout.py; the old names' extensions taken out, as two copies would both run)
install: all
	$(PY) ../tools/mac/aurora68-layout.py install $(IMG) $(if $(PART),--part $(PART)) --build $(B)

clean:
	rm -rf $(B)
