#!/bin/bash

grub-file --is-x86-multiboot2 Kernel/Kernel || exit 1

rm -rf iso/
mkdir -p iso/boot/grub
cp Kernel/Boot/grub.cfg iso/boot/grub/
cp Kernel/Kernel iso/boot/TheOS
grub-mkrescue -o TheOS.iso iso/

exit 0
