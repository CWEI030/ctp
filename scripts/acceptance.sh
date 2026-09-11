#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -lt 1 || "$#" -gt 2 \
    || ! "$1" =~ ^(replay|benchmark|simnow|hot-path|all-offline)$ ]]; then
    echo "usage: scripts/acceptance.sh replay|benchmark [smoke|full|evidence-test]|simnow [preflight|online]|hot-path|all-offline" >&2
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

json_number() {
    local file="$1"
    local key="$2"
    sed -n "s/^[[:space:]]*\"${key}\": \([-0-9.]*\),\{0,1\}$/\1/p" "$file"
}

publish_one_benchmark() {
    local source_directory="$1"
    local published_directory="$2"
    verify_evidence "$source_directory"
    mkdir -p "$published_directory"

    # 正式 CSV 可达数 GB；逐纳秒计数保持全部信息，同时避免把重复文本膨胀提交到 Git。
    {
        printf 'account_index\tstage\tlatency_ns\tcount\n'
        awk -F, '
            NR == 1 {
                if ($0 != "sequence,mono_ns,account_index,stage,latency_ns") exit 10
                next
            }
            NF != 5 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ ||
                $3 !~ /^[0-9]+$/ || $4 !~ /^[a-z_]+$/ || $5 !~ /^[0-9]+$/ {
                exit 11
            }
            { print $3 "\t" $4 "\t" $5 }
            END { if (NR < 2) exit 12 }
        ' "$source_directory/latency_raw.csv" \
            | LC_ALL=C sort -T "$temporary_directory" -t $'\t' -k1,1n -k2,2 -k3,3n \
            | awk -F '\t' '
                NR == 1 { previous = $0; count = 1; next }
                $0 == previous { count++; next }
                { print previous "\t" count; previous = $0; count = 1 }
                END { if (NR > 0) print previous "\t" count }
            '
    } > "$published_directory/latency_raw.histogram.tsv"

    local raw_sha raw_bytes raw_lines raw_samples histogram_samples
    raw_sha="$(sha256sum "$source_directory/latency_raw.csv" | awk '{print $1}')"
    raw_bytes="$(wc -c < "$source_directory/latency_raw.csv")"
    raw_lines="$(wc -l < "$source_directory/latency_raw.csv")"
    raw_samples="$((raw_lines - 1))"
    histogram_samples="$(awk 'NR > 1 { total += $4 } END { print total + 0 }' \
        "$published_directory/latency_raw.histogram.tsv")"
    if [[ "$histogram_samples" -ne "$raw_samples" ]]; then
        echo "published histogram sample count differs from raw CSV" >&2
        return 1
    fi
    {
        printf 'file\tsha256\tbytes\tlines\tsamples\n'
        printf 'latency_raw.csv\t%s\t%s\t%s\t%s\n' \
            "$raw_sha" "$raw_bytes" "$raw_lines" "$raw_samples"
    } > "$published_directory/latency_raw.index.tsv"

    cp "$source_directory/manifest.json" "$source_directory/queue_raw.csv" \
        "$source_directory/cpu_raw.csv" "$source_directory/events_summary.json" \
        "$source_directory/report.md" "$source_directory/reproduce.sh" \
        "$published_directory/"
    (
        cd "$published_directory"
        sha256sum manifest.json latency_raw.histogram.tsv latency_raw.index.tsv \
            queue_raw.csv cpu_raw.csv events_summary.json report.md reproduce.sh \
            > SHA256SUMS
    )
}

verify_published_against_raw() {
    local published_directory="$1"
    local raw_file="$2"
    (cd "$published_directory" && sha256sum -c SHA256SUMS)
    local expected_sha expected_bytes expected_lines actual_sha actual_bytes actual_lines
    read -r expected_sha expected_bytes expected_lines < <(
        awk -F '\t' 'NR == 2 { print $2, $3, $4 }' \
            "$published_directory/latency_raw.index.tsv"
    )
    actual_sha="$(sha256sum "$raw_file" | awk '{print $1}')"
    actual_bytes="$(wc -c < "$raw_file")"
    actual_lines="$(wc -l < "$raw_file")"
    [[ "$expected_sha" == "$actual_sha" \
        && "$expected_bytes" == "$actual_bytes" \
        && "$expected_lines" == "$actual_lines" ]]
}

