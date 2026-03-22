add_library(TheTestDynShared SHARED
    thetestdyn.c
)

set_target_properties(TheTestDynShared
    PROPERTIES
        OUTPUT_NAME thetestdyn
)

target_compile_options(TheTestDynShared
    PRIVATE
        -fPIC
)

target_link_options(TheTestDynShared
    PRIVATE
        -nostdlib
        -Wl,-soname,libthetestdyn.so
        -Wl,--hash-style=sysv
)
