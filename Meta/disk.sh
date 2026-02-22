#!/bin/bash
set -euo pipefail

[ -z "${THEOS_DISK_SIZE:-}" ] && THEOS_DISK_SIZE=512M

[ -z "${THEOS_DISK_NAME:-}" ] && THEOS_DISK_NAME="disk.img"

[ -z "${THEOS_BASE_FOLDER:-}" ] && THEOS_BASE_FOLDER="../Base"
[ -z "${THEOS_USERLAND_APP:-}" ] && THEOS_USERLAND_APP="Userland/Apps/TheApp/TheApp"

if [ ! -f "$THEOS_USERLAND_APP" ] && [ -f "Build/Userland/Apps/TheApp/TheApp" ]; then
	THEOS_USERLAND_APP="Build/Userland/Apps/TheApp/TheApp"
fi

echo "[disk] create image '$THEOS_DISK_NAME' size=$THEOS_DISK_SIZE"
qemu-img create -f raw "$THEOS_DISK_NAME" "$THEOS_DISK_SIZE"

# Keep ext4 features limited to what the in-kernel driver currently supports.
echo "[disk] format ext4 (compat profile)"
mkfs.ext4 -F -O ^has_journal,^64bit,^metadata_csum "$THEOS_DISK_NAME"
tune2fs -c0 -i0 "$THEOS_DISK_NAME"

mkdir -p tmp/

echo "[disk] mount image"
sudo mount "$THEOS_DISK_NAME" tmp/
if [ -d "$THEOS_BASE_FOLDER" ]; then
	sudo cp -r "$THEOS_BASE_FOLDER"/* tmp/
fi
sudo mkdir -p tmp/bin

if [ -f "$THEOS_USERLAND_APP" ]; then
	echo "[disk] install TheApp -> /bin/TheApp from '$THEOS_USERLAND_APP'"
	sudo cp "$THEOS_USERLAND_APP" tmp/bin/TheApp
else
	echo "[disk] warning: TheApp binary not found at '$THEOS_USERLAND_APP'"
fi

echo "[disk] sync and unmount image"
sudo sync
sudo umount tmp/

rmdir tmp/

echo "[disk] done"

exit 0
