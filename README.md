# TheOS-Reborn
TheOS-Reborn is a freestanding x86_64 OS project (Multiboot2 kernel, custom VMM/PMM, SMP, ext4, ring3 userland, minimal libc, MicroPython port).

This README reflects the repository state as of **March 3, 2026**.

## Status Snapshot
### Working
- Multiboot2 boot + long mode + higher-half kernel (`0xFFFFFFFF80000000` base).
- Virtual memory layout split.
- Lower half: user space (`<= 0x00007FFFFFFFFFFF`).
- Higher half: kernel/HHDM/MMIO (`>= 0xFFFF800000000000`).
- Kernel mappings are supervisor-only; user mappings are explicitly `USER_MODE`.
- Startup identity map is kept during early bring-up, then dropped after SMP online.
- HHDM is globally fixed and shared on all CPUs (same virtual layout everywhere).
- MMIO mappings are explicit and uncached through dedicated MMIO window (`VMM_map_mmio_uc_*`).
- NX detection + enable path; W^X checks in ELF loader, `SYS_MAP`, and `SYS_MPROTECT`.
- APIC/IOAPIC + IRQ routing from ACPI MADT overrides; LAPIC timer calibrated by HPET.
- SMP bring-up (INIT/SIPI), AP online, per-CPU scheduler queues, TLB shootdown.
- AHCI storage with IRQ modes (MSI-X, MSI, fallback polling/INTx path).
- ext4 mount from AHCI, with boot-device preference from Multiboot2 `bootdev` hint.
- Ring3 ELF launch at boot (`/bin/TheApp`), then process syscalls (`fork/execve/waitpid/kill`).
- Framebuffer console path.
- Early VGA text init.
- Deferred switch to framebuffer after PSF2 font load.
- PSF2 text rendering, scroll, backspace, blinking cursor.
- Double buffering enabled when backbuffer allocation succeeds.
- Userland apps integrated in disk image: `TheApp`, `TheShell`, `TheTest`, `TheMicroPython`, plus alias `MicroPython`.
- Kernel debug logs.
- Serial sink (optional).
- File sink (optional), buffered in RAM until ext4 is mounted.

### Partially Working / Experimental
- x2APIC support exists but is behind `THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL` (default OFF).
- NUMA detection (SRAT/SLIT) is present, but no NUMA-aware allocator/scheduler policy yet.
- Process scheduling is syscall-driven and minimal (no COW, no full Unix process model).

### Known Gaps
- ext4 implementation is intentionally limited.
- No journal support path.
- Limited feature coverage.
- Write path constrained (single-block-oriented file updates in current syscall layer).
- libc is partial (enough for current apps/porting work, not full POSIX libc).
- No userland threading API/runtime yet.
- Signals are minimal: fault-to-signal translation + `SIGKILL` through `SYS_KILL`; no full signal handlers.
- MicroPython port is usable for REPL/bootstrap work, but script execution still has unresolved limitations (example seen in logs: `OSError: 12`).

## Architecture Notes
### Virtual Memory Layout
- `VMM_HHDM_BASE = 0xFFFF800000000000`
- `VMM_MMIO_BASE = 0xFFFFC00000000000`
- `VMM_KERNEL_VIRT_BASE = 0xFFFFFFFF80000000`
- `VMM_USER_SPACE_MAX = 0x00007FFFFFFFFFFF`

### Isolation/Security Model
- Each user process runs in its own lower-half address space (own `CR3`).
- Kernel higher-half is mirrored and supervisor-only in user process page tables.
- `SYS_MAP/SYS_UNMAP/SYS_MPROTECT` are restricted to a dedicated user mmap window.
- Kernel mappings and other process spaces are blocked from user mmap APIs.
- W^X policy enforced: writable+executable mappings are rejected.
- NX used for non-executable mappings when CPU supports NX.
- User entry stack follows SysV ABI shape (`argc`, `argv[]`, `envp[]`) for `main(argc, argv, envp)`.

## Build Prerequisites
Ubuntu/Debian packages:

```bash
sudo apt install gcc binutils make cmake ninja-build libmpc-dev qemu-system-x86 grub-pc-bin xorriso
```

## Cross Toolchain
The project uses a dedicated cross toolchain in `Toolchain/Local/x86_64`.

```bash
cd Toolchain
./build.sh
```

## Configure And Build
From repository root:

```bash
cmake -S . -B Build -G Ninja
ninja -C Build -j"$(nproc)"
```

Useful targets:

```bash
ninja -C Build create-disk   # build/populate ext4 disk image
ninja -C Build iso           # build bootable ISO
ninja -C Build run           # run QEMU
ninja -C Build install-run   # install + iso + run
```

## Main CMake Options
Top-level:
- `THEOS_QEMU_NUMA_DEFAULT` (default `ON`)
- `THEOS_KERNEL_FS_DISK_IMG` (default `OFF`)

`THEOS_KERNEL_FS_DISK_IMG` behavior:
- `OFF`: ext4 root is embedded into `TheOS.iso` by `iso` target.
- `ON`: ext4 root comes from external `disk.img` (`create-disk` required).

