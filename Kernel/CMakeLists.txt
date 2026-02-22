set(KERNEL_ARCH x86_64)
option(THEOS_ENABLE_KDEBUG "Enable serial KDEBUG output and printf mirroring" ON)
option(KERNEL_DEBUG_LOG_SERIAL "Enable kernel debug log sink to serial COM1" ON)
option(KERNEL_DEBUG_LOG_FILE "Enable kernel debug log sink to ext4 file (RAM buffered until FS ready)" ON)
option(THEOS_ENABLE_SCHED_TESTS "Enable SMP scheduler stress/balance/pathological tests" OFF)
option(THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL "Force x2APIC on SMP systems (experimental)." OFF)



message(STATUS "Kernel: THEOS_ENABLE_KDEBUG=${THEOS_ENABLE_KDEBUG}")
message(STATUS "Kernel: KERNEL_DEBUG_LOG_SERIAL=${KERNEL_DEBUG_LOG_SERIAL}")
message(STATUS "Kernel: KERNEL_DEBUG_LOG_FILE=${KERNEL_DEBUG_LOG_FILE}")
message(STATUS "Kernel: THEOS_ENABLE_SCHED_TESTS=${THEOS_ENABLE_SCHED_TESTS}")
message(STATUS "Kernel: THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL=${THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL}")

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
    CPU/UserMode.c
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
    Device/TTYVGA.c
    Device/TTYFramebuffer.c
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
if(KERNEL_DEBUG_LOG_SERIAL)
    add_compile_definitions(THEOS_KDEBUG_LOG_SERIAL=1)
else()
    add_compile_definitions(THEOS_KDEBUG_LOG_SERIAL=0)
endif()
if(KERNEL_DEBUG_LOG_FILE)
    add_compile_definitions(THEOS_KDEBUG_LOG_FILE=1)
else()
    add_compile_definitions(THEOS_KDEBUG_LOG_FILE=0)
endif()
if(THEOS_ENABLE_SCHED_TESTS)
    add_compile_definitions(THEOS_ENABLE_SCHED_TESTS=1)
else()
    add_compile_definitions(THEOS_ENABLE_SCHED_TESTS=0)
endif()
if(THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL)
    add_compile_definitions(THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL=1)
else()
    add_compile_definitions(THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL=0)
endif()

add_executable(Kernel ${SOURCES})
target_compile_options(Kernel PRIVATE -mcmodel=kernel -fno-pic -fno-pie)
target_link_options(Kernel PRIVATE -no-pie)
set_target_properties(Kernel PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld)

theos_install_headers(Kernel)
theos_install_sources(Kernel)
