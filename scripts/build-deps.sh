#!/bin/bash
# Build chiaki, FFmpeg, Opus, SDL2, OpenSSL from pinned sources as static XCFrameworks
# (ios-arm64 device + ios-arm64-simulator). Output: Frameworks/<name>.xcframework
# Usage: scripts/build-deps.sh [name ...]   (names: openssl opus sdl2 ffmpeg chiaki; default all)
# Needs: Xcode, cmake, protoc (brew install cmake protobuf), python3.
set -euo pipefail

MIN_IOS=16.0
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$ROOT/build/deps}      # sources + intermediates (gitignored)
OUT=${OUT:-$ROOT/Frameworks}
JOBS=$(sysctl -n hw.ncpu)
mkdir -p "$WORK" "$OUT"

# name url tag commit  (commit is verified after clone: tags are not trusted)
SRC_openssl="https://github.com/openssl/openssl openssl-3.5.9 45e844fa2a14ec92d146bd8f5778ac130b6625fb"
SRC_opus="https://github.com/xiph/opus v1.6.1 22244de5a79bd1d6d623c32e72bf1954b56235be"
SRC_sdl2="https://github.com/libsdl-org/SDL release-2.32.10 5d249570393f7a37e037abf22cd6012a4cc56a71"
SRC_ffmpeg="https://github.com/FFmpeg/FFmpeg n7.1.5 3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587"
# chiaki's submodules (nanopb, jerasure, gf-complete) are pinned by this commit.
SRC_chiaki="https://git.sr.ht/~thestr4ng3r/chiaki v2.2.0 89368f63c99d67cde8868c0269b66a1b0c507397"

SLICES="iphoneos iphonesimulator"

fetch() { # name -> $WORK/src/<name>
  local v=SRC_$1; set -- $1 ${!v}
  local dir=$WORK/src/$1
  if [ ! -d "$dir" ]; then
    git clone -q --depth 1 --branch "$3" --recurse-submodules --shallow-submodules "$2" "$dir"
  fi
  [ "$(git -C "$dir" rev-parse HEAD)" = "$4" ] || { echo "pin mismatch for $1" >&2; exit 1; }
}

triple() { # sdk -> clang target
  case $1 in iphoneos) echo "arm64-apple-ios$MIN_IOS";; *) echo "arm64-apple-ios$MIN_IOS-simulator";; esac
}
prefix() { echo "$WORK/install/$1/$2"; }   # name sdk

cmake_build() { # name sdk srcdir [extra cmake args...]
  local name=$1 sdk=$2 src=$3; shift 3
  cmake -S "$src" -B "$WORK/build/$name-$sdk" -G "Unix Makefiles" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$(prefix $name $sdk)" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
  cmake --build "$WORK/build/$name-$sdk" -j"$JOBS" --target install >/dev/null
}

# xcframework name headers_subdir_of_prefix lib... (lib paths relative to prefix; merged if >1)
pack() {
  local name=$1 hdr=$2; shift 2
  local args=() sdk libs l
  rm -rf "$OUT/$name.xcframework"
  for sdk in $SLICES; do
    libs=(); for l in "$@"; do libs+=("$(prefix $name $sdk)/$l"); done
    mkdir -p "$WORK/pack/$name-$sdk"
    libtool -static -o "$WORK/pack/$name-$sdk/lib$name.a" "${libs[@]}" 2>/dev/null
    args+=(-library "$WORK/pack/$name-$sdk/lib$name.a" -headers "$(prefix $name $sdk)/$hdr")
  done
  xcodebuild -create-xcframework "${args[@]}" -output "$OUT/$name.xcframework" >/dev/null
  echo "built $OUT/$name.xcframework"
}

build_openssl() {
  fetch openssl
  for sdk in $SLICES; do
    local b=$WORK/build/openssl-$sdk; rm -rf "$b"; mkdir -p "$b"; cd "$b"
    local sdkpath=$(xcrun --sdk $sdk --show-sdk-path) t=$(triple $sdk)
    # generic target + explicit flags: keeps arm64 simulator working
    export CROSS_TOP=$(dirname "$(dirname "$sdkpath")") CROSS_SDK=$(basename "$sdkpath")  # read again by make
    CC="$(xcrun --sdk $sdk -f clang) -target $t" \
      "$WORK/src/openssl/Configure" iphoneos-cross no-shared no-tests no-apps no-dso no-engine no-asm \
      --prefix="$(prefix openssl $sdk)" >/dev/null
    make -j"$JOBS" build_libs >/dev/null && make install_dev >/dev/null
  done
  pack openssl include lib/libssl.a lib/libcrypto.a
}

