set(EMBEDDEDDOOM_ROOT ${CMAKE_SOURCE_DIR}/Userland/Apps/embeddedDOOM)
set(EMBEDDEDDOOM_SRC_DIR ${EMBEDDEDDOOM_ROOT}/src)

if(NOT EXISTS ${EMBEDDEDDOOM_SRC_DIR})
    message(FATAL_ERROR "embeddedDOOM submodule is missing. Run: git submodule update --init --recursive Userland/Apps/embeddedDOOM")
endif()

file(GLOB EMBEDDEDDOOM_SOURCES CONFIGURE_DEPENDS
    ${EMBEDDEDDOOM_SRC_DIR}/*.c
)

list(REMOVE_ITEM EMBEDDEDDOOM_SOURCES
    ${EMBEDDEDDOOM_SRC_DIR}/i_video.c
    ${EMBEDDEDDOOM_SRC_DIR}/i_video_console.c
    ${EMBEDDEDDOOM_SRC_DIR}/XDriver.c
    ${EMBEDDEDDOOM_SRC_DIR}/DrawFunctions.c
    ${EMBEDDEDDOOM_SRC_DIR}/os_generic.c
)

list(APPEND EMBEDDEDDOOM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/i_video_theos.c
    ${EMBEDDEDDOOM_SRC_DIR}/support/rawwad.c
)

theos_add_user_app(embeddedDOOM
    SOURCES
        ${EMBEDDEDDOOM_SOURCES}
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
        ${EMBEDDEDDOOM_SRC_DIR}
        ${EMBEDDEDDOOM_SRC_DIR}/support
    COMPILE_DEFINITIONS
        NORMALUNIX
        LINUX
        THEOS_RUNTIME
        MAXPLAYERS=1
        RANGECHECK
        DISABLE_NETWORK
        GENERATE_BAKED
        SET_MEMORY_DEBUG=0
    COMPILE_OPTIONS
        -w
)
