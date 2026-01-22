#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=128M

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

qemu-system-x86_64 \
	-chardev stdio,id=char0,mux=on,logfile=serial.log,signal=off \
	-monitor telnet::45454,server,nowait \
	-serial chardev:char0 -mon chardev=char0 \
	-monitor unix:qemu-monitor-socket,server,nowait \
	-m $THEOS_RAM_SIZE \
	-cpu $THEOS_QEMU_CPU \
	-smp 2 \
	-device VGA,vgamem_mb=64 \
	-cdrom TheOS.iso \
	-s \
	-net none \
	-drive id=disk,file=$THEOS_DISK_NAME,format=raw,if=none \
	-device ahci,id=ahci \
	-device driver=ide-hd,drive=disk,bus=ahci.0
	
exit 0