#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

deployment_target="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
read -r -a requested_arches <<< "${TRANS_TYPE_MAC_ARCHS:-arm64 x86_64}"
arch_flags=()
for arch_name in "${requested_arches[@]}"; do
  arch_flags+=("-arch" "$arch_name")
done

clang++ \
  -std=c++17 \
  -Os \
  -Wall \
  -Wextra \
  -Werror \
  -fobjc-arc \
  "${arch_flags[@]}" \
  -mmacosx-version-min="$deployment_target" \
  trans_type_mac.mm \
  -framework Foundation \
  -framework AppKit \
  -framework ApplicationServices \
  -framework Carbon \
  -Wl,-dead_strip \
  -o trans_type_mac

if [[ -n "${CODESIGN_IDENTITY:-}" ]]; then
  codesign --force --options runtime --timestamp --sign "$CODESIGN_IDENTITY" trans_type_mac
fi

echo "Built macOS executable:"
echo "  trans_type_mac"
echo "Architectures: ${requested_arches[*]}"
echo "Minimum macOS: $deployment_target"
