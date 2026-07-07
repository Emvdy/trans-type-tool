#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Werror \
  -fobjc-arc \
  trans_type_mac.mm \
  -framework Foundation \
  -framework AppKit \
  -framework ApplicationServices \
  -framework Carbon \
  -o trans_type_mac

echo "Built macOS executable:"
echo "  trans_type_mac"