capture_environment() {
    local destination="$1"
    local phase="$2"
    {
        printf 'phase=%s\n' "$phase"
        printf 'captured_at='; date --iso-8601=seconds
        printf 'git_commit='; git rev-parse HEAD
        printf 'benchmark_cpuset=%s\n' "${CTP_BENCHMARK_CPUSET:-0-15}"
        printf 'interactive_users='; who | wc -l
        printf 'loadavg='; cat /proc/loadavg
        printf 'kernel='; uname -srvmo
        printf 'clocksource='; cat /sys/devices/system/clocksource/clocksource0/current_clocksource
        printf '\n[lscpu]\n'; lscpu
        printf '\n[memory]\n'; free -b
        printf '\n[uptime]\n'; uptime
        printf '\n[top_processes]\n'
        ps -eLo pid,tid,psr,pcpu,comm --sort=-pcpu | head -n 31 || true
        printf '\n[frequency_policy]\n'
        grep -H . /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver \
            /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true
        if command -v numactl >/dev/null 2>&1; then
            printf '\n[numa]\n'; numactl --hardware
        fi
    } > "$destination"
}

publish_benchmark_root() {
    local result_root="$1"
    local published_root="evidence/performance/$(basename "$result_root")"
    if [[ -e "$published_root" ]]; then
        echo "published evidence already exists: $published_root" >&2
        return 1
    fi
    mkdir -p "$published_root"
    local source_directory run_name manifest raw_index
    {
        printf 'run\taccounts\trate\twarmup_seconds\tduration_seconds\tburst_rate\tburst_seconds\tdropped\traw_samples\traw_sha256\n'
        for source_directory in "$result_root"/*; do
            [[ -d "$source_directory" && -s "$source_directory/manifest.json" ]] || continue
            run_name="$(basename "$source_directory")"
            publish_one_benchmark "$source_directory" "$published_root/$run_name"
            raw_index="$published_root/$run_name/latency_raw.index.tsv"
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$run_name" \
                "$(json_number "$source_directory/manifest.json" accounts)" \
                "$(json_number "$source_directory/manifest.json" rate_per_second)" \
                "$(json_number "$source_directory/manifest.json" warmup_seconds)" \
                "$(json_number "$source_directory/manifest.json" duration_seconds)" \
                "$(json_number "$source_directory/manifest.json" burst_rate_per_second)" \
                "$(json_number "$source_directory/manifest.json" burst_seconds)" \
                "$(json_number "$source_directory/manifest.json" performance_samples_dropped)" \
                "$(awk -F '\t' 'NR == 2 { print $5 }' "$raw_index")" \
                "$(awk -F '\t' 'NR == 2 { print $2 }' "$raw_index")"
        done
    } > "$published_root/matrix_index.tsv"
    cp "$result_root"/environment_*.txt "$published_root/"
    {
        printf '# 正式离线性能矩阵\n\n'
        printf '本目录由完整原始 CSV 无损发布；逐运行原始 CSV 留在 `%s`。\n\n' "$result_root"
        printf '逐纳秒直方图保留 `(account, stage, latency_ns)` 的精确计数，原始文件摘要见各运行的 `latency_raw.index.tsv`。\n\n'
        printf '这是共享主机上的离线回放和柜台替身结果，不代表真实 CTP 网络延迟或生产 SLA；四真实账户 SimNow 验收仍未完成。\n'
    } > "$published_root/README.md"
    (
        cd "$published_root"
        sha256sum matrix_index.tsv environment_*.txt README.md > SHA256SUMS
    )
    echo "[ok] published performance evidence: $published_root"
}

run_evidence_test() {
    local source_directory="$temporary_directory/source"
    local published_directory="$temporary_directory/published"
    mkdir -p "$source_directory"
    printf '%s\n' \
        'sequence,mono_ns,account_index,stage,latency_ns' \
        '1,100,0,market_to_signal,7' \
        '2,101,0,market_to_signal,7' \
        '3,102,1,callback_to_state,11' \
        > "$source_directory/latency_raw.csv"
    for file in manifest.json queue_raw.csv cpu_raw.csv events_summary.json report.md reproduce.sh; do
        printf 'fixture\n' > "$source_directory/$file"
    done
    (
        cd "$source_directory"
        sha256sum manifest.json latency_raw.csv queue_raw.csv cpu_raw.csv \
            events_summary.json report.md reproduce.sh > SHA256SUMS
    )
    publish_one_benchmark "$source_directory" "$published_directory"
    local expected="$temporary_directory/expected.tsv"
    printf '%s\n' \
        $'account_index\tstage\tlatency_ns\tcount' \
        $'0\tmarket_to_signal\t7\t2' \
        $'1\tcallback_to_state\t11\t1' > "$expected"
    diff -u "$expected" "$published_directory/latency_raw.histogram.tsv"
    verify_published_against_raw "$published_directory" "$source_directory/latency_raw.csv"
    printf '4,103,1,callback_to_state,13\n' >> "$source_directory/latency_raw.csv"
    if verify_published_against_raw "$published_directory" "$source_directory/latency_raw.csv"; then
        echo "tampered raw CSV unexpectedly passed verification" >&2
        return 1
    fi
    echo "[ok] lossless evidence publication and tamper detection"
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
    if [[ -n "${CTP_BENCHMARK_CPUSET:-}" ]]; then
        taskset -c "$CTP_BENCHMARK_CPUSET" "${command[@]}"
    else
        "${command[@]}"
    fi
    verify_evidence "$result_directory"
}

run_replay() {
    cmake --build build -j2 --target ctp_strategy_tests
    for repeat in 1 2 3; do
        ctest --test-dir build --output-on-failure -R '^strategy$'
    done
}

run_hot_path() {
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j2 --target ctp_engine_tests
    # 固定四账户异步负载重复运行，证明生产调度链在 Release 下无分配且不串账户。
    ctest --test-dir build --output-on-failure -R '^engine$' \
        --repeat until-fail:20
    scripts/audit.sh hot-path
}

run_benchmark_matrix() {
    local profile="${1:-smoke}"
    if [[ "$profile" == "evidence-test" ]]; then
        run_evidence_test
        return
    fi
    if [[ ! "$profile" =~ ^(smoke|full)$ ]]; then
        echo "benchmark profile must be smoke, full, or evidence-test" >&2
        return 2
    fi
    prepare_offline_config
    cmake --build build -j2 --target ctp_client
    local result_root="runtime/performance/${profile}-$(date +%Y%m%dT%H%M%S)"
    mkdir -p "$result_root"
    if [[ "$profile" == "full" ]]; then
        export CTP_BENCHMARK_CPUSET="${CTP_BENCHMARK_CPUSET:-0-15}"
        capture_environment "$result_root/environment_before.txt" before
    fi
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
        capture_environment "$result_root/environment_after.txt" after
        publish_benchmark_root "$result_root"
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
            --check --allow-orders --acceptance
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
        --allow-orders --acceptance
}

case "$1" in
    replay) run_replay ;;
    benchmark) run_benchmark_matrix "${2:-smoke}" ;;
    simnow) run_simnow "${2:-preflight}" ;;
    hot-path) run_hot_path ;;
    all-offline)
        run_hot_path
        cmake --build build -j2
        ctest --test-dir build --output-on-failure
        run_replay
        scripts/audit.sh all
        run_benchmark_matrix smoke
        ;;
esac
