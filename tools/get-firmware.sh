#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# get-firmware.sh - fetch every third-party file an Aurora68 SD card needs, for a Pi 3B, 3B+, 3A+,
# Zero 2 W and Pi 4 (or 400) on one card, and check each one.
#
# Run it from the card's root folder (macOS, Linux; POSIX sh and curl):
#     sh get-firmware.sh                                   fetch and check, print the container line
#     sh get-firmware.sh --container MacIIci_368CADFE.rom [MyMac.hda ...] [--declrom aurora68-WxH.declrom]
#                                                          ... and make aurora68.cpio with it
#     sh get-firmware.sh --dir /Volumes/BOOT               the card somewhere else
#
# What it puts where:
#   the card's root   the Raspberry Pi firmware's boot files (github.com/raspberrypi/firmware, boot/):
#                     bootcode.bin, start.elf, fixup.dat (Pi 3), start4.elf, fixup4.dat (Pi 4), and
#                     the device trees of the 3B, 3B+, Zero 2 W, 4B and 400. The 3A+ has none of
#                     its own: its firmware loads bcm2710-rpi-3-b-plus.dtb (the kernel tells the
#                     two apart by the revision code). config.txt uses no overlays, so none are
#                     fetched.
#   wifi-firmware/    the WiFi and Bluetooth firmware, which goes INTO the container, not on the card:
#                     BCM43438 (Pi 3B; files named 43430), BCM43436 and 43436s (the two Zero 2 W
#                     chips; the 43436s has no .clm_blob and takes the 43430's), BCM43455 (3B+, 3A+,
#                     4B): the .bin, .txt and .clm_blob from RPi-Distro/firmware-nonfree, the .hcd
#                     from RPi-Distro/bluez-firmware. The kernel takes the WiFi set by the chip's id
#                     and the Bluetooth one by the board, so one container serves every board.
# Every file is fetched by its real path at a pinned commit and checked against its SHA-256 (the
# brcm/ names in firmware-nonfree are symbolic links: a raw download of one is a 31-byte path, not
# firmware). Nothing of Apple's is downloaded: the Macintosh IIci ROM and the system software are
# yours to supply.
set -u

FW=https://raw.githubusercontent.com/raspberrypi/firmware/4a44943386efa917f012ec55aba8b51f750be0f7/boot
NF=https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/4051f1cf4cfb307db3f3eaf0027130fc8267eeae/debian/config/brcm80211
BZ=https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/6851250bf9d51ff50e3c5b2cf2111e2419d4335b/broadcom

# where  url  sha256  (where: . the card's root, w wifi-firmware/)
LIST="
. $FW/bootcode.bin 3d04e3f6b3c62981c38a31429cae62aab14cfd99dedd56403c36216ac08bb16d
. $FW/start.elf 8fd31e816477c455b9ae313787e9a869cacc32e476eac6653768063477368102
. $FW/fixup.dat 34d4908502236975519a976f802822bacff5a9246cfce9492b8034b8de491cc2
. $FW/start4.elf 3073e1537ea9229f2ed2f9f7864c91494fbe2b8c45910f22ccf06b0f3c9295a8
. $FW/fixup4.dat a5eba96d61498b3439300a069ea477c1ac749085761095c624b695f357070f0d
. $FW/bcm2710-rpi-3-b.dtb a1bf19944b3c6cd1c8839de25a9a69f1f53a1ad1dd49993f394bd64a78022a2f
. $FW/bcm2710-rpi-3-b-plus.dtb 50b03677ce2d34ecf8aad91eaaf11a62d75115b35fc1928d00b2f32ffecace77
. $FW/bcm2710-rpi-zero-2-w.dtb 5b31bc70011e454f482ba323b1eca22bf54fa2fa4682576ee73816a2eccfa277
. $FW/bcm2711-rpi-4-b.dtb 9b572c00662cd3acab6a2b1ae5187bfb30faa78c3d3f0ded17e6910a5541d8fb
. $FW/bcm2711-rpi-400.dtb ce00383168ac105de70a6a383dfe2daa89f3f26f6db47e9ac66e0843227b7292
w $NF/cypress/cyfmac43430-sdio.bin 0717f8e798f3962230e76e9d840385ba127a38d31d6a55acd5c97cf53e4acc9d
w $NF/cypress/cyfmac43430-sdio.clm_blob 3376b9c9b32d16bf762e21c7fafb665365070ae240d092498d0d1987c22022aa
w $NF/brcm/brcmfmac43430-sdio.txt fc3949a4c32f07c18308e7e145c7615be314158e7d714a80e04e4791f16495f9
w $NF/brcm/brcmfmac43436-sdio.bin 510a7dd1e056199b309425548ee0bd846993a1837ac7fa1e4d3e641f05a1327a
w $NF/brcm/brcmfmac43436-sdio.clm_blob fce7cbb62ffa6a5a65ca97b13f6fbf28d06c02d986c2072d65bf72164755fc34
w $NF/brcm/brcmfmac43436-sdio.txt 4cda90facd8844cff60d80b34b24ecbae76adb9a62508a109461b8bf42b478d1
w $NF/brcm/brcmfmac43436s-sdio.bin 68b9bcc9855d91733cd44c21de4cb507c91b0d32d838c0696def5eb96c99e2de
w $NF/brcm/brcmfmac43436s-sdio.txt 37a8b85a5a9742761101b764a07bc4d0c8b09f2e180eaea3b503a834277ad595
w $NF/cypress/cyfmac43455-sdio-standard.bin 8868b5420be1191355d62690da4d96d14d30c1b3899d02fd15e9d664596650f9
w $NF/cypress/cyfmac43455-sdio.clm_blob 9823842cae9fb9a5dd1e5fb31f595516ec7deee341354bef30bb3026eee29cc1
w $NF/brcm/brcmfmac43455-sdio.txt ca709be81a78bdb6932936374f39943acbd7af07fae6151011127599a3ce9e3d
w $BZ/BCM43430A1.hcd c096ad4a5c3f06ed7d69eba246bf89ada9acba64a5b6f51b1e9c12f99bb1e1a7
w $BZ/BCM43430B0.hcd 338c2c6631131f516bfc7e64ef0872bd0402e1f98ef9d0c900eef0c814d90a25
w $BZ/BCM4345C0.hcd 51c45e77ddad91a19e96dc8fb75295b2087c279940df2634b23baf71b6dea42c
w $BZ/BCM4345C5.hcd fb9f4ec2df5301bd35f416384e103c012c5983024c49aa72fbbaf90177512caa
"

