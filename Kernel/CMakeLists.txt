set(KERNEL_ARCH x86_64)
option(THEOS_ENABLE_KDEBUG "Enable serial KDEBUG output and printf mirroring" ON)
option(THEOS_ENABLE_SCHED_TESTS "Enable SMP scheduler stress/balance/pathological tests" OFF)
set(THEOS_EARLY_PSF2_FONT "${CMAKE_SOURCE_DIR}/Base/system/fonts/ter-powerline-v14n.psf" CACHE FILEPATH "PSF2 font embedded in kernel for early framebuffer console")
message(STATUS "Kernel: THEOS_ENABLE_KDEBUG=${THEOS_ENABLE_KDEBUG}")
message(STATUS "Kernel: THEOS_ENABLE_SCHED_TESTS=${THEOS_ENABLE_SCHED_TESTS}")
message(STATUS "Kernel: THEOS_EARLY_PSF2_FONT=${THEOS_EARLY_PSF2_FONT}")

set(KERNEL_BOOT_SOURCES
    Boot/Bootloader.S
)

if(EXISTS "${THEOS_EARLY_PSF2_FONT}")
    set(KERNEL_EARLY_FONT_BIN ${CMAKE_CURRENT_BINARY_DIR}/builtin_console.psf)
    set(KERNEL_EARLY_FONT_OBJ ${CMAKE_CURRENT_BINARY_DIR}/builtin_console_psf.o)

    add_custom_command(
        OUTPUT ${KERNEL_EARLY_FONT_OBJ}
        COMMAND ${CMAKE_COMMAND} -E copy "${THEOS_EARLY_PSF2_FONT}" "${KERNEL_EARLY_FONT_BIN}"
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                ${CMAKE_LINKER} -r -b binary -o builtin_console_psf.o builtin_console.psf
        DEPENDS "${THEOS_EARLY_PSF2_FONT}"
        COMMENT "Embedding early PSF2 font into kernel"
        VERBATIM
    )

    set_source_files_properties(${KERNEL_EARLY_FONT_OBJ} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    list(APPEND KERNEL_BOOT_SOURCES ${KERNEL_EARLY_FONT_OBJ})
    add_compile_definitions(THEOS_EARLY_PSF2_EMBEDDED=1)
else()
    message(WARNING "Kernel: early PSF2 font file not found: ${THEOS_EARLY_PSF2_FONT}")
endif()

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
if(THEOS_ENABLE_SCHED_TESTS)
    add_compile_definitions(THEOS_ENABLE_SCHED_TESTS=1)
else()
    add_compile_definitions(THEOS_ENABLE_SCHED_TESTS=0)
endif()

add_executable(Kernel ${SOURCES})
target_compile_options(Kernel PRIVATE -mcmodel=kernel -fno-pic -fno-pie)
target_link_options(Kernel PRIVATE -no-pie)
set_target_properties(Kernel PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld)

theos_install_headers(Kernel)
theos_install_sources(Kernel)
