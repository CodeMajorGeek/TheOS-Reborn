set(LIBC_SOURCES
    string.c
    stdlib.c
    stdio.c
    syscall.c
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DTHEOS_LIBC_BUILD")

add_library(UserLibC STATIC
    string.c
    stdio.c
    syscall.c
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
