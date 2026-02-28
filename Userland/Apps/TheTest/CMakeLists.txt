set(THETEST_SOURCES
    crt0.S
    main.c
)

add_executable(TheTest ${THETEST_SOURCES})
target_include_directories(TheTest
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
)
target_compile_options(TheTest
    PRIVATE
        -fno-pie
)
target_link_options(TheTest
    PRIVATE
        LINKER:-T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
        -nostdlib
        -static
        -n
        -no-pie
)
target_link_libraries(TheTest PRIVATE UserLibC)
