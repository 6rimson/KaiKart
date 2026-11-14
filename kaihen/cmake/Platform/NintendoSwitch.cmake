# The Switch 2 devkit's platform settings, then this port's (kaihen/cmake/kaihen.cmake).
# HBX_SWITCH2 keeps this port's Switch 2 code paths; MKW_KAIHEN marks where kaiHEN differs from the
# HBLauncher (Linux) build. _GNU_SOURCE: newlib hides POSIX declarations (fdopen, ...) under strict
# -std= modes otherwise. -fno-semantic-interposition: the devkit builds with -fPIC, under which GCC
# won't inline (even always_inline) functions it thinks the link could replace; nothing is.
include("$ENV{DEVKITPRO}/libounce/cmake/Platform/NintendoSwitch.cmake")
foreach(lang IN ITEMS C CXX ASM)
  string(APPEND CMAKE_${lang}_FLAGS_INIT " -ffunction-sections -fdata-sections -fno-semantic-interposition -D_GNU_SOURCE -DHBX_SWITCH2=1 -DMKW_KAIHEN=1")
endforeach()
# --wrap: kaihen/platform/kaihen_platform.c logs C++ throws and ends failed runs directly.
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -Wl,--gc-sections -Wl,--wrap=__cxa_throw -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=pthread_detach")
# No libdl: the dlopen stand-in is a header (kaihen/platform/include/dlfcn.h).
set(CMAKE_DL_LIBS "")
# OunceVK (Vulkan) for Dawn, then libnx again for OunceVK's own references.
foreach(lang IN ITEMS C CXX)
  set(CMAKE_${lang}_STANDARD_LIBRARIES "-lvulkan -lnx" CACHE STRING "" FORCE)
endforeach()
