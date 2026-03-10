#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=256M

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"
[ -z "$THEOS_QEMU_GPU" ] && THEOS_QEMU_GPU="vga"
[ -z "$THEOS_ISO_NAME" ] && THEOS_ISO_NAME="TheOS.iso"

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

[ -z "$THEOS_QEMU_NUMA" ] && THEOS_QEMU_NUMA=0
[ -z "$THEOS_BOOT_FROM_ISO_DISK" ] && THEOS_BOOT_FROM_ISO_DISK=1
[ -z "$THEOS_QEMU_KVM" ] && THEOS_QEMU_KVM=1
[ -z "$THEOS_QEMU_SERIAL" ] && THEOS_QEMU_SERIAL=0
[ -z "$THEOS_QEMU_GDB_STUB" ] && THEOS_QEMU_GDB_STUB=0
[ -z "$THEOS_QEMU_TELNET_MONITOR" ] && THEOS_QEMU_TELNET_MONITOR=0

if [ ! -f "$THEOS_ISO_NAME" ]; then
	echo "[run] missing '$THEOS_ISO_NAME' (build it first with: ninja -C Build iso)" >&2
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
			-device "VGA,vgamem_mb=128"
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
	[ -z "$THEOS_NUMA_NODE0_MEM" ] && THEOS_NUMA_NODE0_MEM="128M"
	[ -z "$THEOS_NUMA_NODE1_MEM" ] && THEOS_NUMA_NODE1_MEM="128M"

	NUMA_ARGS=(
		-object "memory-backend-ram,id=ram0,size=${THEOS_NUMA_NODE0_MEM}"
		-object "memory-backend-ram,id=ram1,size=${THEOS_NUMA_NODE1_MEM}"
		-numa "node,nodeid=0,cpus=0-1,memdev=ram0"
		-numa "node,nodeid=1,cpus=2-3,memdev=ram1"
	)
fi

BOOT_MEDIA_ARGS=()
if [ "$THEOS_BOOT_FROM_ISO_DISK" = "1" ]; then
	echo "[run] root fs source: embedded disk in $THEOS_ISO_NAME"
	BOOT_MEDIA_ARGS=(
		-drive "id=osdisk,file=$THEOS_ISO_NAME,format=raw,if=none"
		-device "ahci,id=ahci"
		-device "driver=ide-hd,drive=osdisk,bus=ahci.0"
	)
else
	echo "[run] root fs source: external disk image '$THEOS_DISK_NAME'"
	BOOT_MEDIA_ARGS=(
		-cdrom "$THEOS_ISO_NAME"
		-drive "id=disk,file=$THEOS_DISK_NAME,format=raw,if=none"
		-device "ahci,id=ahci"
		-device "driver=ide-hd,drive=disk,bus=ahci.0"
	)
fi

echo "[run] gpu device: $THEOS_QEMU_GPU"
echo "[run] kvm: $THEOS_QEMU_KVM"
echo "[run] serial console: $THEOS_QEMU_SERIAL"
echo "[run] gdb stub: $THEOS_QEMU_GDB_STUB"
echo "[run] telnet monitor: $THEOS_QEMU_TELNET_MONITOR"

KVM_ARGS=()
if [ "$THEOS_QEMU_KVM" = "1" ]; then
	KVM_ARGS=(-enable-kvm)
fi

SERIAL_ARGS=()
MONITOR_ARGS=(
	-monitor unix:qemu-monitor-socket,server,nowait
)
if [ "$THEOS_QEMU_SERIAL" = "1" ]; then
	SERIAL_ARGS=(
		-chardev "stdio,id=char0,mux=on,logfile=serial.log,signal=off"
		-serial "chardev:char0"
	)
	if [ "$THEOS_QEMU_TELNET_MONITOR" = "1" ]; then
		MONITOR_ARGS+=(-monitor "telnet::45454,server,nowait")
	fi
fi

GDB_ARGS=()
if [ "$THEOS_QEMU_GDB_STUB" = "1" ]; then
	GDB_ARGS=(-s)
fi

qemu-system-x86_64 \
	-m $THEOS_RAM_SIZE \
	-cpu $THEOS_QEMU_CPU \
	-smp 4 \
	-net none \
	"${SERIAL_ARGS[@]}" \
	"${MONITOR_ARGS[@]}" \
	"${GDB_ARGS[@]}" \
	"${GPU_ARGS[@]}" \
	"${NUMA_ARGS[@]}" \
	"${KVM_ARGS[@]}" \
	"${BOOT_MEDIA_ARGS[@]}"
	
exit 0
