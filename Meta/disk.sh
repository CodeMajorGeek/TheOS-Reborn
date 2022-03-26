#!/bin/bash

[ -z "$THEOS_DISK_SIZE" ] && THEOS_DISK_SIZE=512M

[ -z "$THEOS_DISK_NAME" ] && THEOS_DISK_NAME="disk.img"

qemu-img create $THEOS_DISK_NAME $THEOS_DISK_SIZE

exit 0