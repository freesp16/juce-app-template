# CMake toolchain file for the SP-16 application platform (ARMv7-A Cortex-A9, armel/softfp, glibc 2.13).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER   arm-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabi-g++)
set(CMAKE_SYSROOT /opt/sp16/sysroot)
set(CMAKE_FIND_ROOT_PATH /opt/sp16/sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(SP16_ARCH_FLAGS "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp")
set(CMAKE_C_FLAGS_INIT   "${SP16_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${SP16_ARCH_FLAGS}")
# Dynamic link against the firmware's own glibc (the emulator's hardware shim is an LD_PRELOAD library),
# but carry our own C++ runtime: the firmware's libstdc++ is from GCC 4.6.
# -rpath-link: the sysroot is Debian multiarch (lib/arm-linux-gnueabi); ld does not search it for a library's own
# dependencies (libX11 -> libxcb -> libXau, libdl) unless told to.
# --as-needed: link only what is referenced, so libs the firmware lacks (Xinerama, Xcursor) never become DT_NEEDED.
set(SP16_RPATH_LINK "-Wl,-rpath-link=/opt/sp16/sysroot/lib/arm-linux-gnueabi:/opt/sp16/sysroot/usr/lib/arm-linux-gnueabi")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -Wl,--as-needed ${SP16_RPATH_LINK}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -Wl,--as-needed ${SP16_RPATH_LINK}")

# pkg-config for the target sysroot (a wrapper, so the native juceaide build keeps the plain native pkg-config).
set(PKG_CONFIG_EXECUTABLE /usr/local/bin/sp16-pkg-config CACHE FILEPATH "pkg-config for the SP-16 sysroot")
