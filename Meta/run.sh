#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=128M

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

echo $THEOS_ENABLE_KVM;

qemu-system-x86_64 -chardev stdio,id=char0,mux=on,logfile=serial.log,signal=off -serial chardev:char0 -mon chardev=char0 \
	-enable-kvm \
	-monitor unix:qemu-monitor-socket,server,nowait \
	-m $THEOS_RAM_SIZE \
	-cpu $THEOS_QEMU_CPU \
	-smp 2 \
	-device VGA,vgamem_mb=64 \
	-cdrom TheOS.iso \
	-s \
	-drive id=disk,file=$THEOS_DISK_NAME,if=none \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk,bus=ahci.0 \
	
exit 0