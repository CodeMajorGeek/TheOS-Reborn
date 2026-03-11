#!/bin/bash
set -euo pipefail

[ -z "${THEOS_DISK_NAME:-}" ] && THEOS_DISK_NAME="disk.img"
[ -z "${THEOS_EMBED_DISK_IN_ISO:-}" ] && THEOS_EMBED_DISK_IN_ISO=1
[ -z "${THEOS_ISO_NAME:-}" ] && THEOS_ISO_NAME="TheOS.iso"
[ -z "${THEOS_LIMINE_DIR:-}" ] && THEOS_LIMINE_DIR=".cache/limine"
[ -z "${THEOS_LIMINE_REF:-}" ] && THEOS_LIMINE_REF="v10.x-binary"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ ! -d "$THEOS_LIMINE_DIR/.git" ]; then
	echo "[iso] fetching limine ($THEOS_LIMINE_REF) into '$THEOS_LIMINE_DIR'"
	rm -rf "$THEOS_LIMINE_DIR"
	git clone --depth=1 --branch "$THEOS_LIMINE_REF" https://github.com/limine-bootloader/limine.git "$THEOS_LIMINE_DIR"
fi

if [ ! -x "$THEOS_LIMINE_DIR/limine" ]; then
	echo "[iso] building limine host tool"
	make -C "$THEOS_LIMINE_DIR"
fi

LIMINE_BIN="$THEOS_LIMINE_DIR/limine"
LIMINE_BIOS_CD="$THEOS_LIMINE_DIR/limine-bios-cd.bin"
LIMINE_BIOS_SYS="$THEOS_LIMINE_DIR/limine-bios.sys"
LIMINE_UEFI_CD="$THEOS_LIMINE_DIR/limine-uefi-cd.bin"
LIMINE_EFI_X64="$THEOS_LIMINE_DIR/BOOTX64.EFI"

for f in "$LIMINE_BIN" "$LIMINE_BIOS_CD" "$LIMINE_BIOS_SYS" "$LIMINE_UEFI_CD" "$LIMINE_EFI_X64"; do
	if [ ! -f "$f" ]; then
		echo "[iso] missing limine artifact: $f" >&2
		exit 1
	fi
done

rm -rf iso/
mkdir -p iso/boot/limine iso/EFI/BOOT

cp Kernel/Kernel iso/boot/TheOS
cp "$PROJECT_ROOT/Kernel/Boot/limine.conf" iso/boot/limine/limine.conf
cp "$LIMINE_BIOS_SYS" iso/boot/limine/
cp "$LIMINE_BIOS_CD" iso/boot/limine/
cp "$LIMINE_UEFI_CD" iso/boot/limine/
cp "$LIMINE_EFI_X64" iso/EFI/BOOT/BOOTX64.EFI

if [ "$THEOS_EMBED_DISK_IN_ISO" = "1" ]; then
	echo "[iso] preparing embedded root disk '$THEOS_DISK_NAME'"
	"$SCRIPT_DIR/disk.sh"
else
	echo "[iso] embedded root disk disabled (THEOS_EMBED_DISK_IN_ISO=0)"
fi

XORRISO_ARGS=(
	-as mkisofs
	-R
	-r
	-J
	-joliet-long
	-l
	-b boot/limine/limine-bios-cd.bin
	-no-emul-boot
	-boot-load-size 4
	-boot-info-table
	--efi-boot boot/limine/limine-uefi-cd.bin
	-efi-boot-part
	--efi-boot-image
	--protective-msdos-label
)

if [ "$THEOS_EMBED_DISK_IN_ISO" = "1" ] && [ -f "$THEOS_DISK_NAME" ]; then
	echo "[iso] appending ext4 disk image into ISO"
	XORRISO_ARGS+=(
		-append_partition 4 0x83 "$THEOS_DISK_NAME"
	)
fi

XORRISO_ARGS+=(
	iso
	-o "$THEOS_ISO_NAME"
)

xorriso "${XORRISO_ARGS[@]}"

if [ "$THEOS_EMBED_DISK_IN_ISO" = "1" ]; then
	BIOS_GPT_PART="$(sgdisk -p "$THEOS_ISO_NAME" 2>/dev/null | awk '/^[[:space:]]*[0-9]+/{n=$1} END{print n}')"
	if [ -n "$BIOS_GPT_PART" ]; then
		"$LIMINE_BIN" bios-install --force "$THEOS_ISO_NAME" "$BIOS_GPT_PART"
	else
		"$LIMINE_BIN" bios-install --force "$THEOS_ISO_NAME"
	fi
else
	"$LIMINE_BIN" bios-install "$THEOS_ISO_NAME"
fi

echo "[iso] built $THEOS_ISO_NAME"
exit 0
