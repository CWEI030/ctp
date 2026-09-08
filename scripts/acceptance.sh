#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -lt 1 || "$#" -gt 2 \
    || ! "$1" =~ ^(replay|benchmark|simnow|all-offline)$ ]]; then
    echo "usage: scripts/acceptance.sh replay|benchmark [smoke|full]|simnow [preflight|online]|all-offline" >&2
    exit 2
fi

repository_root="$(git rev-parse --show-toplevel)"
cd "$repository_root"

temporary_directory="$(mktemp -d)"
trap 'rm -rf "$temporary_directory"' EXIT

prepare_offline_config() {
    offline_config="$temporary_directory/accounts.ini"
    cp config/accounts.example.ini "$offline_config"
    chmod 600 "$offline_config"
}

verify_evidence() {
    local result_directory="$1"
    local required
    for required in manifest.json latency_raw.csv queue_raw.csv cpu_raw.csv \
        events_summary.json report.md reproduce.sh SHA256SUMS; do
        test -s "$result_directory/$required"
    done
    (cd "$result_directory" && sha256sum -c SHA256SUMS)
}

run_one_benchmark() {
    local accounts="$1"
    local rate="$2"
    local warmup="$3"
    local duration="$4"
    local burst_rate="$5"
    local burst_seconds="$6"
    local result_directory="$7"
    local -a command=(
        ./build/ctp_client benchmark
        --config "$offline_config"
        --input tests/data/replay/minimal_signal_v1.csv
        --output "$result_directory"
        --accounts "$accounts"
        --rate "$rate"
        --warmup-seconds "$warmup"
        --duration-seconds "$duration")
    if [[ "$burst_seconds" -ne 0 ]]; then
        command+=(--burst-rate "$burst_rate" --burst-seconds "$burst_seconds")
    fi
    "${command[@]}"
    verify_evidence "$result_directory"
}

run_replay() {
    cmake --build build -j2 --target ctp_strategy_tests
    for repeat in 1 2 3; do
        ctest --test-dir build --output-on-failure -R '^strategy$'
    done
}

run_benchmark_matrix() {
    local profile="${1:-smoke}"
    if [[ ! "$profile" =~ ^(smoke|full)$ ]]; then
        echo "benchmark profile must be smoke or full" >&2
        return 2
    fi
    prepare_offline_config
    cmake --build build -j2 --target ctp_client
    local result_root="runtime/performance/${profile}-$(date +%Y%m%dT%H%M%S)"
    mkdir -p "$result_root"
    if [[ "$profile" == "smoke" ]]; then
        for accounts in 1 2 4; do
            run_one_benchmark "$accounts" 1000 0 1 2000 1 \
                "$result_root/accounts-$accounts"
        done
    else
        for repeat in 1 2 3; do
            for accounts in 1 2 4; do
                run_one_benchmark "$accounts" 1000 60 900 2000 60 \
                    "$result_root/steady-a${accounts}-r${repeat}"
            done
            for rate in 5000 10000 20000; do
                run_one_benchmark 4 "$rate" 60 900 0 0 \
                    "$result_root/saturation-${rate}-r${repeat}"
            done
        done
    fi
    echo "[ok] benchmark evidence root: $result_root"
}

run_simnow() {
    local mode="${1:-preflight}"
    local config_path="${CTP_SIMNOW_CONFIG:-config/accounts.local.ini}"
    if [[ ! "$mode" =~ ^(preflight|online)$ ]]; then
        echo "simnow mode must be preflight or online" >&2
        return 2
    fi
    if [[ ! -f "$config_path" ]]; then
        echo "[blocked] SimNow config not found: $config_path" >&2
        return 3
    fi

    cmake --build build -j2 --target ctp_client
    local check_output
    if ! check_output="$(
        ./build/ctp_client engine --mode live --config "$config_path" \
            --check --allow-orders
    )"; then
        echo "[blocked] SimNow configuration preflight failed" >&2
        return 3
    fi
    echo "$check_output"

    local accounts distinct_users
    accounts="$(sed -n 's/.*accounts=\([0-9][0-9]*\).*/\1/p' <<<"$check_output")"
    distinct_users="$(sed -n 's/.*distinct_users=\([0-9][0-9]*\).*/\1/p' <<<"$check_output")"
    if [[ -z "$accounts" || -z "$distinct_users" \
        || "$accounts" -lt 4 || "$distinct_users" -lt 4 ]]; then
        echo "[blocked] four-account acceptance requires at least four enabled accounts with four distinct user IDs" >&2
        return 3
    fi
    if [[ "$mode" == "preflight" ]]; then
        echo "[ok] SimNow four-account preflight passed; no network connection or order was attempted"
        return 0
    fi
    if [[ "${CTP_SIMNOW_CONFIRM:-}" != "I_UNDERSTAND_SIMNOW_ORDERS" ]]; then
        echo "[blocked] set CTP_SIMNOW_CONFIRM=I_UNDERSTAND_SIMNOW_ORDERS to authorize the online order run" >&2
        return 3
    fi

    ./build/ctp_client engine --mode live --config "$config_path" \
        --allow-orders
}

case "$1" in
    replay) run_replay ;;
    benchmark) run_benchmark_matrix "${2:-smoke}" ;;
    simnow) run_simnow "${2:-preflight}" ;;
    all-offline)
        cmake --build build -j2
        ctest --test-dir build --output-on-failure
        run_replay
        scripts/audit.sh all
        run_benchmark_matrix smoke
        ;;
esac
