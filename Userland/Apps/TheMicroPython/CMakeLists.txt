set(MICROPY_ROOT ${CMAKE_SOURCE_DIR}/Userland/Apps/MicroPython)
set(MICROPY_PORT_DIR ${MICROPY_ROOT}/ports/theos)
set(MICROPY_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/micropython-build)
set(MICROPY_PORT_BIN ${MICROPY_BUILD_DIR}/TheMicroPython)
set(MICROPY_APP_BIN ${CMAKE_CURRENT_BINARY_DIR}/TheMicroPython)
set(MICROPY_USERLIBC ${CMAKE_BINARY_DIR}/Userland/Libraries/LibC/libUserLibC.a)

add_custom_target(TheMicroPythonApp ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MICROPY_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E env
        CROSS_COMPILE=${TOOLCHAIN_PREFIX}
        BUILD=${MICROPY_BUILD_DIR}
        THEOS_LIBC_INC=${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
        THEOS_USERLIBC=${MICROPY_USERLIBC}
        make -C ${MICROPY_PORT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${MICROPY_PORT_BIN} ${MICROPY_APP_BIN}
    BYPRODUCTS ${MICROPY_APP_BIN}
    DEPENDS
        UserLibC
        ${MICROPY_PORT_DIR}/Makefile
        ${MICROPY_PORT_DIR}/mpconfigport.h
        ${MICROPY_PORT_DIR}/mphalport.h
        ${MICROPY_PORT_DIR}/qstrdefsport.h
        ${MICROPY_PORT_DIR}/crt0.S
        ${MICROPY_PORT_DIR}/linker.ld
        ${MICROPY_PORT_DIR}/main.c
        ${MICROPY_PORT_DIR}/theos_mphal.c
    USES_TERMINAL
    VERBATIM
)
