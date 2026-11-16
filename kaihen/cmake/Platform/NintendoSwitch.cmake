# The Switch 2 devkit's platform settings, then this port's (kaihen/cmake/kaihen.cmake).
# HBX_SWITCH2 turns on the runtime's Switch 2 code paths; MKW_KAIHEN marks the kaiHEN ones among them.
# _GNU_SOURCE: newlib hides POSIX declarations (fdopen, ...) under strict
# -std= modes otherwise. -fno-semantic-interposition: the devkit builds with -fPIC, under which GCC
# won't inline (even always_inline) functions it thinks the link could replace; nothing is.
include("$ENV{DEVKITPRO}/libounce/cmake/Platform/NintendoSwitch.cmake")
foreach(lang IN ITEMS C CXX ASM)
  string(APPEND CMAKE_${lang}_FLAGS_INIT " -ffunction-sections -fdata-sections -fno-semantic-interposition -D_GNU_SOURCE -DHBX_SWITCH2=1 -DMKW_KAIHEN=1")
endforeach()
# --wrap: kaihen/platform/kaihen_platform.c logs C++ throws, ends failed runs directly, and gives
# every pthread the thread pointer Clang's code reads.
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -Wl,--gc-sections -Wl,--wrap=__cxa_throw -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_create")
# No libdl: the dlopen stand-in is a header (kaihen/platform/include/dlfcn.h).
set(CMAKE_DL_LIBS "")
# OunceVK (Vulkan) for Dawn, then libnx again for OunceVK's own references.
foreach(lang IN ITEMS C CXX)
  set(CMAKE_${lang}_STANDARD_LIBRARIES "-lvulkan -lnx" CACHE STRING "" FORCE)
endforeach()

if("$ENV{KAIHEN_COMPILER}" STREQUAL "clang")
  # Clang reads the thread pointer from TPIDR_EL0 (GCC's -mtp=soft asks libnx instead; the shim
  # copies libnx's value into TPIDR_EL0 for every thread; kaihen/tools drops -mtp=soft). It finds newlib through the sysroot and
  # GCC's libstdc++ headers explicitly. The devkit's GCC links: its specs, newlib and libgcc.
  set(KAIHEN_GCC "$ENV{DEVKITPRO}/devkitA64")
  set(KAIHEN_CXX_HEADERS "${KAIHEN_GCC}/aarch64-none-elf/include/c++/15.2.0")
  # Unwind tables everywhere, as GCC makes them: for a bare-metal target Clang leaves them out of C
  # code, and a C++ exception can't then pass a C frame (the shim's throw logger, SDL callbacks).
  foreach(lang IN ITEMS C CXX ASM)
    string(APPEND CMAKE_${lang}_FLAGS_INIT " --sysroot=${KAIHEN_GCC}/aarch64-none-elf -funwind-tables -fasynchronous-unwind-tables")
  endforeach()
  string(APPEND CMAKE_CXX_FLAGS_INIT " -nostdinc++ -isystem ${KAIHEN_CXX_HEADERS} -isystem ${KAIHEN_CXX_HEADERS}/aarch64-none-elf")
  foreach(lang IN ITEMS C CXX)
    set(CMAKE_${lang}_LINK_EXECUTABLE
        "${KAIHEN_GCC}/bin/aarch64-none-elf-g++ <CMAKE_${lang}_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
  endforeach()
endif()
