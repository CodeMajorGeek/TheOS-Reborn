include_guard(GLOBAL)

include(CMakeParseArguments)

set(THEOS_USERLAND_DEFAULT_CRT0
    ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/crt/crt0.c.S)
set(THEOS_USERLAND_DEFAULT_LINKER_SCRIPT
    ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/crt/linker.dynamic.ld)

function(theos_add_user_app target_name)
    set(options)
    set(one_value_args CRT0 LINKER_SCRIPT)
    set(multi_value_args
        SOURCES
        INCLUDES
        COMPILE_DEFINITIONS
        COMPILE_OPTIONS
        LINK_OPTIONS
        LIBRARIES
    )
    cmake_parse_arguments(THEOS_APP
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(NOT THEOS_APP_SOURCES)
        message(FATAL_ERROR "theos_add_user_app(${target_name}) requires SOURCES.")
    endif()

    set(app_crt0 ${THEOS_APP_CRT0})
    if(NOT app_crt0)
        set(app_crt0 ${THEOS_USERLAND_DEFAULT_CRT0})
    endif()

    set(app_linker_script ${THEOS_APP_LINKER_SCRIPT})
    if(NOT app_linker_script)
        set(app_linker_script ${THEOS_USERLAND_DEFAULT_LINKER_SCRIPT})
    endif()

    add_executable(${target_name}
        ${THEOS_APP_SOURCES}
        ${app_crt0}
    )

    set_source_files_properties(${app_crt0}
        PROPERTIES
            COMPILE_FLAGS "-x assembler-with-cpp"
    )

    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/Userland/Libraries/LibC/Includes
            ${THEOS_APP_INCLUDES}
    )

    target_compile_definitions(${target_name}
        PRIVATE
            ${THEOS_APP_COMPILE_DEFINITIONS}
    )

    target_compile_options(${target_name}
        PRIVATE
            -fno-pie
            ${THEOS_APP_COMPILE_OPTIONS}
    )

    target_link_options(${target_name}
        PRIVATE
            LINKER:-T ${app_linker_script}
            -nostdlib
            LINKER:-Bdynamic
            -n
            -no-pie
            LINKER:-rpath,/lib
            LINKER:--hash-style=sysv
            ${THEOS_APP_LINK_OPTIONS}
    )

    target_link_libraries(${target_name}
        PRIVATE
            UserLibCShared
            ${THEOS_APP_LIBRARIES}
    )
endfunction()
