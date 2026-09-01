#!/usr/bin/env bash

set -euo pipefail

if [[ "${1:-}" != "secrets" || "$#" -ne 1 ]]; then
    echo "usage: scripts/audit.sh secrets" >&2
    exit 2
fi

repository_root="$(git rev-parse --show-toplevel)"
cd "$repository_root"

failed=0
mapfile -d '' committable_files \
    < <(git ls-files -z --cached --others --exclude-standard)

while IFS= read -r tracked_path; do
    case "$tracked_path" in
        config/accounts.local.ini|config/*.local.ini|.env|.env.*)
            if [[ "$tracked_path" != ".env.example" ]]; then
                echo "[fail] local secret file is tracked: $tracked_path" >&2
                failed=1
            fi
            ;;
    esac
done < <(git ls-files)

private_key_pattern='BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY'
for candidate in "${committable_files[@]}"; do
    if grep -Iq . "$candidate" \
        && grep -Eq -- "$private_key_pattern" "$candidate"; then
        echo "[fail] a committable file contains a private-key header: $candidate" >&2
        failed=1
    fi
done

for tracked_config in "${committable_files[@]}"; do
    if [[ "$tracked_config" != config/*.ini ]]; then
        continue
    fi
    while IFS='=' read -r key value; do
        key="${key//[[:space:]]/}"
        value="${value#${value%%[![:space:]]*}}"
        value="${value%${value##*[![:space:]]}}"
        case "$key" in
            user_id|password|app_id|auth_code)
                if [[ -n "$value" && ! "$value" =~ ^\<[^\>]+\>$ ]]; then
                    echo "[fail] tracked config has a non-placeholder $key: $tracked_config" >&2
                    failed=1
                fi
                ;;
        esac
    done < "$tracked_config"
done

while IFS= read -r local_config; do
    while IFS='=' read -r key value; do
        key="${key//[[:space:]]/}"
        value="${value#${value%%[![:space:]]*}}"
        value="${value%${value##*[![:space:]]}}"
        case "$key" in
            user_id|password|app_id|auth_code)
                if [[ ${#value} -ge 4 ]]; then
                    for candidate in "${committable_files[@]}"; do
                        if grep -Iq . "$candidate" \
                            && grep -IFq -- "$value" "$candidate"; then
                            echo "[fail] value from local field $key also appears in a committable file" >&2
                            failed=1
                            break
                        fi
                    done
                fi
                ;;
        esac
    done < "$local_config"
done < <(find config -maxdepth 1 -type f -name '*.local.ini' -print)

if [[ "$failed" -ne 0 ]]; then
    exit 1
fi

echo "[ok] committable files contain no detected local CTP secrets"
