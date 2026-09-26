#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/dual-tank-tests"
mkdir -p "$cache"
archive="$cache/cjson-1.7.18.tar.gz"
if [[ ! -f "$archive" ]]; then
  curl -fsSL --max-time 60 \
    https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz -o "$archive"
fi
printf '%s  %s\n' 3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5 "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$cache"
python3 "$root/tests/extract_firmware.py" "$root/dualTankController.ino" > "$cache/firmware.inc"
cc -std=c99 -Wall -Wextra -Werror -c "$cache/cJSON-1.7.18/cJSON.c" -o "$cache/cjson.o"
cxxflags=(-std=c++17 -Wall -Wextra -Werror -I"$cache/cJSON-1.7.18" -I"$cache" -I"$root/tests" -I"$root/tests/transport_stubs")
for name in protocol firmware transport; do
  c++ "${cxxflags[@]}" "$root/tests/test_${name}.cpp" "$cache/cjson.o" -o "$cache/test_${name}"
  "$cache/test_${name}"
done
