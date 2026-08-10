#!/bin/bash
# Smoke test: compile + selftest + JSON validity
set -e
cd "$(dirname "$0")"
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o /tmp/sched-watch-test
/tmp/sched-watch-test --selftest
/tmp/sched-watch-test --json > /tmp/sched-watch-json.txt
python3 -c "import json; d=json.load(open('/tmp/sched-watch-json.txt')); assert d['summary']['cpus']>0; assert d['counters']['ctxt']>0; print('JSON OK:', d['loadavg'], d['summary'])"
echo "SMOKE TEST PASSED"
