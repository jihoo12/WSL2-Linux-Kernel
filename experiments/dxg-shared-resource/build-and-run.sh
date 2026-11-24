#!/bin/sh
# SPDX-License-Identifier: MIT
# Build with a caller-supplied DirectX-Headers checkout; never install packages.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf 'usage: %s path/to/DirectX-Headers [--texture|--test-texture|--test-ipc]\n' "$0" >&2
    exit 64
fi
if ! headers=$(CDPATH='' cd -- "$1" && pwd); then exit 64; fi
shift
if [ ! -f "$headers/include/directx/d3d12.h" ] ||
   [ ! -f "$headers/src/dxguids.cpp" ]; then
    printf 'DirectX-Headers source checkout required: %s\n' "$headers" >&2
    exit 64
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -O2 \
    -isystem "$headers/include" -isystem "$headers/include/wsl/stubs" \
    "$here/dxg-shared-resource.cpp" "$headers/src/dxguids.cpp" -ldl -pthread \
    -o "$tmp/dxg-shared-resource"
wsl_libs=/usr/lib/wsl/lib
case ":${LD_LIBRARY_PATH:-}:" in
    *":$wsl_libs:"*) ;;
    *) export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$wsl_libs" ;;
esac
"$tmp/dxg-shared-resource" "$@"