DIR=.
ROM=
DECLROM=
DISKS=
while [ $# -gt 0 ]; do
    case "$1" in
    --dir) DIR=$2; shift 2 ;;
    --declrom) DECLROM=$2; shift 2 ;;
    --container) ROM=$2; shift 2
        while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do DISKS="$DISKS $1"; shift; done ;;
    -h|--help) sed -n '3,30p' "$0"; exit 0 ;;
    *) echo "get-firmware.sh: unknown option $1 (--help)"; exit 2 ;;
    esac
done

command -v curl > /dev/null 2>&1 || { echo "curl is needed (macOS and most Linux systems have it)"; exit 1; }
if command -v sha256sum > /dev/null 2>&1; then SHA="sha256sum"
elif command -v shasum > /dev/null 2>&1; then SHA="shasum -a 256"
else SHA=""; echo "(no sha256sum or shasum here: sizes are checked, checksums are not)"; fi

sum_of() { $SHA "$1" | cut -d' ' -f1; }
size_of() { wc -c < "$1" | tr -d ' '; }

mkdir -p "$DIR/wifi-firmware" || exit 1
echo "$LIST" | while read -r where url want; do
    [ -n "$url" ] || continue
    name=${url##*/}
    if [ "$where" = "w" ]; then dest="$DIR/wifi-firmware/$name"; else dest="$DIR/$name"; fi
    if [ -f "$dest" ] && [ -n "$SHA" ] && [ "$(sum_of "$dest")" = "$want" ]; then
        printf '  kept   %8s  %s\n' "$(size_of "$dest")" "${dest#$DIR/}"
        continue
    fi
    tries=0
    while :; do
        tries=$((tries + 1))
        if curl -fsSL --retry 3 -o "$dest.part" "$url"; then break; fi
        [ $tries -ge 3 ] && break
        sleep $((tries * 2))
    done
    if [ ! -f "$dest.part" ]; then
        printf '  FAILED            %s (could not download %s)\n' "$name" "$url"
        echo x >> "$DIR/wifi-firmware/.failed"
        continue
    fi
    n=$(size_of "$dest.part")
    if [ "$n" -lt 1000 ]; then
        printf '  FAILED %8s  %s: too small to be firmware (a link, or an error page)\n' "$n" "$name"
        rm -f "$dest.part"; echo x >> "$DIR/wifi-firmware/.failed"; continue
    fi
    if [ -n "$SHA" ] && [ "$(sum_of "$dest.part")" != "$want" ]; then
        printf '  FAILED %8s  %s: its SHA-256 is not the expected one\n' "$n" "$name"
        rm -f "$dest.part"; echo x >> "$DIR/wifi-firmware/.failed"; continue
    fi
    mv -f "$dest.part" "$dest"
    printf '  got    %8s  %s\n' "$n" "${dest#$DIR/}"
done

if [ -f "$DIR/wifi-firmware/.failed" ]; then
    n=$(wc -l < "$DIR/wifi-firmware/.failed" | tr -d ' ')
    rm -f "$DIR/wifi-firmware/.failed"
    echo
    echo "$n file(s) FAILED: run the script again (it keeps what is already right)."
    exit 1
fi

W=wifi-firmware
WIFI="$W/cyfmac43430-sdio.bin $W/brcmfmac43430-sdio.txt $W/cyfmac43430-sdio.clm_blob $W/BCM43430A1.hcd \
$W/brcmfmac43436-sdio.bin $W/brcmfmac43436-sdio.txt $W/brcmfmac43436-sdio.clm_blob $W/BCM43430B0.hcd \
$W/brcmfmac43436s-sdio.bin $W/brcmfmac43436s-sdio.txt \
$W/cyfmac43455-sdio-standard.bin $W/brcmfmac43455-sdio.txt $W/cyfmac43455-sdio.clm_blob $W/BCM4345C0.hcd $W/BCM4345C5.hcd"
[ -n "$DECLROM" ] || DECLROM=aurora68.declrom
echo
echo "All files present and checked. The boot files are in the card's root; the WiFi and Bluetooth"
echo "firmware in wifi-firmware/ goes into the container (about 3 MB for every board's set):"
echo
echo "  python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom $DECLROM \\"
echo "    $WIFI"
echo
echo "(your own IIci ROM, 368CADFE, 512 KB: it is never downloaded; aurora68-WxH.declrom for a larger"
echo " screen; a disk image can go on the card beside aurora68.cpio instead of inside it.)"

if [ -n "$ROM" ]; then
    [ -f "$DIR/$ROM" ] || [ -f "$ROM" ] || { echo "no ROM file $ROM"; exit 1; }
    command -v python3 > /dev/null 2>&1 || { echo "python3 is needed for make-container.py"; exit 1; }
    echo
    echo "== making aurora68.cpio"
    # shellcheck disable=SC2086
    (cd "$DIR" && python3 make-container.py -o aurora68.cpio "$ROM" "$DECLROM" $DISKS $WIFI)
fi
