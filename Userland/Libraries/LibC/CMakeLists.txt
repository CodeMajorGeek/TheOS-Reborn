set(LIBC_SOURCES
    dirent.c
    errno.c
    malloc.c
    pthread.c
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
    dirent.c
    errno.c
    malloc.c
    pthread.c
    string.c
    stdlib_parse.c
    stdio.c
    syscall.c
    tls.c
    unistd.c
)
target_include_directories(UserLibC
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Includes
)
target_compile_options(UserLibC
    PRIVATE
        -fno-pie
)

set(SOURCES
    ${LIBC_SOURCES}
)
