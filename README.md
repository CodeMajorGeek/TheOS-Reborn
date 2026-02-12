# TheOS-Reborn
TheOS-Reborn is the 64-bit rewrite of my previous TheOS attempt.

## Current status
- x86_64 kernel boot (Multiboot2 + long mode).
- Basic memory management (PMM/VMM).
- Interrupt handling (IDT/ISR/IRQ).
- APIC/IOAPIC setup with MADT parsing and IRQ override flags handling (polarity/trigger).
- SMP bring-up (BSP + AP startup with INIT/SIPI).
- AP idle loop (`sti; hlt`), IPI PING/PONG, and TLB shootdown validation.
- LAPIC timer calibrated with HPET and enabled on BSP + APs.
- Syscall foundation with per-CPU kernel stack/TSS integration.
- SMP scheduler phases B1/B2/B3 (per-CPU runqueue, push balancing, work stealing with backoff).
- AHCI/SATA disk init and basic ext4 interactions.
- HPET support enabled and used as LAPIC calibration source.

## Known limitations
- APIC mapping currently targets a practical xAPIC/QEMU workflow; x2APIC-scale topologies are not fully addressed yet.
- Scheduler remains a bring-up implementation (no full process scheduler/userland scheduling yet).
- Scheduler validation is currently focused on in-kernel stress tests run at boot.

## Prerequisites
On Ubuntu/Debian:
```bash
sudo apt install gcc binutils make cmake ninja-build libmpc-dev qemu-system-x86 grub-pc-bin xorriso
```

## Build cross toolchain
The project uses a dedicated cross-compiler in `Toolchain/Local`.

```bash
cd Toolchain
./build.sh
```

## Build kernel and ISO
From repository root:
```bash
cmake -S . -B Build -G Ninja
ninja -C Build -j"$(nproc)"
ninja -C Build iso -j"$(nproc)"
```

## Useful Ninja targets
From repository root:
```bash
# Build + install + create ISO + run QEMU
ninja -C Build install-run

# Create/update disk image
ninja -C Build create-disk
# If you used "create-image" before, the equivalent target is create-disk.

# Build ISO then run QEMU
ninja -C Build run
```

Environment variables supported by `Meta/run.sh`:
- `THEOS_RAM_SIZE` (default: `128M`)
- `THEOS_QEMU_CPU` (default: `max`)
- `THEOS_DISK_NAME` (default: `disk.img`)

Serial output:
- `ninja -C Build run` writes to `Build/serial.log`.
- Running `Meta/run.sh` directly writes to `serial.log` in the current working directory.
