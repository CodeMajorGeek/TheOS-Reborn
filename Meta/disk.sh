#!/bin/bash
set -euo pipefail

[ -z "${THEOS_DISK_SIZE:-}" ] && THEOS_DISK_SIZE=512M

[ -z "${THEOS_DISK_NAME:-}" ] && THEOS_DISK_NAME="disk.img"

[ -z "${THEOS_BASE_FOLDER:-}" ] && THEOS_BASE_FOLDER="../Base"
[ -z "${THEOS_USERLAND_APP:-}" ] && THEOS_USERLAND_APP="Userland/Apps/TheApp/TheApp"
[ -z "${THEOS_USERLAND_SHELL:-}" ] && THEOS_USERLAND_SHELL="Userland/Apps/TheShell/TheShell"
[ -z "${THEOS_USERLAND_TEST:-}" ] && THEOS_USERLAND_TEST="Userland/Apps/TheTest/TheTest"
[ -z "${THEOS_USERLAND_POWERMANAGER:-}" ] && THEOS_USERLAND_POWERMANAGER="Userland/Apps/ThePowerManager/ThePowerManager"
[ -z "${THEOS_USERLAND_SYSTEMMONITOR:-}" ] && THEOS_USERLAND_SYSTEMMONITOR="Userland/Apps/TheSystemMonitor/TheSystemMonitor"
[ -z "${THEOS_USERLAND_MICROPY:-}" ] && THEOS_USERLAND_MICROPY="Userland/Apps/TheMicroPython/TheMicroPython"


if [ ! -f "$THEOS_USERLAND_APP" ] && [ -f "Build/Userland/Apps/TheApp/TheApp" ]; then
	THEOS_USERLAND_APP="Build/Userland/Apps/TheApp/TheApp"
fi
if [ ! -f "$THEOS_USERLAND_SHELL" ] && [ -f "Build/Userland/Apps/TheShell/TheShell" ]; then
	THEOS_USERLAND_SHELL="Build/Userland/Apps/TheShell/TheShell"
fi
if [ ! -f "$THEOS_USERLAND_TEST" ] && [ -f "Build/Userland/Apps/TheTest/TheTest" ]; then
	THEOS_USERLAND_TEST="Build/Userland/Apps/TheTest/TheTest"
fi
if [ ! -f "$THEOS_USERLAND_POWERMANAGER" ] && [ -f "Build/Userland/Apps/ThePowerManager/ThePowerManager" ]; then
	THEOS_USERLAND_POWERMANAGER="Build/Userland/Apps/ThePowerManager/ThePowerManager"
fi
if [ ! -f "$THEOS_USERLAND_SYSTEMMONITOR" ] && [ -f "Build/Userland/Apps/TheSystemMonitor/TheSystemMonitor" ]; then
	THEOS_USERLAND_SYSTEMMONITOR="Build/Userland/Apps/TheSystemMonitor/TheSystemMonitor"
fi
if [ ! -f "$THEOS_USERLAND_MICROPY" ] && [ -f "Build/Userland/Apps/TheMicroPython/TheMicroPython" ]; then
	THEOS_USERLAND_MICROPY="Build/Userland/Apps/TheMicroPython/TheMicroPython"
fi

STAGE_DIR="$(mktemp -d)"
cleanup() {
	rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

echo "[disk] prepare staged root tree in '$STAGE_DIR'"
if [ -d "$THEOS_BASE_FOLDER" ]; then
	cp -a "$THEOS_BASE_FOLDER"/. "$STAGE_DIR"/
fi
mkdir -p "$STAGE_DIR/bin"
mkdir -p "$STAGE_DIR/system/fonts"

if [ -f "$THEOS_USERLAND_APP" ]; then
	echo "[disk] install TheApp -> /bin/TheApp from '$THEOS_USERLAND_APP'"
	cp "$THEOS_USERLAND_APP" "$STAGE_DIR/bin/TheApp"
else
	echo "[disk] warning: TheApp binary not found at '$THEOS_USERLAND_APP'"
fi

if [ -f "$THEOS_USERLAND_SHELL" ]; then
	echo "[disk] install TheShell -> /bin/TheShell from '$THEOS_USERLAND_SHELL'"
	cp "$THEOS_USERLAND_SHELL" "$STAGE_DIR/bin/TheShell"
else
	echo "[disk] warning: TheShell binary not found at '$THEOS_USERLAND_SHELL'"
fi

if [ -f "$THEOS_USERLAND_TEST" ]; then
	echo "[disk] install TheTest -> /bin/TheTest from '$THEOS_USERLAND_TEST'"
	cp "$THEOS_USERLAND_TEST" "$STAGE_DIR/bin/TheTest"
else
	echo "[disk] warning: TheTest binary not found at '$THEOS_USERLAND_TEST'"
fi

if [ -f "$THEOS_USERLAND_POWERMANAGER" ]; then
	echo "[disk] install ThePowerManager -> /bin/ThePowerManager from '$THEOS_USERLAND_POWERMANAGER'"
	cp "$THEOS_USERLAND_POWERMANAGER" "$STAGE_DIR/bin/ThePowerManager"
else
	echo "[disk] warning: ThePowerManager binary not found at '$THEOS_USERLAND_POWERMANAGER'"
fi

if [ -f "$THEOS_USERLAND_SYSTEMMONITOR" ]; then
	echo "[disk] install TheSystemMonitor -> /bin/TheSystemMonitor from '$THEOS_USERLAND_SYSTEMMONITOR'"
	cp "$THEOS_USERLAND_SYSTEMMONITOR" "$STAGE_DIR/bin/TheSystemMonitor"
else
	echo "[disk] warning: TheSystemMonitor binary not found at '$THEOS_USERLAND_SYSTEMMONITOR'"
fi

if [ -f "$THEOS_USERLAND_MICROPY" ]; then
	echo "[disk] install TheMicroPython -> /bin/TheMicroPython and /bin/MicroPython from '$THEOS_USERLAND_MICROPY'"
	cp "$THEOS_USERLAND_MICROPY" "$STAGE_DIR/bin/TheMicroPython"
else
	echo "[disk] warning: TheMicroPython binary not found at '$THEOS_USERLAND_MICROPY'"
fi

echo "[disk] create image '$THEOS_DISK_NAME' size=$THEOS_DISK_SIZE"
qemu-img create -f raw "$THEOS_DISK_NAME" "$THEOS_DISK_SIZE"

# Keep ext4 features limited to what the in-kernel driver currently supports.
echo "[disk] format ext4 (compat profile) and populate tree"
mkfs.ext4 -F -d "$STAGE_DIR" -O ^has_journal,^64bit,^metadata_csum "$THEOS_DISK_NAME"
tune2fs -c0 -i0 "$THEOS_DISK_NAME"

echo "[disk] done"

exit 0
