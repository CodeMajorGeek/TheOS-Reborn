set(LIBC_SOURCES
    string.c
    stdlib.c
    stdio.c
    syscall.c
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DTHEOS_LIBC_BUILD")

set(SOURCES
    ${LIBC_SOURCES}
)
