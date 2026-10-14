#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ -x portable/Linux-x86_64/run.sh ]]; then exec portable/Linux-x86_64/run.sh "$@"; fi
if [[ -x build/KoraLo ]]; then exec build/KoraLo "$@"; fi
missing=()
for tool in cmake c++ ctest; do command -v "$tool" >/dev/null 2>&1 || missing+=("$tool"); done
[[ -f /usr/include/X11/Xlib.h ]] || missing+=("X11 development headers")
if ((${#missing[@]})); then
  printf 'Source build requires preinstalled tools: %s\nUse the ready portable package.\n' "${missing[*]}" >&2
  exit 2
fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKORALO_BUILD_TESTS=ON
cmake --build build --parallel "${KORALO_BUILD_JOBS:-2}"
ctest --test-dir build --output-on-failure
exec ./build/KoraLo "$@"
