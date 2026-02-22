#!/bin/bash
set -euo pipefail

[ -z "${THEOS_DISK_SIZE:-}" ] && THEOS_DISK_SIZE=512M

[ -z "${THEOS_DISK_NAME:-}" ] && THEOS_DISK_NAME="disk.img"

[ -z "${THEOS_BASE_FOLDER:-}" ] && THEOS_BASE_FOLDER="../Base"
[ -z "${THEOS_USERLAND_APP:-}" ] && THEOS_USERLAND_APP="Userland/Apps/TheApp/TheApp"
[ -z "${THEOS_USERLAND_SHELL:-}" ] && THEOS_USERLAND_SHELL="Userland/Apps/TheShell/TheShell"


if [ ! -f "$THEOS_USERLAND_APP" ] && [ -f "Build/Userland/Apps/TheApp/TheApp" ]; then
	THEOS_USERLAND_APP="Build/Userland/Apps/TheApp/TheApp"
fi
if [ ! -f "$THEOS_USERLAND_SHELL" ] && [ -f "Build/Userland/Apps/TheShell/TheShell" ]; then
	THEOS_USERLAND_SHELL="Build/Userland/Apps/TheShell/TheShell"
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

echo "[disk] create image '$THEOS_DISK_NAME' size=$THEOS_DISK_SIZE"
qemu-img create -f raw "$THEOS_DISK_NAME" "$THEOS_DISK_SIZE"

# Keep ext4 features limited to what the in-kernel driver currently supports.
echo "[disk] format ext4 (compat profile) and populate tree"
mkfs.ext4 -F -d "$STAGE_DIR" -O ^has_journal,^64bit,^metadata_csum "$THEOS_DISK_NAME"
tune2fs -c0 -i0 "$THEOS_DISK_NAME"

echo "[disk] done"

exit 0
