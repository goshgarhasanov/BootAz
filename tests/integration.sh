#!/usr/bin/env bash
# End-to-end tests on loop devices (needs root). Each scenario writes a sparse image file through
# bootaz-cli exactly like a USB stick and checks the result with the system's own tools.
#   sudo tests/integration.sh <build-dir> <test-data-dir> [windows.iso]
# test-data-dir comes from tests/make_test_images.sh (plus alpine.iso for the Linux scenarios).
set -u
BUILD=$(readlink -f "${1:?build dir}")
DATA=$(readlink -f "${2:?test data dir}")
WINISO=${3:-}
CLI="$BUILD/bootaz-cli"
WORK=$(mktemp -d /var/tmp/bootaz-it.XXXXXX)
PASS=0
FAIL=0
LOOP=""

cleanup() {
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { echo "  ✓ $*"; PASS=$((PASS + 1)); }
bad()  { echo "  ✗ $*"; FAIL=$((FAIL + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# A fresh loop device of the given size (GB, may be fractional via M suffix).
newdisk() {
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    rm -f "$WORK/disk.img"
    truncate -s "$1" "$WORK/disk.img"
    LOOP=$(losetup --find --show --partscan "$WORK/disk.img")
    case "$LOOP" in /dev/loop*) ;; *) echo "no loop device"; exit 2 ;; esac
}
part() { echo "${LOOP}p$1"; }
label() { blkid -p -s LABEL -o value "$(part "$1")" 2>/dev/null; }
fstype() { blkid -p -s TYPE -o value "$(part "$1")" 2>/dev/null; }
write() { "$CLI" --lang en "$@" --device "$LOOP" --yes >"$WORK/out.txt" 2>&1; }

[ "$(id -u)" = 0 ] || { echo "run as root (loop devices)"; exit 2; }

echo "== DD mode (Alpine)"
if [ -f "$DATA/alpine.iso" ]; then
    newdisk 1G
    write write --image "$DATA/alpine.iso" --mode dd
    check "write succeeded" "[ $? = 0 ]"
    check "image bytes on the drive" "cmp -n $(stat -c %s "$DATA/alpine.iso") '$DATA/alpine.iso' '$LOOP'"
fi

echo "== ISO mode, GPT, own FAT32 (Alpine)"
if [ -f "$DATA/alpine.iso" ]; then
    newdisk 1G
    write write --image "$DATA/alpine.iso" --mode iso --scheme gpt
    check "write succeeded" "[ $? = 0 ]"
    check "GPT" "sfdisk -d '$LOOP' 2>/dev/null | grep -q 'label: gpt'"
    check "FAT32 is clean" "fsck.vfat -n '$(part 1)' >/dev/null 2>&1"
    check "label named after the system" "[ '$(label 1)' = 'ALPINE_3_20' ]"
    check "EFI loader present" "mdir -i '$(part 1)' ::/efi/boot >/dev/null 2>&1"
fi

echo "== ISO mode, MBR, BIOS + UEFI with Syslinux (Alpine)"
if [ -f "$DATA/alpine.iso" ] && command -v syslinux >/dev/null; then
    newdisk 1G
    write write --image "$DATA/alpine.iso" --mode iso --scheme mbr --target bios+uefi
    check "write succeeded" "[ $? = 0 ]"
    check "partition is active" "sfdisk -d '$LOOP' 2>/dev/null | grep -q 'bootable'"
    check "syslinux installed" "mdir -a -i '$(part 1)' ::/boot/syslinux/ldlinux.sys >/dev/null 2>&1"
fi

echo "== Persistence (Linux images with casper / live only)"
# Covered by the planner tests; Alpine does not support it and must refuse quietly.
if [ -f "$DATA/alpine.iso" ]; then
    newdisk 1G
    write write --image "$DATA/alpine.iso" --mode iso --persistence 200M
    check "persistence ignored for Alpine" "grep -q 'does not support persistence' '$WORK/out.txt'"
fi

echo "== Windows-like UDF image, NTFS + UEFI:NTFS + unattend"
newdisk 1G
write write --image "$DATA/udf.iso" --scheme mbr --target bios+uefi --fs ntfs --win-bypass --win-no-online --win-local-user Test
check "write succeeded" "[ $? = 0 ]"
check "NTFS data partition" "[ '$(fstype 1)' = ntfs ]"
check "UEFI:NTFS partition" "[ '$(label 2)' = BOOTAZ_UEFI ]"
mkdir -p "$WORK/m"
if mount -o ro "$(part 1)" "$WORK/m" 2>/dev/null; then
    check "install.wim copied intact" "[ \"\$(sha256sum < '$WORK/m/sources/install.wim' | cut -d' ' -f1)\" = \"\$(cat '$DATA/install.wim.sha256')\" ]"
    check "autounattend.xml with the TPM bypass" "grep -q BypassTPMCheck '$WORK/m/autounattend.xml'"
    check "local account in autounattend.xml" "grep -q '<Name>Test</Name>' '$WORK/m/autounattend.xml'"
    umount "$WORK/m"
fi
check "Windows NTFS boot record" "dd if='$(part 1)' bs=512 count=1 status=none | grep -aq 'Ctrl+Alt+Del'"

echo "== Format only: FAT32 / exFAT / NTFS / ext4"
newdisk 300M
for fs in fat32 exfat ntfs ext4; do
    write format --fs $fs --label "T $fs"
    expected=$fs; [ $fs = fat32 ] && expected=vfat
    check "$fs" "[ '$(fstype 1)' = '$expected' ]"
done

echo "== Restore, fake-capacity check, bad blocks, save"
newdisk 300M
write restore
check "restore: one FAT32 partition" "[ '$(fstype 1)' = vfat ] && [ \$(sfdisk -d '$LOOP' 2>/dev/null | grep -c start=) = 1 ]"
before=$(sha256sum < "$LOOP")
write fakecheck
check "fakecheck passes on a real drive" "[ $? = 0 ]"
check "fakecheck kept the data" "[ \"\$(sha256sum < '$LOOP')\" = \"$before\" ]"
write badblocks --passes 1
check "bad block test passes" "[ $? = 0 ]"
write format --fs fat32 --label SAVE
"$CLI" save --device "$LOOP" --out "$WORK/s.img.zst" --used-only >/dev/null 2>&1
check "drive saved as .img.zst" "[ -s '$WORK/s.img.zst' ]"

echo "== Fake drive (device-mapper: 300 MB reported, 100 MB real)"
if command -v dmsetup >/dev/null; then
    truncate -s 100M "$WORK/real.img"
    R=$(losetup --find --show "$WORK/real.img")
    printf "0 204800 linear $R 0\n204800 409600 zero\n" | dmsetup create bootaz-it-fake
    "$CLI" --lang en fakecheck --device /dev/mapper/bootaz-it-fake --allow-fixed >"$WORK/out.txt" 2>&1
    check "fake drive detected" "grep -q 'FAKE DRIVE' '$WORK/out.txt'"
    dmsetup remove bootaz-it-fake
    losetup -d "$R"
fi

echo "== Safety"
SYS=$(findmnt -n -o SOURCE / | sed 's/[0-9]*$//; s/p$//')
"$CLI" --lang en write --device "$SYS" --image "$DATA/rr.iso" --allow-fixed --yes >"$WORK/out.txt" 2>&1
check "refuses the system drive" "grep -qi 'operating system' '$WORK/out.txt'"

if [ -n "$WINISO" ] && [ -f "$WINISO" ]; then
    for layout in ntfs dual split; do
        echo "== Windows ISO, layout $layout"
        newdisk 16G
        write write --image "$WINISO" --scheme gpt --win-layout $layout --win-bypass
        check "write succeeded" "[ $? = 0 ]"
    done
fi

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL = 0 ]
