#!/usr/bin/env bash
# Boots a drive image as a USB stick in QEMU and saves a screenshot.
#   tests/qemu_boot.sh <image> bios|uefi <seconds> <out.png>
set -euo pipefail
img=$1 fw=$2 secs=$3 out=$4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fwopts=()
if [ "$fw" = uefi ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
    fwopts=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
            -drive if=pflash,format=raw,file="$work/vars.fd")
fi
accel=()
[ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
qemu-system-x86_64 "${accel[@]}" -m ${QEMU_MEM:-1024} -machine q35 "${fwopts[@]}" \
    -drive file="$img",format=raw,if=none,id=usb,snapshot=on \
    -device qemu-xhci -device usb-storage,drive=usb,bootindex=0 \
    -display none -monitor unix:"$work/mon",server,nowait -no-reboot &
pid=$!
sleep "$secs"
printf 'screendump %s\n' "$work/shot.ppm" | python3 -c "
import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect('$work/mon'); time.sleep(0.3)
s.sendall(sys.stdin.read().encode()); time.sleep(1.0); s.sendall(b'quit\n'); time.sleep(0.3)"
wait $pid 2>/dev/null || true
python3 -c "from PIL import Image; Image.open('$work/shot.ppm').save('$out')"
echo "screenshot: $out"
