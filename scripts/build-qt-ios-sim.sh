#!/bin/bash
# Qt's official iOS package has no arm64 simulator slice (arm64 = device, x86_64 = simulator).
# Build qtbase + qtsvg 6.8.3 for the arm64 simulator into ~/Qt/6.8.3/ios-sim.
# Needs ~/Qt/6.8.3/macos (host Qt) and sources: aqt install-src mac 6.8.3 --archives qtbase qtsvg -O <dir>
set -euo pipefail
SRC=${1:?usage: $0 <dir containing 6.8.3/Src>}/6.8.3/Src
PREFIX=$HOME/Qt/6.8.3/ios-sim
# Qt 6.8.3 + Xcode 27 clang: __yield is not declared; use inline asm instead
sed -i '' 's|    __yield();              // Generic|    __asm__ __volatile__("yield");|' "$SRC/qtbase/src/corelib/thread/qyieldcpu.h"
B=$HOME/Qt/build-sim; mkdir -p "$B/qtbase" "$B/qtsvg"
ARGS="-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphonesimulator"
cd "$B/qtbase"
"$SRC/qtbase/configure" -prefix "$PREFIX" -release -static -platform macx-ios-clang -sysroot iphonesimulator \
  -qt-host-path "$HOME/Qt/6.8.3/macos" -nomake examples -nomake tests -- $ARGS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
cmake --build . --parallel && cmake --install .
cd "$B/qtsvg"
"$PREFIX/bin/qt-configure-module" "$SRC/qtsvg" -- $ARGS
cmake --build . --parallel && cmake --install .
