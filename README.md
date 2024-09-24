# TheOS-Reborn
TheOS is an OS written by me (CodeMajorGeek alias Théo Colinmaire).
This is the repository of the 64 bits version of TheOS reworked completly from my last attempt.

# How to compile and test:
First gcc, binutils, make, cmake, ninja, libmpc, grub2 & qemu are required.

On Ubuntu :
    sudo apt install gcc binutils make cmake ninja-build libmpc-dev qemu grub2

TheOS uses a cross-compiler located in the folder Toolchain.
To build it, just run build.sh in the Toolchain folder and wait (pretty long time).

On Ubuntu :
    ./build.sh

## Things I want in TheOS-Reborn:
- **HID support (keyboard and mouse).**
- **Storage support (AHCI, SATA...).**
- **Have a good LibC implementation for future user space.**
- **Multitasking support (tasks).**
- **User-mode ring3 support, syscalls (TSSs).**
- **PCI IDE Controller support for storage.**

## Ingoing things:
- **Kernel programming.**
- **CLib programming.**

## Check list:
- **Kernel loader in x86_64 long mode (using multiboot2 and assembly).**
- **TTY terminal (crapy one, low resolution VGA text mode).**
- **Interrupt Managment (IRQs & ISRs)**
- **Memory managment (virtual memory managment, pagging...).**