#!/bin/bash

[ -z "$THEOS_DISK_SIZE" ] && THEOS_DISK_SIZE=512M

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

[ -z "$THEOS_BASE_FOLDER" ] && THEOS_BASE_FOLDER="../Base"

qemu-img create -f raw $THEOS_DISK_NAME $THEOS_DISK_SIZE

mkfs.ext4 $THEOS_DISK_NAME
tune2fs -c0 -i0 $THEOS_DISK_NAME

mkdir tmp/

sudo mount $THEOS_DISK_NAME tmp/
sudo cp -r $THEOS_BASE_FOLDER/* tmp/
sudo umount tmp/

sudo rm -rd tmp/

exit 0