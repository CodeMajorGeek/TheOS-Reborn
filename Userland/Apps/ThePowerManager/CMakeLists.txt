set(THEPOWERMANAGER_SOURCES
    crt0.S
    main.c
)

add_executable(ThePowerManager ${THEPOWERMANAGER_SOURCES})
target_include_directories(ThePowerManager
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/EasyArgs/includes
)
target_compile_options(ThePowerManager
    PRIVATE
        -fno-pie
)
target_link_options(ThePowerManager
    PRIVATE
        LINKER:-T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
        -nostdlib
        -static
        -n
        -no-pie
)
target_link_libraries(ThePowerManager PRIVATE UserLibC)
