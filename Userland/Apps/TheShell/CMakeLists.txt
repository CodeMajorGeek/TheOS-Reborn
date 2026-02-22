set(THESHELL_SOURCES
    crt0.S
    main.c
)

add_executable(TheShell ${THESHELL_SOURCES})
target_include_directories(TheShell
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
)
target_compile_options(TheShell
    PRIVATE
        -fno-pie
)
target_link_options(TheShell
    PRIVATE
        LINKER:-T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
        -nostdlib
        -static
        -n
        -no-pie
)
target_link_libraries(TheShell PRIVATE UserLibC)
