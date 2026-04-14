#!/bin/bash

[ -z "$THEOS_RAM_SIZE" ] && THEOS_RAM_SIZE=256M

# Machine « pc » (i440FX) : contrôleur souris/clavier PS/2 plus proche des invités type PC classique ;
# sur certaines configs, « q35 » + affichage GTK peut donner une souris « figée » côté invité.
[ -z "$THEOS_QEMU_MACHINE" ] && THEOS_QEMU_MACHINE="pc"

[ -z "$THEOS_QEMU_CPU" ] && THEOS_QEMU_CPU="max"
[ -z "$THEOS_QEMU_GPU" ] && THEOS_QEMU_GPU="vga"
[ -z "$THEOS_ISO_NAME" ] && THEOS_ISO_NAME="TheOS.iso"

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

[ -z "$THEOS_QEMU_NUMA" ] && THEOS_QEMU_NUMA=0
[ -z "$THEOS_BOOT_FROM_ISO_DISK" ] && THEOS_BOOT_FROM_ISO_DISK=1
# Accélération hôte (peu liée à la souris PS/2) ; en débogage tu peux forcer THEOS_QEMU_KVM=0 (TCG, plus lent).
[ -z "$THEOS_QEMU_KVM" ] && THEOS_QEMU_KVM=1
[ -z "$THEOS_QEMU_SERIAL" ] && THEOS_QEMU_SERIAL=0
[ -z "$THEOS_QEMU_GDB_STUB" ] && THEOS_QEMU_GDB_STUB=0
[ -z "$THEOS_QEMU_TELNET_MONITOR" ] && THEOS_QEMU_TELNET_MONITOR=0
[ -z "$THEOS_QEMU_AUDIO" ] && THEOS_QEMU_AUDIO=1
[ -z "$THEOS_QEMU_AUDIO_BACKEND" ] && THEOS_QEMU_AUDIO_BACKEND="pa"
[ -z "$THEOS_QEMU_AUDIO_WAV_PATH" ] && THEOS_QEMU_AUDIO_WAV_PATH="theos-audio.wav"
# Backend UI : SDL gère souvent mieux la souris relative / le curseur invité que GTK sur certaines machines.
# Si THEOS_QEMU_DISPLAY est déjà défini, il est utilisé tel quel (THEOS_QEMU_UI ignoré).
THEOS_QEMU_UI_EFFECTIVE=""
if [ -z "$THEOS_QEMU_DISPLAY" ]; then
	[ -z "$THEOS_QEMU_UI" ] && THEOS_QEMU_UI="sdl"
	case "$THEOS_QEMU_UI" in
		sdl|SDL)
			THEOS_QEMU_DISPLAY="sdl"
			THEOS_QEMU_UI_EFFECTIVE="sdl"
			;;
		gtk|GTK)
			# grab-on-hover=off : évite d’avoir à « capturer » la souris au survol.
			THEOS_QEMU_DISPLAY="gtk,zoom-to-fit=on,show-tabs=off,grab-on-hover=off"
			THEOS_QEMU_UI_EFFECTIVE="gtk"
			;;
		*)
			echo "[run] invalid THEOS_QEMU_UI='$THEOS_QEMU_UI' (expected: gtk|sdl)" >&2
			exit 1
			;;
	esac
else
	THEOS_QEMU_UI_EFFECTIVE="(THEOS_QEMU_DISPLAY)"
fi
[ -z "$THEOS_QEMU_NET" ] && THEOS_QEMU_NET="e1000e"
[ -z "$THEOS_QEMU_NET_SOCKET_MCAST" ] && THEOS_QEMU_NET_SOCKET_MCAST="230.0.0.42:23456"
[ -z "$THEOS_QEMU_NET_INJECT_ON_BOOT" ] && THEOS_QEMU_NET_INJECT_ON_BOOT=0
[ -z "$THEOS_QEMU_NET_INJECT_DELAY_MS" ] && THEOS_QEMU_NET_INJECT_DELAY_MS=4000
[ -z "$THEOS_QEMU_NET_INJECT_COUNT" ] && THEOS_QEMU_NET_INJECT_COUNT=3
[ -z "$THEOS_QEMU_NET_INJECT_INTERVAL_MS" ] && THEOS_QEMU_NET_INJECT_INTERVAL_MS=300
[ -z "$THEOS_QEMU_NET_INJECT_SIGNATURE" ] && THEOS_QEMU_NET_INJECT_SIGNATURE="THEOS_RX_AUTOTEST"

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
echo "[run] audio hda: $THEOS_QEMU_AUDIO"
echo "[run] audio backend: $THEOS_QEMU_AUDIO_BACKEND"
echo "[run] ui: $THEOS_QEMU_UI_EFFECTIVE (THEOS_QEMU_UI=gtk|sdl ou THEOS_QEMU_DISPLAY=...)"
echo "[run] display: $THEOS_QEMU_DISPLAY"
echo "[run] machine: $THEOS_QEMU_MACHINE"
echo "[run] fullscreen: disabled (temporary)"

KVM_ARGS=()
if [ "$THEOS_QEMU_KVM" = "1" ]; then
	KVM_ARGS=(-enable-kvm)
fi

