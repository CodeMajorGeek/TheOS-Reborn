set(EMBEDDEDDOOM_ROOT ${CMAKE_SOURCE_DIR}/Userland/Apps/embeddedDOOM)
set(EMBEDDEDDOOM_SRC_DIR ${EMBEDDEDDOOM_ROOT}/src)
set(EMBEDDEDDOOM_CRT0 ${CMAKE_SOURCE_DIR}/Userland/Apps/TheApp/crt0.c.S)
set(EMBEDDEDDOOM_LINKER ${CMAKE_SOURCE_DIR}/Userland/Apps/TheApp/linker.ld)

if(NOT EXISTS ${EMBEDDEDDOOM_SRC_DIR})
    message(FATAL_ERROR "embeddedDOOM submodule is missing. Run: git submodule update --init --recursive Userland/Apps/embeddedDOOM")
endif()

file(GLOB EMBEDDEDDOOM_SOURCES CONFIGURE_DEPENDS
    ${EMBEDDEDDOOM_SRC_DIR}/*.c
)

list(REMOVE_ITEM EMBEDDEDDOOM_SOURCES
    ${EMBEDDEDDOOM_SRC_DIR}/i_video.c
    ${EMBEDDEDDOOM_SRC_DIR}/XDriver.c
    ${EMBEDDEDDOOM_SRC_DIR}/DrawFunctions.c
    ${EMBEDDEDDOOM_SRC_DIR}/os_generic.c
)

list(APPEND EMBEDDEDDOOM_SOURCES
    ${EMBEDDEDDOOM_CRT0}
    ${EMBEDDEDDOOM_SRC_DIR}/support/rawwad.c
)

add_executable(embeddedDOOM ${EMBEDDEDDOOM_SOURCES})
set_source_files_properties(${EMBEDDEDDOOM_CRT0} PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp")

target_include_directories(embeddedDOOM
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
        ${EMBEDDEDDOOM_SRC_DIR}
        ${EMBEDDEDDOOM_SRC_DIR}/support
)

target_compile_definitions(embeddedDOOM
    PRIVATE
        NORMALUNIX
        LINUX
        MAXPLAYERS=1
        RANGECHECK
        DISABLE_NETWORK
        GENERATE_BAKED
        SET_MEMORY_DEBUG=0
)

target_compile_options(embeddedDOOM
    PRIVATE
        -fno-pie
        -w
)

target_link_options(embeddedDOOM
    PRIVATE
        LINKER:-T ${EMBEDDEDDOOM_LINKER}
        -nostdlib
        -static
        -n
        -no-pie
)

target_link_libraries(embeddedDOOM PRIVATE UserLibC)