build_opus() {
  fetch opus
  for sdk in $SLICES; do cmake_build opus $sdk "$WORK/src/opus" -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF; done
  # chiaki includes <opus/opus.h>, so ship the include/ dir (contains opus/)
  pack opus include lib/libopus.a
}

build_sdl2() {
  fetch sdl2
  for sdk in $SLICES; do cmake_build sdl2 $sdk "$WORK/src/sdl2" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF; done
  # app uses <SDL.h>, so ship include/SDL2 contents
  pack sdl2 include/SDL2 lib/libSDL2.a
}

build_ffmpeg() {
  fetch ffmpeg
  for sdk in $SLICES; do
    local b=$WORK/build/ffmpeg-$sdk; rm -rf "$b"; mkdir -p "$b"; cd "$b"
    local sdkpath=$(xcrun --sdk $sdk --show-sdk-path) t=$(triple $sdk)
    # Minimal: chiaki only needs h264/hevc decode (VideoToolbox) via libavcodec+libavutil.
    "$WORK/src/ffmpeg/configure" --prefix="$(prefix ffmpeg $sdk)" \
      --enable-cross-compile --arch=arm64 --target-os=darwin \
      --cc="$(xcrun --sdk $sdk -f clang)" --sysroot="$sdkpath" \
      --extra-cflags="-target $t -isysroot $sdkpath" --extra-ldflags="-target $t -isysroot $sdkpath" \
      --disable-everything --disable-programs --disable-doc --disable-debug --disable-shared --enable-static \
      --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
      --disable-network --disable-iconv --disable-securetransport --disable-audiotoolbox \
      --enable-pic --enable-videotoolbox \
      --enable-decoder=h264,hevc --enable-parser=h264,hevc \
      --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox >/dev/null
    make -j"$JOBS" >/dev/null && make install >/dev/null
  done
  pack ffmpeg include lib/libavcodec.a lib/libavutil.a
}

build_chiaki() {
  fetch chiaki
  # avcodec_close() was removed in FFmpeg 7 (avcodec_free_context already closes)
  sed -i '' '/avcodec_close(/d' "$WORK/src/chiaki/lib/src/ffmpegdecoder.c"
  # needs OpenSSL, Opus, FFmpeg for the same slice; build them first
  for d in openssl opus ffmpeg; do [ -d "$(prefix $d iphoneos)" ] || { echo "build $d first" >&2; exit 1; }; done
  local venv=$WORK/venv
  [ -d "$venv" ] || { python3 -m venv "$venv"; "$venv/bin/pip" -q install "protobuf<4" "setuptools<81"; }
  for sdk in $SLICES; do
    local ssl=$(prefix openssl $sdk) opus=$(prefix opus $sdk) ff=$(prefix ffmpeg $sdk)
    cmake_build chiaki $sdk "$WORK/src/chiaki" \
      -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_SETSU=OFF \
      -DCHIAKI_ENABLE_FFMPEG_DECODER=ON -DCHIAKI_ENABLE_PI_DECODER=OFF -DCHIAKI_LIB_ENABLE_OPUS=ON \
      -DCMAKE_FIND_ROOT_PATH="$ssl;$opus;$ff" -DOPENSSL_ROOT_DIR="$ssl" -DOPENSSL_USE_STATIC_LIBS=ON \
      -DPYTHON_EXECUTABLE="$venv/bin/python" -DPython_EXECUTABLE="$venv/bin/python" \
      -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
      -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
    # chiaki has no install rules: collect libs + headers by hand
    local p=$(prefix chiaki $sdk) b=$WORK/build/chiaki-$sdk
    rm -rf "$p"; mkdir -p "$p/lib" "$p/include/chiaki"
    cp "$WORK/src/chiaki/lib/include/chiaki/"*.h "$b/lib/include/chiaki/"*.h "$p/include/chiaki/"
    find "$b" -name '*.a' -exec cp {} "$p/lib/" \;
  done
  # merges libchiaki + nanopb + jerasure + gf_complete; OpenSSL/Opus/FFmpeg stay separate xcframeworks
  pack chiaki include lib/libchiaki.a lib/libprotobuf-nanopb.a lib/libjerasure.a lib/libgf_complete.a
}

TARGETS=${*:-openssl opus sdl2 ffmpeg chiaki}
for t in $TARGETS; do "build_$t"; done
