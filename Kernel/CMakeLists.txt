set(KERNEL_ARCH x86_64)
option(THEOS_ENABLE_KDEBUG "Enable serial KDEBUG output and printf mirroring" ON)
message(STATUS "Kernel: THEOS_ENABLE_KDEBUG=${THEOS_ENABLE_KDEBUG}")

set(KERNEL_BOOT_SOURCES
    Boot/Bootloader.S
)

set(KERNEL_CPU_SOURCES
    CPU/IDT.c
    CPU/Interrupts.S
    CPU/ISR.c
    CPU/MSR.c
    CPU/FPU.c
    CPU/ACPI.c
    CPU/NUMA.c
    CPU/UserMode.S
    CPU/PCI.c
    CPU/Syscall.c
    CPU/Syscall.S
    CPU/APIC.c
    CPU/SMP.c
    CPU/APEntry.c
    CPU/APLoader.S
    CPU/TSS.S
    CPU/GDT.c
)

set(KERNEL_DEBUG_SOURCES
    Debug/Logger.c
    Debug/Spinlock.c
    Debug/KDebug.c
)

set(KERNEL_DEVICES_SOURCES
    Device/COM.c
    Device/TTY.c
    Device/PIC.c
    Device/HPET.c
    Device/PIT.c
    Device/Keyboard.c
    Device/CMOS.c
    Device/RTC.c
)

set(KERNEL_MEMORY_SOURCES
    Memory/PMM.c
    Memory/KMem.c
    Memory/VMM.c
)

set(KERNEL_STORAGE_SOURCES
    Storage/AHCI.c
)

set(KERNEL_FILESYSTEM_SOURCES
    FileSystem/ext4.c
)

set(KERNEL_TASK_SOURCES
    Task/Task.c
    Task/RCU.c
    Task/Task.S
)

set(KERNEL_SOURCES
    Entry.c
    ${KERNEL_BOOT_SOURCES}
    ${KERNEL_CPU_SOURCES}
    ${KERNEL_DEBUG_SOURCES}
    ${KERNEL_DEVICES_SOURCES}
    ${KERNEL_MEMORY_SOURCES}
    ${KERNEL_STORAGE_SOURCES}
    ${KERNEL_FILESYSTEM_SOURCES}
    ${KERNEL_TASK_SOURCES}
)

set(LIBC_SOURCES
    ../Userland/Libraries/LibC/string.c
    ../Userland/Libraries/LibC/stdlib.c
    ../Userland/Libraries/LibC/stdio.c
)

set(SOURCES
    ${KERNEL_SOURCES}
    ${LIBC_SOURCES}
)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Boot/grub.cfg ${CMAKE_BINARY_DIR}/Kernel/Boot/grub.cfg COPYONLY)

add_link_options(LINKER:-T ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld -nostdlib -n)

add_compile_definitions(__THEOS_KERNEL)
add_compile_definitions(__USE_QEMU)
if(THEOS_ENABLE_KDEBUG)
    add_compile_definitions(THEOS_ENABLE_KDEBUG)
endif()

add_executable(Kernel ${SOURCES})
set_target_properties(Kernel PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld)

theos_install_headers(Kernel)
theos_install_sources(Kernel)
