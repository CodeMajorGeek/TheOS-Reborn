set(LIBC_SOURCES
    dlfcn.c
    dirent.c
    errno.c
    malloc.c
    pthread.c
    signal.c
    sys/ipc.c
    sys/ioctl.c
    sys/termios.c
    sys/time.c
    string.c
    stdlib.c
    stdlib_parse.c
    stdio.c
    syscall.c
    tls.c
    unistd.c
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DTHEOS_LIBC_BUILD")

add_library(UserLibC STATIC
    ${LIBC_SOURCES}
)
target_include_directories(UserLibC
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Includes
)
target_compile_options(UserLibC
    PRIVATE
        -fno-pie
)

add_library(UserLibCShared SHARED
    ${LIBC_SOURCES}
)
set_target_properties(UserLibCShared
    PROPERTIES
        OUTPUT_NAME c
)
target_include_directories(UserLibCShared
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Includes
)
target_compile_options(UserLibCShared
    PRIVATE
        -fPIC
)
target_link_options(UserLibCShared
    PRIVATE
        -nostdlib
        -Wl,-soname,libc.so
        -Wl,--hash-style=sysv
)

set(SOURCES
    ${LIBC_SOURCES}
)
