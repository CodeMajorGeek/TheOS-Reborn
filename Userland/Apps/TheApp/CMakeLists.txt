set(THEAPP_SOURCES
    crt0.S
    main.c
)

add_executable(TheApp ${THEAPP_SOURCES})
target_include_directories(TheApp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
)
target_compile_options(TheApp
    PRIVATE
        -fno-pie
)
target_link_options(TheApp
    PRIVATE
        LINKER:-T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
        -nostdlib
        -static
        -n
        -no-pie
)
target_link_libraries(TheApp PRIVATE UserLibC)
