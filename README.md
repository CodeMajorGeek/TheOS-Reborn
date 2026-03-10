# TheOS-Reborn

![Arch](https://img.shields.io/badge/arch-x86__64-informational)
![Boot](https://img.shields.io/badge/boot-limine-blue)
![Build tool](https://img.shields.io/badge/build%20tool-ninja-informational)
![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Status](https://img.shields.io/badge/status-experimental-orange)

TheOS-Reborn is a freestanding x86_64 operating system project with a Limine boot path, custom PMM/VMM, SMP, ACPI/APIC, AHCI/ext4, ring3 userland, a minimal libc, and a MicroPython port.

> This README reflects repository behavior as of **March 10, 2026**.

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Stability](#stability)
- [Current Boot Sequence](#current-boot-sequence)
- [Boot Screenshot](#boot-screenshot)
- [Memory Model (Limine-first)](#memory-model-limine-first)
- [Architecture at a Glance](#architecture-at-a-glance)
- [Build & Run](#build--run)
- [Configuration Options](#configuration-options)
- [Disk Image & Userland](#disk-image--userland)
- [Syscalls](#syscalls)
- [Logging & Debugging](#logging--debugging)
- [Known Gaps](#known-gaps)
- [License](#license)
- [Roadmap](#roadmap)

---

## Overview

Project goals:

- Learn OS internals on real x86_64 abstractions.
- Keep a full boot-to-userland stack in one repository.
- Favor explicit low-level behavior over hidden magic.

Current stack includes:

- Limine native entry (`Boot/LimineEntry.c.S` -> `boot_limine_entry` -> `k_entry`).
- Higher-half kernel with runtime relocation support from Limine executable address response.
- PMM + VMM with HHDM and dedicated MMIO mapping window.
- ACPI initialization from Limine-provided RSDP (no legacy BIOS scan).
- APIC/IOAPIC + HPET/PIT + SMP bring-up.
- AHCI storage and ext4 root mount.
- Ring3 ELF launch (`/bin/TheApp`, then shell and apps).

![Rough component split](Docs/components.png)

To regenerate graphs:

```bash
ninja -C Build graphs
```

---

## Features

Core features currently implemented:

- Limine-native boot entry with higher-half kernel handoff.
- Limine-first boot metadata usage:
  - runtime kernel base relocation,
  - HHDM offset,
  - memory map ingestion,
  - RSDP ACPI initialization path,
  - framebuffer discovery/selection,
  - boot-media hints for root filesystem selection.
- Custom PMM/VMM stack with:
  - page-based physical allocator,
  - HHDM mapping policy,
  - kernel/user split,
  - startup identity map and later teardown,
  - dedicated MMIO mapping window.
- ACPI/APIC/IOAPIC stack:
  - MADT parsing,
  - IRQ override handling,
  - APIC enable path and IOAPIC routing.
- SMP bring-up:
  - AP startup and online tracking,
  - inter-CPU tests and TLB shootdown paths.
- Timing stack:
  - HPET initialization,
  - LAPIC timer calibration,
  - PIT fallback path.
- Storage and filesystem:
  - AHCI controller discovery and IRQ mode setup (MSI/MSI-X/legacy fallback),
  - ext4 root mount with preferred Limine hint path and controlled fallback probing.
- Console and graphics:
  - early VGA path,
  - PSF2 font loading,
  - deferred framebuffer switch,
  - optional double buffering.
- Userland/runtime:
  - ring3 ELF launch,
  - process syscalls (`fork/execve/waitpid/kill`),
  - shell, tests, power manager, and MicroPython app.

---

## Stability

`Status: experimental` means:

- Behavior is actively evolving and boot/runtime order may still change between commits.
- Backward compatibility (internal APIs, syscall details, build defaults) is not guaranteed.
- Subsystems can be individually stable while overall integration still has edge cases.
- Regressions are possible when low-level memory, SMP, storage, or interrupt code is modified.

What it does **not** mean:

- It is not a non-booting prototype. The current tree boots, mounts ext4, brings up SMP, and launches userland.
- It is not abandonware; active refactors and instrumentation are ongoing.

---

## Current Boot Sequence

The runtime order in `k_entry` is intentionally staged and observable in `Build/serial.log`:

1. **Limine loads kernel image** (`/boot/TheOS`) and jumps to Limine entrypoint.
2. **Assembly handoff** (`limine_entry` -> `boot_limine_entry`) switches to kernel stack and enters `k_entry`.
3. **Early debug sinks**: VGA TTY + logger + serial/file kdebug init.
4. **Limine base revision check** and runtime kernel base resolution (physical + virtual).
5. **PMM base init** with runtime kernel boundaries.
6. **Limine boot info parse**:
   - HHDM offset,
   - cmdline,
   - memory map (`memmap_request`),
   - RSDP pointer,
   - executable source hints (`mbr_disk_id`, partition index),
   - framebuffer discovery/selection.
7. **VMM map kernel**:
   - maps kernel image,
   - maps HHDM from Limine-derived boot entries,
   - keeps startup identity map during early bring-up.
8. **Load CR3**, **reload GDT**, enable NX path when supported.
9. **Early ACPI path from Limine RSDP** (RSDT/XSDT init), then MADT fetch and ACPI power init.
10. **IDT init** and BSP FPU init.
11. **APIC/IOAPIC/NUMA path** (if MADT + APIC available).
12. **PCI scan** (ACPI is already resolved before this stage).
13. **Keyboard + syscall init**.
14. **Root filesystem mount**:
   - prefer Limine boot-media hint (`mbr_disk_id_hint` + optional partition hint),
   - fallback to wider AHCI probing only if preferred path fails.
15. **Framebuffer activation deferred** until PSF2 font is loaded from ext4.
16. **Task init** then **SMP bring-up** of APs.
17. **Drop startup identity map** after SMP is online.
18. **Timer stack init** (HPET preferred for LAPIC calibration, PIT fallback).
19. **Enable interrupts**, start LAPIC timers on BSP/APs.
20. **Launch ring3 userland** (`/bin/TheApp` -> `TheShell`).

---

## Boot Screenshot

Early boot/runtime capture (serial + framebuffer path):

![Boot Screenshot](Docs/screenshot-tty-at-boot.png)

---

## Memory Model (Limine-first)

### Physical memory input source

The kernel now uses **Limine memmap as the source of truth**:

- `limine_memmap_request` entries are classified and stored in PMM boot entries.
- Each boot entry tracks:
  - physical range,
  - Limine memmap type,
  - flags: `allocatable` and `hhdm_map`.

### PMM policy

- PMM allocates pages only from entries considered allocatable (`LIMINE_MEMMAP_USABLE`).
- Low memory under `0x100000` is never managed by PMM.
- Kernel image pages are never returned by allocator.

### VMM/HHDM policy

- HHDM mappings are built from boot entries flagged `hhdm_map` (not only PMM-usable ranges).
- This allows safe access to needed non-allocatable ranges (for example ACPI data, framebuffer, bootloader reclaimable areas while still in early boot).
- If no boot entries are available, VMM falls back to legacy usable-region mapping.

### Virtual layout

- User canonical lower half up to `0x00007FFFFFFFFFFF`.
- Kernel/HHDM/MMIO in higher half.
- Default constants:
  - `VMM_HHDM_BASE = 0xFFFF800000000000`
  - `VMM_MMIO_BASE = 0xFFFFC00000000000`
  - `VMM_KERNEL_VIRT_BASE = 0xFFFFFFFF80000000`

ASCII view:

```text
0xFFFFFFFFFFFFFFFF  +-----------------------------------------------+
                    | Kernel higher-half mappings                    |
0xFFFFFFFF80000000  +-----------------------------------------------+  VMM_KERNEL_VIRT_BASE
                    | MMIO window (uncached mappings)               |
0xFFFFC00000000000  +-----------------------------------------------+  VMM_MMIO_BASE
                    | HHDM direct map (phys + offset)               |
0xFFFF800000000000  +-----------------------------------------------+  VMM_HHDM_BASE
                    | non-canonical gap                             |
0x0000800000000000  +-----------------------------------------------+
                    | User virtual address space                    |
0x0000000000000000  +-----------------------------------------------+
```

---

## Architecture at a Glance

### Boot pipeline

```mermaid
flowchart TD
    A["Limine"] --> B["LimineEntry.c.S"]
    B --> C["Bootloader.S / boot_limine_entry"]
    C --> D["k_entry (Entry.c)"]
    D --> E["LimineHelper: memmap/HHDM/RSDP/fb"]
    E --> F["PMM + VMM map kernel/HHDM"]
    F --> G["ACPI early (RSDP -> MADT)"]
    G --> H["APIC/IOAPIC + PCI + AHCI/ext4"]
    H --> I["SMP + timers + userland launch"]
```

### CPU and interrupt subsystem

- APIC/IOAPIC configured from ACPI MADT.
- IRQ overrides handled from MADT records.
- MSI/MSI-X path for AHCI when available.
- HPET-backed LAPIC calibration preferred; PIT fallback remains available.

### Userland model

- Ring3 ELF process start.
- `fork/execve/waitpid/kill/yield` path implemented.
- Shell-centric workflow with additional user apps (`TheTest`, `ThePowerManager`, `TheMicroPython`).

---

## Build & Run

### Prerequisites (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y \
  gcc binutils make cmake ninja-build git \
  libmpc-dev qemu-system-x86 xorriso mtools
```

### Cross toolchain

```bash
cd Toolchain
./build.sh
```

### Configure and build

```bash
mkdir -p Build
cd Build
cmake .. -GNinja
ninja install-run
```

Useful targets:

```bash
ninja create-disk   # build/populate ext4 disk image
ninja iso           # build bootable ISO
ninja run           # run QEMU with current ISO
ninja install-run   # install + iso + run
ninja graphs        # regenerate project graphs
```

---

## Configuration Options

### Top-level CMake options

- `THEOS_HARDWARE_TEST_PROFILE` (default `OFF`)
  - when `ON`, forces hardware-like defaults (serial off, embedded disk path, no gdb/telnet monitor).
- `THEOS_QEMU_NUMA_DEFAULT` (default `ON`)
- `THEOS_KERNEL_FS_DISK_IMG` (default `OFF`)
  - `OFF`: ext4 embedded inside ISO.
  - `ON`: external `disk.img` rootfs.
- `THEOS_ENABLE_KVM` (default `ON`)
- `THEOS_RUN_SERIAL_CONSOLE` (default `ON`)
- `THEOS_RUN_GDB_STUB` (default `OFF`)
- `THEOS_RUN_TELNET_MONITOR` (default `OFF`)
- `THEOS_AUTO_PULL_SUBMODULES` (default `ON`)
  - auto-updates required submodules (currently `EasyArgs`) during configure.

### Kernel CMake options

- `THEOS_ENABLE_KDEBUG` (default `ON`)
- `KERNEL_DEBUG_LOG_SERIAL` (default `ON`)
- `KERNEL_DEBUG_LOG_FILE` (default `ON`)
- `THEOS_ENABLE_SCHED_TESTS` (default `OFF`)
- `THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL` (default `OFF`)

### Runtime options (`Meta/run.sh`)

Main environment controls:

- `THEOS_RAM_SIZE` (default `128M`)
- `THEOS_QEMU_CPU` (default `max`)
- `THEOS_QEMU_GPU` (`vga` or `virtio`, default `vga`)
- `THEOS_QEMU_NUMA` (`0`/`1`)
- `THEOS_QEMU_KVM` (`0`/`1`)
- `THEOS_QEMU_SERIAL` (`0`/`1`)
- `THEOS_QEMU_GDB_STUB` (`0`/`1`)
- `THEOS_QEMU_TELNET_MONITOR` (`0`/`1`)
- `THEOS_BOOT_FROM_ISO_DISK` (`1` embedded disk in ISO, `0` external disk image)

Limine config (`Kernel/Boot/limine.conf`) currently enables serial output:

```ini
serial: yes
serial_baudrate: 115200
```

---

## Disk Image & Userland

`Meta/disk.sh` stages `Base/` and installs:

- `/bin/TheApp`
- `/bin/TheShell`
- `/bin/TheTest`
- `/bin/ThePowerManager`
- `/bin/TheMicroPython`
- `/bin/MicroPython` (alias)

Runtime resources:

- `/system/keyboard.conf`
- `/system/azerty.conf`
- `/system/fonts/ter-powerline-v14n.psf`

### Root filesystem selection policy

At boot, root mount does:

1. Try Limine executable-file hint (`mbr_disk_id_hint`, optional partition hint).
2. If that path fails, fallback probe on remaining AHCI devices/partitions.

---

## Syscalls

Current public syscall IDs are `1..27` (`Includes/UAPI/Syscall.h`).

- `1` `SYS_SLEEP_MS`
- `2` `SYS_TICK_GET`
- `3` `SYS_CPU_INFO_GET`
- `4` `SYS_SCHED_INFO_GET`
- `5` `SYS_AHCI_IRQ_INFO_GET`
- `6` `SYS_RCU_SYNC`
- `7` `SYS_RCU_INFO_GET`
- `8` `SYS_CONSOLE_WRITE`
- `9` `SYS_EXIT`
- `10` `SYS_FORK`
- `11` `SYS_EXECVE`
- `12` `SYS_YIELD`
- `13` `SYS_MAP`
- `14` `SYS_UNMAP`
- `15` `SYS_MPROTECT`
- `16` `SYS_OPEN`
- `17` `SYS_CLOSE`
- `18` `SYS_READ`
- `19` `SYS_WRITE`
- `20` `SYS_LSEEK`
- `21` `SYS_KBD_GET_SCANCODE`
- `22` `SYS_FS_ISDIR`
- `23` `SYS_FS_MKDIR`
- `24` `SYS_FS_READDIR`
- `25` `SYS_WAITPID`
- `26` `SYS_KILL`
- `27` `SYS_POWER`

---

## Logging & Debugging

### Serial path

- Limine serial output is enabled in `limine.conf`.
- Kernel `kdebug` serial sink is enabled by default (`KERNEL_DEBUG_LOG_SERIAL=ON`).
- `Meta/run.sh` writes QEMU serial logs to `Build/serial.log` when serial console is enabled.

### File sink

- Kernel logs are buffered in RAM first.
- Once ext4 is mounted, buffered logs are flushed to files such as:
  - `kdebug.log`
  - `kdebug.log.<n>`
  - `kdebug.log.overflow` (if pre-flush overflow happened)

### Debug helpers

- GDB stub: enable `THEOS_RUN_GDB_STUB=ON`.
- Telnet monitor: enable `THEOS_RUN_TELNET_MONITOR=ON`.

---

## Known Gaps

- ext4 implementation remains intentionally limited (not full production ext4 feature set).
- libc is partial and targeted to current apps/ports.
- No complete POSIX process/thread model yet (no COW, no pthread runtime).
- Signal model is still minimal.
- Some components (x2APIC SMP mode, parts of scheduler stress paths) are experimental.

---

## License

- This repository ships with the GNU General Public License version 3 text in [`LICENSE`](LICENSE).
- In practice, treat TheOS-Reborn core code as **GPLv3**.
- Some bundled/third-party components (for example inside imported projects) may carry their own licenses; check their headers and upstream license files when redistributing.

---

## Roadmap

- Keep hardening memory and process semantics.
- Extend filesystem write-path coverage.
- Expand libc coverage for larger userland compatibility.
- Continue stabilizing MicroPython script execution behavior.
- Improve scheduling and multi-process runtime capabilities.
