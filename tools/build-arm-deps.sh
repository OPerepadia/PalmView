#!/usr/bin/env bash
# Builds what the native ARM module links against, into deps/:
#   ffmpeg-build-arm   FFmpeg's h264 + aac_fixed decoders for the Palm (ARMv5T)
#   ffmpeg-build-host  the same decoders for host reference checks
#   musl-build-arm     musl's libm, position-independent
#   venv               Unicorn CPU emulator for make test-arm (optional)
#
# Needs arm-none-eabi-gcc with newlib (Arch: arm-none-eabi-gcc,
# arm-none-eabi-newlib; Debian/Ubuntu: gcc-arm-none-eabi, libnewlib-arm-none-eabi).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
deps="$root/deps"
ffmpeg_version=9.0.2
ffmpeg_sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
musl_version=1.2.5
musl_sha256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
jobs="$(nproc 2>/dev/null || echo 4)"

# Keep in sync with ARM_CFLAGS in the Makefile.
arm_flags="-marm -march=armv5t -mtune=xscale -mfloat-abi=soft -fno-short-enums \
-fPIC -msingle-pic-base -mno-pic-data-is-text-relative -mlong-calls \
-ffunction-sections -fdata-sections -fno-common -fno-merge-constants"

fetch() {
  local url="$1" file="$2" sha="$3"
  mkdir -p "$(dirname "$file")"
  [[ -f "$file" ]] || curl -fsSL --retry 3 -o "$file" "$url"
  echo "$sha  $file" | sha256sum -c --quiet
}

common_ffmpeg_options=(
  --disable-everything --disable-programs --disable-doc --disable-avdevice
  --disable-avformat --disable-swscale --disable-swresample --disable-avfilter
  --disable-network --disable-pthreads --disable-autodetect
  --enable-hardcoded-tables --disable-asm --disable-debug
  --enable-decoder=h264 --enable-decoder=aac_fixed --enable-static --disable-shared
)

fetch "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" \
  "$deps/ffmpeg-src/ffmpeg-$ffmpeg_version.tar.xz" "$ffmpeg_sha256"
if [[ ! -d "$deps/ffmpeg" ]]; then
  tar -xJf "$deps/ffmpeg-src/ffmpeg-$ffmpeg_version.tar.xz" -C "$deps"
  mv "$deps/ffmpeg-$ffmpeg_version" "$deps/ffmpeg"
fi
python3 -I "$root/tools/patch-ffmpeg.py" "$deps/ffmpeg"

mkdir -p "$deps/ffmpeg-build-arm"
(cd "$deps/ffmpeg-build-arm" && ../ffmpeg/configure \
  --enable-cross-compile --cross-prefix=arm-none-eabi- --arch=arm --cpu=armv5t \
  --target-os=none --disable-runtime-cpudetect --disable-w32threads \
  --disable-os2threads "${common_ffmpeg_options[@]}" \
  --extra-cflags="$arm_flags -Wno-incompatible-pointer-types" \
  --extra-ldflags="--specs=nosys.specs -Wl,--no-enum-size-warning" >configure.log &&
  make -j"$jobs" libavcodec/libavcodec.a libavutil/libavutil.a >build.log)

mkdir -p "$deps/ffmpeg-build-host"
(cd "$deps/ffmpeg-build-host" && ../ffmpeg/configure "${common_ffmpeg_options[@]}" \
  >configure.log && make -j"$jobs" libavcodec/libavcodec.a libavutil/libavutil.a >build.log)

fetch "https://musl.libc.org/releases/musl-$musl_version.tar.gz" \
  "$deps/musl-src/musl-$musl_version.tar.gz" "$musl_sha256"
if [[ ! -d "$deps/musl" ]]; then
  tar -xzf "$deps/musl-src/musl-$musl_version.tar.gz" -C "$deps"
  mv "$deps/musl-$musl_version" "$deps/musl"
fi
musl="$deps/musl" out="$deps/musl-build-arm"
rm -rf "$out" && mkdir -p "$out/include/bits" "$out/obj"
sed -f "$musl/tools/mkalltypes.sed" "$musl/arch/arm/bits/alltypes.h.in" \
  "$musl/include/alltypes.h.in" >"$out/include/bits/alltypes.h"
for source in "$musl"/src/math/*.c; do
  # shellcheck disable=SC2086
  arm-none-eabi-gcc $arm_flags -O2 -std=c99 -ffreestanding -nostdinc \
    -D_XOPEN_SOURCE=700 -fno-builtin -I"$musl/arch/arm" -I"$musl/arch/generic" \
    -I"$out/include" -I"$musl/src/include" -I"$musl/src/internal" -I"$musl/include" \
    -w -c "$source" -o "$out/obj/$(basename "$source" .c).o"
done
arm-none-eabi-ar rcs "$out/libm.a" "$out"/obj/*.o

if [[ ! -x "$deps/venv/bin/python" ]] && python3 -m venv "$deps/venv"; then
  "$deps/venv/bin/pip" install -q unicorn || echo "Unicorn not installed: make test-arm unavailable"
fi
echo "ARM dependencies ready. Build with: make bench"
