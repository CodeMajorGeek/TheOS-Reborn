if(EXISTS "${CMAKE_SOURCE_DIR}/Userland/Apps/TheEmbeddedDOOM/src")
	set(EMBEDDEDDOOM_ROOT ${CMAKE_SOURCE_DIR}/Userland/Apps/TheEmbeddedDOOM)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/Userland/Apps/embeddedDOOM/src")
	set(EMBEDDEDDOOM_ROOT ${CMAKE_SOURCE_DIR}/Userland/Apps/embeddedDOOM)
else()
	set(EMBEDDEDDOOM_ROOT ${CMAKE_SOURCE_DIR}/Userland/Apps/TheEmbeddedDOOM)
endif()
set(EMBEDDEDDOOM_SRC_DIR ${EMBEDDEDDOOM_ROOT}/src)

if(NOT EXISTS ${EMBEDDEDDOOM_SRC_DIR})
	message(FATAL_ERROR "embeddedDOOM tree missing (TheEmbeddedDOOM or legacy embeddedDOOM). Run: git submodule update --init Userland/Apps/TheEmbeddedDOOM")
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
	${EMBEDDEDDOOM_ROOT}/theos_port/i_video_theos.c
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

set_target_properties(embeddedDOOM PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Userland/Apps/TheEmbeddedDOOM"
)
