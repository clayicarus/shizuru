#!/bin/bash
# copy_bridge.sh — copies libshizuru_bridge.dylib into the macOS app bundle Frameworks/
# Called from Xcode build phase or manually after cmake build.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# CMake build output (adjust if your build dir is different)
BUILD_DIR="${WORKSPACE_ROOT}/build"
DYLIB_SRC="${BUILD_DIR}/ui/bridge/libshizuru_bridge.dylib"

# Flutter macOS app bundle Frameworks directory.
# During flutter build, BUILT_PRODUCTS_DIR and FRAMEWORKS_FOLDER_PATH are set by Xcode.
if [ -n "${BUILT_PRODUCTS_DIR}" ] && [ -n "${FRAMEWORKS_FOLDER_PATH}" ]; then
  DEST="${BUILT_PRODUCTS_DIR}/${FRAMEWORKS_FOLDER_PATH}"
else
  # Fallback: copy next to the script for manual testing
  DEST="${SCRIPT_DIR}/Runner/Frameworks"
fi

mkdir -p "${DEST}"

if [ -f "${DYLIB_SRC}" ]; then
  cp "${DYLIB_SRC}" "${DEST}/libshizuru_bridge.dylib"
  echo "Copied libshizuru_bridge.dylib to ${DEST}"

  # Also copy to the Flutter build output if it exists.
  FLUTTER_APP="${WORKSPACE_ROOT}/ui/build/macos/Build/Products/Debug/ui.app/Contents/Frameworks"
  if [ -d "${FLUTTER_APP}" ]; then
    cp "${DYLIB_SRC}" "${FLUTTER_APP}/libshizuru_bridge.dylib"
    echo "Copied libshizuru_bridge.dylib to ${FLUTTER_APP}"
  fi
else
  echo "Warning: ${DYLIB_SRC} not found. Build the C++ bridge first with cmake."
  echo "  cd ${WORKSPACE_ROOT} && cmake -B build && cmake --build build --target shizuru_bridge"
fi
