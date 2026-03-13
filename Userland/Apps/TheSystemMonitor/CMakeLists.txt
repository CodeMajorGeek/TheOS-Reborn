set(THESYSTEMMONITOR_SOURCES
    crt0.c.S
    main.c
)

add_executable(TheSystemMonitor ${THESYSTEMMONITOR_SOURCES})
set_source_files_properties(crt0.c.S PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp")
target_include_directories(TheSystemMonitor
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
)
target_compile_options(TheSystemMonitor
    PRIVATE
        -fno-pie
)
target_link_options(TheSystemMonitor
    PRIVATE
        LINKER:-T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
        -nostdlib
        -static
        -n
        -no-pie
)
target_link_libraries(TheSystemMonitor PRIVATE UserLibC)
