# TheOS-Reborn
TheOS-Reborn is a 64-bit rewrite of my previous TheOS attempt.

## Current status
- x86_64 kernel boot (Multiboot2 + long mode).
- PMM/VMM bring-up.
- IDT/ISR/IRQ core handling.
- APIC/IOAPIC init with MADT IRQ override flags (polarity/trigger).
- SMP bring-up (INIT/SIPI), AP online, AP idle (`sti; hlt`).
- Per-CPU LAPIC timer enabled on BSP and APs, calibrated from HPET.
- SMP scheduler bring-up with per-CPU runqueue, push balancing, work stealing, backoff.
- Spinlocks with irqsave/irqrestore paths.
- IPI ping/pong and TLB shootdown validation tests.
- AVX YMM save/restore stress test across online CPUs at boot.
- AHCI/SATA + ext4 basic operations.
- AHCI IRQ via MSI (MSI-X/MSI fallback depending on device), INTx disabled when MSI is enabled.
- SSE/AVX init with lazy FPU switching (`CR0.TS` + `#NM`) and AVX-safe state save/restore (`xsave/xrstor`, CPUID leaf `0xD`, per-task aligned state).
- Kernel heap allocator (`kmalloc`/`krealloc`/`kfree`) protected for SMP with an irq-safe spinlock.
- NUMA detection via ACPI SRAT/SLIT (testable in QEMU with `-numa`).
- ELF64 userland loader (ext4 root file -> mapped user segments + ring3 jump).
- First userland app `TheApp` (`Hello World`) built and copied to disk image by `create-disk`.
- Userland mappings are in a dedicated lower-half range with `U/S=User`, while kernel mappings are `Supervisor-only`.

## Known limitations
- Scheduler is still a kernel bring-up scheduler, not a full user process scheduler yet.
- Userland execution currently targets one loaded ELF app at boot (`TheApp`).
- Some paths are still validated mostly through boot-time stress tests.
- NUMA-aware allocation/scheduling policies are not implemented yet (detection/mapping is done).

## Prerequisites
Ubuntu/Debian:

```bash
sudo apt install gcc binutils make cmake ninja-build libmpc-dev qemu-system-x86 grub-pc-bin xorriso
```

## Build cross toolchain
The project uses a dedicated cross-compiler in `Toolchain/Local`.

```bash
cd Toolchain
./build.sh
```

## Configure and build (Ninja)
From repository root:

```bash
cmake -S . -B Build -G Ninja
ninja -C Build -j"$(nproc)"
```

## Useful Ninja targets
From repository root:

```bash
# Build kernel + install + ISO + run QEMU
ninja -C Build install-run

# Create/update disk image
ninja -C Build create-disk
# "create-image" old name -> now "create-disk"

# Build/update ISO
ninja -C Build iso

# Run QEMU
ninja -C Build run
```

## QEMU runtime options (`Meta/run.sh`)
Supported environment variables:
- `THEOS_RAM_SIZE` (default: `128M`)
- `THEOS_QEMU_CPU` (default: `max`)
- `THEOS_DISK_NAME` (default: `disk.img`)
- `THEOS_QEMU_NUMA` (default: `0`, set `1` to enable two NUMA nodes)
- `THEOS_NUMA_NODE0_MEM` (default: `64M` when NUMA is enabled)
- `THEOS_NUMA_NODE1_MEM` (default: `64M` when NUMA is enabled)

Note:
- `Meta/run.sh` keeps `THEOS_QEMU_NUMA=0` by default when run directly.
- `ninja -C Build run` enables NUMA by default through CMake option `THEOS_QEMU_NUMA_DEFAULT=ON`.

Example NUMA run:

```bash
THEOS_QEMU_NUMA=1 THEOS_NUMA_NODE0_MEM=64M THEOS_NUMA_NODE1_MEM=64M ninja -C Build run
```

Disable NUMA-by-default for the `run` target:

```bash
cmake -S . -B Build -G Ninja -DTHEOS_QEMU_NUMA_DEFAULT=OFF
```

## Logs
- `ninja -C Build run` writes serial output to `Build/serial.log`.
- If you run `Meta/run.sh` manually from a different directory, serial output file/path depends on the current working directory.
