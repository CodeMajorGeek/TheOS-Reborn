#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=512M

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"

qemu-system-x86_64 -chardev stdio,id=char0,mux=on,logfile=serial.log,signal=off -serial chardev:char0 -mon chardev=char0 \
	-m $THEOS_RAM_SIZE \
	-cpu $THEOS_QEMU_CPU \
	-smp 2 \
	-device VGA,vgamem_mb=64 \
	-cdrom TheOS.iso \
	-s -S

exit 0