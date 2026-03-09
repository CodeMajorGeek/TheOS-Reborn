#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=128M

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"
[ -z "$THEOS_QEMU_GPU" ] && THEOS_QEMU_GPU="vga"

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

[ -z "$THEOS_QEMU_NUMA" ] && THEOS_QEMU_NUMA=0
[ -z "$THEOS_BOOT_FROM_ISO_DISK" ] && THEOS_BOOT_FROM_ISO_DISK=1

if [ ! -f "TheOS.iso" ]; then
	echo "[run] missing TheOS.iso (build it first with: ninja -C Build iso)" >&2
	exit 1
fi

if [ "$THEOS_BOOT_FROM_ISO_DISK" = "0" ] && [ ! -f "$THEOS_DISK_NAME" ]; then
	echo "[run] missing disk image '$THEOS_DISK_NAME' (create it with: ninja -C Build create-disk)" >&2
	exit 1
fi

GPU_ARGS=()
case "$THEOS_QEMU_GPU" in
	vga|VGA|std|STD|legacy|LEGACY)
		THEOS_QEMU_GPU="vga"
		GPU_ARGS=(
			-device "VGA,vgamem_mb=64"
		)
		;;
	virtio|VIRTIO|virtio-vga|VIRTIO-VGA)
		THEOS_QEMU_GPU="virtio"
		GPU_ARGS=(
			-device "virtio-vga"
		)
		;;
	*)
		echo "[run] invalid THEOS_QEMU_GPU='$THEOS_QEMU_GPU' (expected: vga|virtio)" >&2
		exit 1
		;;
esac

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

BOOT_MEDIA_ARGS=()
if [ "$THEOS_BOOT_FROM_ISO_DISK" = "1" ]; then
	echo "[run] root fs source: embedded disk in TheOS.iso"
	BOOT_MEDIA_ARGS=(
		-drive "id=osdisk,file=TheOS.iso,format=raw,if=none"
		-device "ahci,id=ahci"
		-device "driver=ide-hd,drive=osdisk,bus=ahci.0"
	)
else
	echo "[run] root fs source: external disk image '$THEOS_DISK_NAME'"
	BOOT_MEDIA_ARGS=(
		-cdrom "TheOS.iso"
		-drive "id=disk,file=$THEOS_DISK_NAME,format=raw,if=none"
		-device "ahci,id=ahci"
		-device "driver=ide-hd,drive=disk,bus=ahci.0"
	)
fi

echo "[run] gpu device: $THEOS_QEMU_GPU"

qemu-system-x86_64 \
	-chardev stdio,id=char0,mux=on,logfile=serial.log,signal=off \
	-monitor telnet::45454,server,nowait \
	-serial chardev:char0 -mon chardev=char0 \
	-monitor unix:qemu-monitor-socket,server,nowait \
	-m $THEOS_RAM_SIZE \
	-cpu $THEOS_QEMU_CPU \
	-smp 4 \
	-s \
	-net none \
	"${GPU_ARGS[@]}" \
	"${NUMA_ARGS[@]}" \
	"-enable-kvm" \
	"${BOOT_MEDIA_ARGS[@]}"
	
exit 0
