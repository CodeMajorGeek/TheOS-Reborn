set(KERNEL_ARCH x86_64)

set(KERNEL_SOURCES 
    Boot/boot.S
    entry.c
    Device/COM.c
    Device/TTY.c
    CPU/IO.c
    CPU/IDT.c
    CPU/interrupts.S
    CPU/ISR.c
    Device/PIC.c
    Device/PIT.c
    Device/APIC.c
    Debug/logger.c
    CPU/MSR.c
    CPU/MADT.c
    CPU/ACPI.c
    Memory/Memory.c
    CPU/user_mode.S
    Device/Keyboard.c
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

add_executable(Kernel ${SOURCES})
set_target_properties(Kernel PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld)

theos_install_headers(Kernel)
theos_install_sources(Kernel)
