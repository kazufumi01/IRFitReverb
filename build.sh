#!/bin/bash
# Universal build (AU + VST3 + Standalone) of IR Fit Reverb, installs AU / VST3, then validates the AU.
set -e
cd "$(dirname "$0")"
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=$(xcrun -f clang) -DCMAKE_CXX_COMPILER=$(xcrun -f clang++) \
  -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release -j --target IRFitReverb_All
killall -9 AudioComponentRegistrar 2>/dev/null || true
auval -v aufx IrFt Jpkl | tail -3
