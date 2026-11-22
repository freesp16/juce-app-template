#!/bin/sh
# Builds the app for the unit (32-bit ARM "armel", glibc 2.13) inside the toolchain container and checks the result.
#   scripts/build-unit.sh          → out/<name>, ready to copy to the USB stick
#   scripts/build-unit.sh clean    start from an empty build folder
# The first run builds the toolchain image (toolchain/Dockerfile: GCC 11 + a Debian wheezy sysroot), about 30 minutes.
set -eu
cd "$(dirname "$0")/.."
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
name=$(sed -n 's/^set(APP_NAME "\(.*\)")$/\1/p' CMakeLists.txt)
[ -f third_party/JUCE/CMakeLists.txt ] || { echo "JUCE is missing: git submodule update --init --depth 1" >&2; exit 1; }
docker image inspect "$image" >/dev/null 2>&1 || docker build -t "$image" toolchain
[ "${1:-}" = clean ] && rm -rf build/unit
mkdir -p out
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work "$image" sh -c "
  set -e
  [ -f build/unit/CMakeCache.txt ] || cmake --preset unit
  cmake --build --preset unit -j\$(nproc)
  arm-linux-gnueabi-strip -o out/$name build/unit/bin/$name
  sp16-check-glibc out/$name                       # nothing newer than the unit's glibc 2.13
  if arm-linux-gnueabi-readelf -d out/$name | grep -E 'NEEDED.*(EGL|GLES|GAL)'; then exit 1; fi # bound at run time
  arm-linux-gnueabi-readelf -n out/$name | grep -q APPINFO                    # name, version and icon for the menu
"
ls -l "out/$name"
