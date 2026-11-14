# kaiHEN (native Switch 2 homebrew): the Switch 2 devkit's toolchain (devkitA64 GCC, newlib,
# libounce), static, PIE as libnx requires. This port's flags are added in Platform/ next to this
# file, after the devkit's platform file sets its own.
include("$ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_LIST_DIR}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(THREADS_PREFER_PTHREAD_FLAG ON)
