#!/usr/bin/env bash
set -euo pipefail
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(git -C "$script_directory" rev-parse --show-toplevel)"
output_directory="${1:-$repository_root/runtime/performance/reproduced-accounts-1}"
temporary_directory="$(mktemp -d)"
trap 'rm -rf "$temporary_directory"' EXIT
cp "$repository_root/config/accounts.example.ini" "$temporary_directory/accounts.ini"
chmod 600 "$temporary_directory/accounts.ini"
"$repository_root/build/ctp_client" benchmark \
  --config "$temporary_directory/accounts.ini" \
  --input "$script_directory/replay_input.csv" \
  --output "$output_directory" \
  --accounts 1 --rate 1000 --warmup-seconds 2 \
  --duration-seconds 36 --submit-stride 1
