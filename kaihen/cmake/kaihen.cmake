# kaiHEN (native Switch 2 homebrew): the Switch 2 devkit's toolchain (newlib, libounce), static, PIE as
# libnx requires. KAIHEN_COMPILER=clang (environment): Clang compiles (with the shipped PGO profiles) and
# the devkit's GCC links; otherwise the devkit's GCC does both. This port's flags are added in
# Platform/ next to this file, after the devkit's platform file sets its own.
if("$ENV{KAIHEN_COMPILER}" STREQUAL "clang")
  # Named before the devkit's toolchain file, whose find_program() keeps a compiler already set.
  # Through kaihen/tools: the devkit's flags carry GCC's -mtp=soft, which those drop.
  set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../tools/kaihen-clang" CACHE FILEPATH "")
  set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../tools/kaihen-clang++" CACHE FILEPATH "")
  set(CMAKE_ASM_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../tools/kaihen-clang" CACHE FILEPATH "")
  foreach(lang C CXX ASM)
    set(CMAKE_${lang}_COMPILER_TARGET aarch64-none-elf)
  endforeach()
endif()
include("$ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_LIST_DIR}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(THREADS_PREFER_PTHREAD_FLAG ON)