Kernel:
- `THEOS_ENABLE_KDEBUG` (default `ON`)
- `KERNEL_DEBUG_LOG_SERIAL` (default `ON`)
- `KERNEL_DEBUG_LOG_FILE` (default `ON`)
- `THEOS_ENABLE_SCHED_TESTS` (default `OFF`)
- `THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL` (default `OFF`)

Examples:

```bash
# Use external disk.img as root fs source (instead of embedded ext4 partition in ISO)
cmake -S . -B Build -G Ninja -DTHEOS_KERNEL_FS_DISK_IMG=ON

# Re-enable scheduler stress tests at boot
cmake -S . -B Build -G Ninja -DTHEOS_ENABLE_SCHED_TESTS=ON
```

## Runtime Options (`Meta/run.sh`)
Environment variables:
- `THEOS_RAM_SIZE` (default `128M`)
- `THEOS_QEMU_CPU` (default `max`)
- `THEOS_QEMU_GPU` (`vga` default, or `virtio`)
- `THEOS_DISK_NAME` (default `disk.img`)
- `THEOS_BOOT_FROM_ISO_DISK` (`1` default, `0` for external disk image)
- `THEOS_QEMU_NUMA` (`0` default in script)
- `THEOS_NUMA_NODE0_MEM`, `THEOS_NUMA_NODE1_MEM` (when NUMA enabled)

Notes:
- `ninja -C Build run` can set NUMA ON by default via `THEOS_QEMU_NUMA_DEFAULT`.
- GRUB is configured for a single, hidden, zero-timeout framebuffer entry.

## Disk Image Content
`Meta/disk.sh` stages `Base/` and installs:
- `/bin/TheApp`
- `/bin/TheShell`
- `/bin/TheTest`
- `/bin/TheMicroPython`
- `/bin/MicroPython` (alias copy)

Keyboard/font base files:
- `/system/keyboard.conf`
- `/system/azerty.conf`
- `/system/fonts/ter-powerline-v14n.psf` (default runtime font path)

## Userland Apps
### TheApp
- Boot userland entry app.
- Prints hello message, then `execve("/bin/TheShell", ...)`.

### TheShell
- Built-in commands: `pwd`, `cd`, `ls`, `cat`, `touch`, `mkdir`, `echo <text> | <path>`, `help`, `exit`.
- Executes binaries from `/bin` (supports aliases like `test` -> `/bin/TheTest`).
- Uses libc keyboard/scancode layer with configurable layout (`/system/keyboard.conf` -> `azerty.conf`).

### TheTest
- Security/regression test app for syscall behavior.
- Includes mmap boundary tests, unmap edge cases, fork/wait race scenarios, and fault probes.

### TheMicroPython
- Built as a standalone userland app from `ports/theos` (not embedded in another app).
- Supports REPL and argument parsing (`--help`, script argument path).
- Integrated with TheOS keyboard input and Ctrl+C polling hook.

## Syscall Surface (Current)
Current syscall enum range: `1..27`.

Filesystem/IO:
- `SYS_FS_LS`, `SYS_FS_READ`, `SYS_FS_CREATE`, `SYS_FS_WRITE`
- `SYS_OPEN`, `SYS_CLOSE`, `SYS_FS_SEEK`, `SYS_FS_ISDIR`, `SYS_FS_MKDIR`
- `SYS_CONSOLE_WRITE`, `SYS_KBD_GET_SCANCODE`

Timing/system info:
- `SYS_SLEEP_MS`, `SYS_TICK_GET`, `SYS_CPU_INFO_GET`, `SYS_SCHED_INFO_GET`
- `SYS_AHCI_IRQ_INFO_GET`, `SYS_RCU_SYNC`, `SYS_RCU_INFO_GET`

Process/memory:
- `SYS_EXIT`, `SYS_FORK`, `SYS_EXECVE`, `SYS_WAITPID`, `SYS_KILL`, `SYS_YIELD`
- `SYS_MAP`, `SYS_UNMAP`, `SYS_MPROTECT`

## LibC Status
- Current userland libc focus: syscall wrappers, stdio/printf subset, string/memory helpers, basic errno, minimal headers for porting.
- Keyboard input path is in libc stdio (`getchar/fgets`) with scancode parsing, Shift/Caps/AltGr, and layout file loading.
- `strings.h` currently includes case-insensitive helpers (`strcasecmp`, `strncasecmp`).
- Headers like `math.h`, `errno.h`, `stddef.h`, `limits.h` were expanded for compatibility work (MicroPython bring-up).
- Still incomplete vs POSIX libc.
- Many APIs are missing or only declared (example: several `unistd.h` functions are declared but not yet implemented in libc sources).
- No userland heap allocator (`malloc/free/realloc/calloc`) in current libc implementation.

## Logging
- Serial run logs are written to `Build/serial.log` when using `ninja -C Build run`.
- Kernel debug file sink stores logs in RAM first, then flushes to ext4 once FS is ready.
- File sink output files: `kdebug.log`, `kdebug.log.<n>` (extra chunks), and optional `kdebug.log.overflow` on RAM buffer overflow before flush.

## Current Priorities
- Continue hardening process/memory/filesystem semantics.
- Expand libc coverage for larger userland ports.
- Stabilize MicroPython script execution path.
- Extend ext4 write capabilities beyond current single-block-oriented constraints.
- Move toward richer userland scheduling/threading model.
