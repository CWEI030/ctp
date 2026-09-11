#!/usr/bin/env bash
set -euo pipefail
./build/ctp_client benchmark --config '/tmp/tmp.0lPoztF5Ng/accounts.ini' --input 'tests/data/replay/minimal_signal_v1.csv' --output 'runtime/performance/evidence-20260911T111455/accounts-1' --accounts 1 --rate 1000 --warmup-seconds 2 --duration-seconds 36 --submit-stride 1
