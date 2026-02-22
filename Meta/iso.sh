#!/bin/bash
set -euo pipefail

[ -z "${THEOS_DISK_NAME:-}" ] && THEOS_DISK_NAME="disk.img"
[ -z "${THEOS_EMBED_DISK_IN_ISO:-}" ] && THEOS_EMBED_DISK_IN_ISO=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

grub-file --is-x86-multiboot2 Kernel/Kernel || exit 1

rm -rf iso/
mkdir -p iso/boot/grub
cp Kernel/Boot/grub.cfg iso/boot/grub/
cp Kernel/Kernel iso/boot/TheOS

if [ "$THEOS_EMBED_DISK_IN_ISO" = "1" ]; then
	echo "[iso] preparing embedded root disk '$THEOS_DISK_NAME'"
	"$SCRIPT_DIR/disk.sh"
fi

if [ "$THEOS_EMBED_DISK_IN_ISO" = "1" ] && [ -f "$THEOS_DISK_NAME" ]; then
	echo "[iso] building hybrid ISO with embedded ext4 partition"
	grub-mkrescue -o TheOS.iso iso/ -- \
		-append_partition 4 0x83 "$THEOS_DISK_NAME" \
		-boot_image any appended_part_as=gpt
else
	grub-mkrescue -o TheOS.iso iso/
fi

exit 0
