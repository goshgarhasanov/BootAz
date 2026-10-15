#!/usr/bin/env bash
# Builds the sample images the core tests read. Needs: xorriso, mkfs.vfat, mtools, python3 + pycdlib
# (pass the python with pycdlib as $PYTHON), gzip, xz, zstd, bzip2.
#   tests/make_test_images.sh <output-dir>
set -euo pipefail
out=${1:?output dir}
PYTHON=${PYTHON:-python3}
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# ---- A Linux-like tree: deep folders, mixed case, long names, a symlink, a 5 MiB file, an EFI loader.
src="$work/src"
mkdir -p "$src/Deep/Dir/With/Long Name Folder" "$src/EFI/BOOT" "$src/boot/grub"
echo "hello from rock ridge" > "$src/Deep/Dir/With/Long Name Folder/CamelCase-File.txt"
echo "readme" > "$src/README.txt"
ln -s README.txt "$src/link-to-readme"
head -c $((5 * 1024 * 1024 + 123)) /dev/urandom > "$src/big.bin"
sha256sum "$src/big.bin" | cut -d' ' -f1 > "$out/big.bin.sha256"
printf 'MZfake-efi' > "$src/EFI/BOOT/BOOTX64.EFI"
printf 'set timeout=1\nmenuentry "Test" { linux /vmlinuz }\n' > "$src/boot/grub/grub.cfg"

# El Torito EFI image (FAT) so the image is "UEFI bootable".
truncate -s 2M "$work/efi.img"
mkfs.vfat -n EFI "$work/efi.img" >/dev/null
mmd -i "$work/efi.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/efi.img" "$src/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$work/efi.img" "$src/efi.img"

xorriso -as mkisofs -quiet -V BOOTAZ_TEST -R -J -joliet-long \
    -e efi.img -no-emul-boot -isohybrid-gpt-basdat \
    -o "$out/rr.iso" "$src" 2>/dev/null
xorriso -as mkisofs -quiet -V BOOTAZ_TEST --norock -J -joliet-long -o "$out/joliet.iso" "$src" 2>/dev/null

# ---- A Windows-like UDF image.
win="$work/win"
mkdir -p "$win/sources" "$win/efi/boot" "$win/boot"
head -c $((3 * 1024 * 1024 + 7)) /dev/urandom > "$win/sources/install.wim"
sha256sum "$win/sources/install.wim" | cut -d' ' -f1 > "$out/install.wim.sha256"
printf 'boot.wim' > "$win/sources/boot.wim"
printf 'bootmgr' > "$win/bootmgr"
printf 'MZefi' > "$win/efi/boot/bootx64.efi"
"$PYTHON" - "$win" "$out/udf.iso" <<'EOF'
import os, sys, pycdlib
root, dest = sys.argv[1], sys.argv[2]
iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, udf='2.60', vol_ident='CCCOMA_X64FRE_EN-US_DV9')
n = 0
for dirpath, dirs, files in os.walk(root):
    rel = os.path.relpath(dirpath, root)
    for d in sorted(dirs):
        p = '/' + os.path.join(rel, d).lstrip('./') if rel != '.' else '/' + d
        n += 1
        iso.add_directory('/D%d' % n, udf_path=p)
    for f in sorted(files):
        p = os.path.join(dirpath, f)
        up = '/' + os.path.relpath(p, root)
        n += 1
        iso.add_file(p, '/F%d.;1' % n, udf_path=up)
iso.write(dest)
iso.close()
EOF

# ---- A raw disk image, compressed four ways.
truncate -s 8M "$work/disk.img"
printf 'o\nn\np\n1\n2048\n\nt\nc\nw\n' | /sbin/fdisk "$work/disk.img" >/dev/null 2>&1 || true
head -c 1M /dev/urandom | dd of="$work/disk.img" bs=1M seek=2 conv=notrunc status=none
sha256sum "$work/disk.img" | cut -d' ' -f1 > "$out/disk.img.sha256"
gzip -c "$work/disk.img" > "$out/disk.img.gz"
xz -c -T0 "$work/disk.img" > "$out/disk.img.xz"
zstd -q -c "$work/disk.img" > "$out/disk.img.zst"
bzip2 -c "$work/disk.img" > "$out/disk.img.bz2"
echo "test images in $out"
