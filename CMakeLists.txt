cmake_minimum_required(VERSION 3.16)
project(TheOS C ASM)

include(${CMAKE_SOURCE_DIR}/Meta/CMake/utils.cmake)

set(CMAKE_INSTALL_MESSAGE NEVER)

enable_testing()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(THEOS_ARCH "x86_64" CACHE STRING "Target architecture for TheOS.")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

unset(CMAKE_SYSROOT)
set(CMAKE_STAGING_PREFIX ${CMAKE_BINARY_DIR}/Root)
set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/Root)
set(CMAKE_INSTALL_DATAROOTDIR ${CMAKE_BINARY_DIR}/Root/Res)

set(TOOLCHAIN_PATH ${CMAKE_SOURCE_DIR}/Toolchain/Local/${THEOS_ARCH}/bin)
set(TOOLCHAIN_PREFIX ${TOOLCHAIN_PATH}/${THEOS_ARCH}-pc-theos-)

set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_LINKER ${TOOLCHAIN_PREFIX}ld)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_AR ${TOOLCHAIN_PREFIX}ar)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msoft-float -O -fno-stack-protector -ffreestanding -nostdlib -nostdinc -static")

add_custom_target(iso
	COMMAND chmod +x ${CMAKE_SOURCE_DIR}/Meta/iso.sh
	COMMAND ${CMAKE_SOURCE_DIR}/Meta/iso.sh
	USES_TERMINAL
)

add_custom_target(run
	DEPENDS iso
	COMMAND chmod +x ${CMAKE_SOURCE_DIR}/Meta/run.sh
	COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh
	USES_TERMINAL
)

add_custom_target(install-run
	COMMAND ${CMAKE_MAKE_PROGRAM} install
	COMMAND ${CMAKE_MAKE_PROGRAM} iso
	COMMAND ${CMAKE_MAKE_PROGRAM} run
	USES_TERMINAL
)

add_custom_target(create-disk
	COMMAND chmod +x ${CMAKE_SOURCE_DIR}/Meta/disk.sh
	COMMAND ${CMAKE_SOURCE_DIR}/Meta/disk.sh
	USES_TERMINAL
)

include_directories(.)
include_directories(${CMAKE_BINARY_DIR})

add_link_options(--sysroot ${CMAKE_BINARY_DIR}/Root)

include_directories(Includes)
include_directories(Userland/Libraries/LibC/Includes)

add_subdirectory(Userland)
add_subdirectory(Kernel)
