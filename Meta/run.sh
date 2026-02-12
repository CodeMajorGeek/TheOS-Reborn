#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=128M

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

[ -z "$THEOS_QEMU_NUMA" ] && THEOS_QEMU_NUMA=0

NUMA_ARGS=()
if [ "$THEOS_QEMU_NUMA" = "1" ]; then
	[ -z "$THEOS_NUMA_NODE0_MEM" ] && THEOS_NUMA_NODE0_MEM="64M"
	[ -z "$THEOS_NUMA_NODE1_MEM" ] && THEOS_NUMA_NODE1_MEM="64M"

	NUMA_ARGS=(
		-object "memory-backend-ram,id=ram0,size=${THEOS_NUMA_NODE0_MEM}"
		-object "memory-backend-ram,id=ram1,size=${THEOS_NUMA_NODE1_MEM}"
		-numa "node,nodeid=0,cpus=0-1,memdev=ram0"
		-numa "node,nodeid=1,cpus=2-3,memdev=ram1"
	)
fi

qemu-system-x86_64 \
	-chardev stdio,id=char0,mux=on,logfile=serial.log,signal=off \
	-monitor telnet::45454,server,nowait \
	-serial chardev:char0 -mon chardev=char0 \
	-monitor unix:qemu-monitor-socket,server,nowait \
	-m $THEOS_RAM_SIZE \
	-cpu $THEOS_QEMU_CPU \
	-smp 4 \
	-device VGA,vgamem_mb=64 \
	-cdrom TheOS.iso \
	-s \
	-net none \
	"${NUMA_ARGS[@]}" \
	-drive id=disk,file=$THEOS_DISK_NAME,format=raw,if=none \
	-device ahci,id=ahci \
	-device driver=ide-hd,drive=disk,bus=ahci.0
	
exit 0
