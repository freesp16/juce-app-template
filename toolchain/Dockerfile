# Cross toolchain for the device application platform (SP-16 hardware):
#   ARMv7-A Cortex-A9, armel (soft-float ABI, loader /lib/ld-linux.so.3), glibc 2.13.
#
# Stage 1 builds a sysroot from Debian wheezy (eglibc 2.13, armel) with the -dev packages a JUCE app needs.
# Stage 2 builds binutils + GCC 11 against that sysroot, so libstdc++/libgcc never reference symbols newer than 2.13.
# Link your app dynamically (the emulator's hardware shim is an LD_PRELOAD library) with
# -static-libstdc++ -static-libgcc (the firmware's own libstdc++ is GCC 4.6's).

ARG DEBIAN=debian:bookworm-slim

FROM ${DEBIAN} AS sysroot
RUN apt-get update && apt-get install -y --no-install-recommends debootstrap ca-certificates && rm -rf /var/lib/apt/lists/*
# Wheezy is archived; its Release signatures have expired, hence --no-check-gpg. --foreign only unpacks, never runs ARM code.
RUN debootstrap --arch=armel --foreign --variant=minbase --no-check-gpg \
      --include=libc6-dev,linux-libc-dev,libasound2-dev,libx11-dev,libxext-dev,libxrandr-dev,libxrender-dev,libxinerama-dev,libxcursor-dev,libfreetype6-dev,libfontconfig1-dev,zlib1g-dev \
      wheezy /sysroot http://archive.debian.org/debian \
 && for deb in /sysroot/var/cache/apt/archives/*.deb; do dpkg-deb -x "$deb" /sysroot; done \
 && rm -rf /sysroot/var/cache/apt/archives/*.deb /sysroot/debootstrap
# Absolute symlinks (e.g. libm.so -> /lib/arm-linux-gnueabi/libm.so.6) must point inside the sysroot.
RUN cd /sysroot && find . -type l | while read -r link; do \
      target=$(readlink "$link"); case "$target" in /*) \
        ln -sfn "$(realpath -m --relative-to="/sysroot/$(dirname "$link")" "/sysroot$target")" "$link";; esac; done

FROM ${DEBIAN} AS gcc
ARG BINUTILS=2.40
ARG GCC=11.4.0
RUN apt-get update && apt-get install -y --no-install-recommends build-essential wget ca-certificates xz-utils bison flex \
      texinfo libgmp-dev libmpfr-dev libmpc-dev file && rm -rf /var/lib/apt/lists/*
COPY --from=sysroot /sysroot /opt/sp16/sysroot
WORKDIR /src
RUN wget -q https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS}.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-${GCC}/gcc-${GCC}.tar.xz \
 && tar xf binutils-${BINUTILS}.tar.xz && tar xf gcc-${GCC}.tar.xz && rm *.tar.xz
ENV TARGET=arm-linux-gnueabi PREFIX=/opt/sp16/toolchain SYSROOT=/opt/sp16/sysroot
RUN mkdir build-binutils && cd build-binutils \
 && ../binutils-${BINUTILS}/configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT --disable-nls \
      --disable-werror --disable-multilib --enable-multiarch \
 && make -j"$(nproc)" && make install && cd .. && rm -rf build-binutils
ENV PATH=/opt/sp16/toolchain/bin:$PATH
RUN mkdir build-gcc && cd build-gcc \
 && ../gcc-${GCC}/configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT \
      --enable-languages=c,c++ --disable-multilib --enable-multiarch \
      --with-arch=armv7-a --with-tune=cortex-a9 --with-fpu=neon --with-float=softfp --with-mode=thumb \
      --with-glibc-version=2.13 --enable-threads=posix --enable-__cxa_atexit --enable-shared \
      --disable-libsanitizer --disable-libgomp --disable-libquadmath --disable-libssp --disable-libitm \
      --disable-libatomic --disable-libvtv --disable-nls --disable-bootstrap \
 && make -j"$(nproc)" && make install-strip && cd .. && rm -rf build-gcc

FROM ${DEBIAN}
# g++ and the *native* -dev packages are for JUCE's helper tool (juceaide), which JUCE builds for the build machine
# (it compiles the JUCE GUI modules) when cross-compiling. The target's own libraries come from /opt/sp16/sysroot.
RUN apt-get update && apt-get install -y --no-install-recommends make cmake ninja-build pkg-config file python3 \
      g++ git ca-certificates libgmp10 libmpfr6 libmpc3 \
      libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxrender-dev libxcomposite-dev \
      libfreetype-dev libfontconfig1-dev && rm -rf /var/lib/apt/lists/*
COPY --from=gcc /opt/sp16 /opt/sp16
COPY cross.cmake /opt/sp16/cross.cmake
COPY check-glibc.sh /usr/local/bin/sp16-check-glibc
COPY sp16-pkg-config /usr/local/bin/sp16-pkg-config
RUN chmod +x /usr/local/bin/sp16-check-glibc /usr/local/bin/sp16-pkg-config

# JUCE 8 needs FreeType >= 2.10 (FT_Outline_EmboldenXY, FT_Set_Default_Properties); the wheezy sysroot has 2.4.9.
# Build a current FreeType statically into the sysroot, replacing the old headers, .pc and dev symlink. The old
# libfreetype.so.6 stays: libfontconfig links against it, and it is what the device ships at run time.
ARG FREETYPE=2.13.3
RUN apt-get update && apt-get install -y --no-install-recommends wget xz-utils && rm -rf /var/lib/apt/lists/* \
 && export PATH=/opt/sp16/toolchain/bin:$PATH \
 && cd /tmp && wget -q https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE}.tar.xz \
 && tar xf freetype-${FREETYPE}.tar.xz && cd freetype-${FREETYPE} \
 && ./configure --build="$(./builds/unix/config.guess)" --host=arm-linux-gnueabi \
      --prefix=/usr --libdir=/usr/lib/arm-linux-gnueabi \
      --enable-static --disable-shared --with-zlib=no --with-bzip2=no --with-png=no --with-harfbuzz=no --with-brotli=no \
      CC=arm-linux-gnueabi-gcc CFLAGS="-O2 -fPIC -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp" \
 && make -j"$(nproc)" \
 && rm -rf /opt/sp16/sysroot/usr/include/freetype2 /opt/sp16/sysroot/usr/include/ft2build.h \
           /opt/sp16/sysroot/usr/lib/arm-linux-gnueabi/libfreetype.so /opt/sp16/sysroot/usr/lib/arm-linux-gnueabi/libfreetype.la \
           /opt/sp16/sysroot/usr/lib/arm-linux-gnueabi/libfreetype.a \
 && make install DESTDIR=/opt/sp16/sysroot \
 && cd / && rm -rf /tmp/freetype-*
# No global PKG_CONFIG_* here on purpose: they would also apply to JUCE's native juceaide build (see sp16-pkg-config).
ENV PATH_ORIG=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    PATH=/opt/sp16/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
WORKDIR /work

# JUCE 9's OpenGL ES backend on Linux includes <EGL/egl.h> and <EGL/eglext.h> (EGL 1.5 declarations). Copy the Khronos
# headers (arch-independent) into the sysroot. Headers only: the app never links libEGL/libGLESv2, it resolves the
# Vivante driver flavour at run time (hw/platform/common/display/gldispatch), and sp16-pkg-config cannot see these
# native packages, so JUCE's optional pkg-config lookup for egl/gl/glesv2 finds nothing to link.
# JUCE 9's juce_gui_basics also includes XInput2.h and Xcomposite.h, which the wheezy sysroot lacks; JUCE loads libXi
# and libXcomposite with dlopen at run time (X11Symbols), so only their declarations are needed (newer XI2.h replaces
# wheezy's so XInput2.h compiles).
RUN apt-get update && apt-get install -y --no-install-recommends libegl-dev libgles-dev libxi-dev libxcomposite-dev \
 && cp -r /usr/include/EGL /usr/include/GLES2 /usr/include/KHR /opt/sp16/sysroot/usr/include/ \
 && cp /usr/include/X11/extensions/XInput2.h /usr/include/X11/extensions/XI2.h /usr/include/X11/extensions/Xcomposite.h \
       /usr/include/X11/extensions/composite.h /opt/sp16/sysroot/usr/include/X11/extensions/ \
 && rm -rf /var/lib/apt/lists/*

# qemu-arm (user mode) runs unit tests built for the device on any development machine (NEON ↔ scalar parity, not
# timing). A build tool only; never shipped.
RUN apt-get update && apt-get install -y --no-install-recommends qemu-user && rm -rf /var/lib/apt/lists/*