SERIAL_ARGS=()
MONITOR_ARGS=(
	-monitor unix:qemu-monitor-socket,server,nowait
)
RUN_SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$RUN_SCRIPT_DIR/.." && pwd)"
[ -z "$THEOS_QEMU_SERIAL_LOG" ] && THEOS_QEMU_SERIAL_LOG="$REPO_ROOT/Build/serial.log"
if [ "$THEOS_QEMU_SERIAL" = "1" ]; then
	echo "[run] serial log file: $THEOS_QEMU_SERIAL_LOG (override with THEOS_QEMU_SERIAL_LOG=...)"
	SERIAL_ARGS=(
		-chardev "stdio,id=char0,mux=on,logfile=$THEOS_QEMU_SERIAL_LOG,signal=off"
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

DISPLAY_ARGS=(
	-display "$THEOS_QEMU_DISPLAY"
)

MACHINE_ARGS=(
	-machine "$THEOS_QEMU_MACHINE"
)

AUDIO_ARGS=()
if [ "$THEOS_QEMU_AUDIO" = "1" ]; then
	AUDIODEV_ARG=""
	case "$THEOS_QEMU_AUDIO_BACKEND" in
		none|alsa|dbus|jack|oss|pa|pipewire|sdl|spice)
			AUDIODEV_ARG="$THEOS_QEMU_AUDIO_BACKEND,id=theos_audio0"
			;;
		wav|WAV)
			AUDIODEV_ARG="wav,id=theos_audio0,path=$THEOS_QEMU_AUDIO_WAV_PATH"
			;;
		*)
			echo "[run] invalid THEOS_QEMU_AUDIO_BACKEND='$THEOS_QEMU_AUDIO_BACKEND' (expected: none|alsa|dbus|jack|oss|pa|pipewire|sdl|spice|wav)" >&2
			exit 1
			;;
	esac

	AUDIO_ARGS=(
		-audiodev "$AUDIODEV_ARG"
		-device "ich9-intel-hda,id=hda"
		-device "hda-output,audiodev=theos_audio0"
	)
fi

NET_ARGS=()
case "$THEOS_QEMU_NET" in
	none|NONE|off|OFF|0)
		THEOS_QEMU_NET="none"
		NET_ARGS=(
			-net none
		)
		;;
	e1000e|E1000E|intel|INTEL|1)
		THEOS_QEMU_NET="e1000e"
		NET_ARGS=(
			-netdev "user,id=theos_net0"
			-device "e1000e,netdev=theos_net0,id=theos_nic0"
		)
		;;
	socket|SOCKET|mcast|MCAST|2)
		THEOS_QEMU_NET="socket"
		NET_ARGS=(
			-netdev "socket,id=theos_net0,mcast=$THEOS_QEMU_NET_SOCKET_MCAST"
			-device "e1000e,netdev=theos_net0,id=theos_nic0"
		)
		;;
	*)
		echo "[run] invalid THEOS_QEMU_NET='$THEOS_QEMU_NET' (expected: none|e1000e|socket)" >&2
		exit 1
		;;
esac

echo "[run] network device: $THEOS_QEMU_NET"
if [ "$THEOS_QEMU_NET" = "socket" ]; then
	echo "[run] network socket mcast: $THEOS_QEMU_NET_SOCKET_MCAST"
fi

NET_INJECT_PID=""
if [ "$THEOS_QEMU_NET" = "socket" ] && [ "$THEOS_QEMU_NET_INJECT_ON_BOOT" = "1" ]; then
	echo "[run] net inject on boot: enabled (delay_ms=$THEOS_QEMU_NET_INJECT_DELAY_MS count=$THEOS_QEMU_NET_INJECT_COUNT interval_ms=$THEOS_QEMU_NET_INJECT_INTERVAL_MS sig='$THEOS_QEMU_NET_INJECT_SIGNATURE')"
	python3 "$(dirname "$0")/net_inject.py" \
		--mcast "$THEOS_QEMU_NET_SOCKET_MCAST" \
		--delay-ms "$THEOS_QEMU_NET_INJECT_DELAY_MS" \
		--count "$THEOS_QEMU_NET_INJECT_COUNT" \
		--interval-ms "$THEOS_QEMU_NET_INJECT_INTERVAL_MS" \
		--signature "$THEOS_QEMU_NET_INJECT_SIGNATURE" &
	NET_INJECT_PID=$!
fi

qemu-system-x86_64 \
	-m $THEOS_RAM_SIZE \
	"${MACHINE_ARGS[@]}" \
	-cpu $THEOS_QEMU_CPU \
	-smp 4 \
	"${SERIAL_ARGS[@]}" \
	"${MONITOR_ARGS[@]}" \
	"${GDB_ARGS[@]}" \
	"${DISPLAY_ARGS[@]}" \
	"${GPU_ARGS[@]}" \
	"${AUDIO_ARGS[@]}" \
	"${NET_ARGS[@]}" \
	"${NUMA_ARGS[@]}" \
	"${KVM_ARGS[@]}" \
	"${BOOT_MEDIA_ARGS[@]}"

QEMU_STATUS=$?
if [ -n "$NET_INJECT_PID" ]; then
	wait "$NET_INJECT_PID" 2>/dev/null || true
fi

exit $QEMU_STATUS
