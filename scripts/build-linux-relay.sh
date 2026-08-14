#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"${root}/build/linux-x64"}

mkdir -p "${output}"
gcc -std=c11 -O2 -Wall -Wextra -Werror \
  "${root}/server/welnpt_relay_linux.c" \
  -o "${output}/welnpt-relay" \
  -lcrypto -lrt
"${output}/welnpt-relay" --self-test
echo "Built ${output}/welnpt-relay"
