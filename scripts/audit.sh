#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -ne 1 || ! "${1:-}" =~ ^(secrets|hot-path|all)$ ]]; then
    echo "usage: scripts/audit.sh secrets|hot-path|all" >&2
    exit 2
fi

repository_root="$(git rev-parse --show-toplevel)"
cd "$repository_root"

audit_secrets() {
    local failed=0
    local tracked_path candidate tracked_config local_config key value
    local private_key_pattern='BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY'
    local -a committable_files
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
                            # 已发布延迟原始数据只含经发布器校验的整数列和固定阶段名；
                            # 数百万个时钟/延迟整数可能偶然包含纯数字用户代码。
                            if [[ "$candidate" == evidence/performance/evidence-*/accounts-*/latency_raw.csv ]]; then
                                continue
                            fi
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
        return 1
    fi
    echo "[ok] committable files contain no detected local CTP secrets"
}

extract_function() {
    local source_file="$1"
    local marker="$2"
    local matches
    matches="$(grep -Fc -- "$marker" "$source_file")"
    if [[ "$matches" -ne 1 ]]; then
        echo "[fail] hot-path marker must match exactly once: $source_file: $marker (matches=$matches)" >&2
        return 1
    fi
    awk -v marker="$marker" '
        index($0, marker) != 0 { active = 1; found = 1 }
        active {
            print
            line = $0
            opens = gsub(/\{/, "{", line)
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            if (opens > 0) seen_body = 1
            if (seen_body && depth == 0) exit
        }
        END { if (!found) exit 1 }
    ' "$source_file"
}

audit_hot_path() {
    local temporary_file
    temporary_file="$(mktemp)"
    trap 'rm -f "$temporary_file"' RETURN
    while IFS='|' read -r source_file marker; do
        extract_function "$source_file" "$marker" >> "$temporary_file"
    done <<'HOT_PATHS'
include/ctp/engine.hpp|bool try_push(const Event& event) noexcept
include/ctp/engine.hpp|bool try_pop(Event& event) noexcept
src/engine.cpp|MarketIngress::normalize(
src/engine.cpp|MarketIngress::publish(
src/engine.cpp|MarketIngress::ingest(
src/engine.cpp|MarketIngress::OnRtnDepthMarketData(
src/engine.cpp|void run(std::atomic<bool>& stopping) noexcept
src/engine.cpp|RiskSnapshot risk_snapshot(
src/engine.cpp|LiveAccountWorker::poll_once(
src/strategy.cpp|ThresholdStrategy::on_market(
src/trading.cpp|AccountTradingState::create_order(
src/trading.cpp|AccountTradingState::apply_local_event(
src/trading.cpp|AccountTradingState::apply_order_report(
src/trading.cpp|AccountTradingState::apply_trade(
src/trading.cpp|RiskRejectReason evaluate_risk(
src/trading.cpp|AccountTradingSession::submit(
src/trading.cpp|AccountTradingSession::cancel(
src/trading.cpp|AccountTradingSession::on_market(
src/trading.cpp|AccountTradingSession::drain_callbacks(
src/trading.cpp|void push_callback(const CallbackEvent& event) noexcept
src/trading.cpp|void trace(
src/trading.cpp|void record_recovery_response(
src/trading.cpp|void set_recovery_phase(
src/trading.cpp|void recovery_failure(
src/trading.cpp|AccountTradingSession::OnRspAuthenticate(
src/trading.cpp|AccountTradingSession::OnRspUserLogin(
src/trading.cpp|AccountTradingSession::OnRspQrySettlementInfoConfirm(
src/trading.cpp|AccountTradingSession::OnRspSettlementInfoConfirm(
src/trading.cpp|AccountTradingSession::OnRspQryOrder(
src/trading.cpp|AccountTradingSession::OnRspQryTrade(
src/trading.cpp|AccountTradingSession::OnRspQryInvestorPosition(
src/trading.cpp|AccountTradingSession::OnRspQryTradingAccount(
src/trading.cpp|AccountTradingSession::OnRtnOrder(
src/trading.cpp|AccountTradingSession::OnRtnTrade(
src/trading.cpp|AccountTradingSession::OnRspOrderInsert(
src/trading.cpp|AccountTradingSession::OnRspOrderAction(
src/trader_client.cpp|int request_order_insert(
src/trader_client.cpp|int request_order_action(
src/telemetry.cpp|AsyncTraceJournal::try_record(
src/telemetry.cpp|AsyncPerformanceRecorder::try_record(
HOT_PATHS

    local forbidden='std::(mutex|timed_mutex|recursive_mutex|shared_mutex|lock_guard|unique_lock|scoped_lock|condition_variable|cout|cerr|clog|ofstream)|(std::string)([^_[:alnum:]]|$)|std::(vector|deque|list|map|unordered_map|set|unordered_set)[[:space:]<]|this_thread::sleep_|\.(push_back|emplace_back|reserve|resize)\(|(^|[^[:alnum:]_])(malloc|calloc|realloc|free|printf|fprintf|fwrite|operator[[:space:]]+new|make_unique|make_shared|new|delete)[[:space:]<(]'
    if grep -En -- "$forbidden" "$temporary_file"; then
        echo "[fail] application hot path contains a forbidden API" >&2
        return 1
    fi
    echo "[ok] audited application hot-path entry points contain no forbidden API"
    echo "[note] SDK internals and startup, shutdown, configuration and file writers are outside this static promise"
}

case "$1" in
    secrets) audit_secrets ;;
    hot-path) audit_hot_path ;;
    all)
        audit_secrets
        audit_hot_path
        ;;
esac
