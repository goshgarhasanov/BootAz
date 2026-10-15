#!/usr/bin/env python3
"""Boots a drive image as a USB stick in QEMU, sends key presses and takes screenshots.
   tests/qemu_drive.py <image> <uefi|bios> <outdir> "<step>" ...
   step:  wait:SECONDS | key:KEY[,KEY...] | shot:NAME
   Example: qemu_drive.py usb.img uefi out "wait:60" "shot:lang" "key:ret" "wait:5" "shot:next"
A blank 64 GB target disk is attached so Windows setup can reach its disk page."""
import os, shutil, socket, subprocess, sys, tempfile, time

img, fw, out = sys.argv[1], sys.argv[2], sys.argv[3]
steps = sys.argv[4:]
os.makedirs(out, exist_ok=True)
work = tempfile.mkdtemp()
mon = os.path.join(work, 'mon')
target = os.environ.get('TARGET') or os.path.join(work, 'target.qcow2')
if not os.path.exists(target):
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', target, '64G'], check=True)
vars_path = os.environ.get('VARS') or os.path.join(work, 'vars.fd')
usb = img != 'none'
reboot = os.environ.get('REBOOT') == '1'   # let the guest reboot in place (installed system)
cmd = ['qemu-system-x86_64', '-m', '4096', '-smp', '2', '-machine', 'q35']
if os.access('/dev/kvm', os.W_OK):
    cmd += ['-enable-kvm', '-cpu', 'host']
if fw == 'uefi':
    if not os.path.exists(vars_path):
        shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', vars_path)
    cmd += ['-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
            '-drive', 'if=pflash,format=raw,file=' + vars_path]
if usb:
    cmd += ['-drive', f'file={img},format=raw,if=none,id=usb,snapshot=on',
            '-device', 'qemu-xhci', '-device', 'usb-storage,drive=usb,bootindex=0']
cmd += ['-drive', 'file=' + target + ',if=none,id=hd',
        '-device', 'nvme,drive=hd,serial=bootaz,bootindex=1',
        '-display', 'none', '-monitor', f'unix:{mon},server,nowait'] + ([] if reboot else ['-no-reboot'])
proc = subprocess.Popen(cmd)
for _ in range(50):
    if os.path.exists(mon):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX)
s.connect(mon)
time.sleep(0.3)
s.recv(65536)

def send(c):
    s.sendall((c + '\n').encode())
    time.sleep(0.4)
    try:
        s.setblocking(False)
        s.recv(65536)
    except BlockingIOError:
        pass
    s.setblocking(True)

for st in steps:
    if proc.poll() is not None:
        print('qemu exited', flush=True)
        break
    kind, _, arg = st.partition(':')
    if kind == 'wait':
        time.sleep(float(arg))
    elif kind == 'exit':   # wait until the guest powers off / reboots (with -no-reboot), max SECONDS
        try:
            proc.wait(timeout=float(arg))
            print('qemu exited', flush=True)
            break
        except subprocess.TimeoutExpired:
            print('still running after', arg, flush=True)
    elif kind == 'key':
        for k in arg.split(','):
            send('sendkey ' + k)
            time.sleep(0.6)
    elif kind == 'shot':
        ppm = os.path.join(work, arg + '.ppm')
        send('screendump ' + ppm)
        time.sleep(0.8)
        from PIL import Image
        Image.open(ppm).save(os.path.join(out, arg + '.png'))
        print('shot', os.path.join(out, arg + '.png'), flush=True)
if proc.poll() is None:
    send('quit')
    proc.wait(timeout=30)
shutil.rmtree(work, ignore_errors=True)
