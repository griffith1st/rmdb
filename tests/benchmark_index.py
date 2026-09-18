"""Reproducible local index measurement; latency is never a test assertion.

Run from the repository root on Linux/WSL:
    python3 tests/benchmark_index.py build/bin/rmdb --output docs/index-benchmark.json
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import sys
import tempfile
import time

from sql_harness import Server


ROW_COUNT = 10_000
REPEATS = 3
THRESHOLD = 0.70


def workload():
    cases = []
    for index in range(20):
        key = 1 + index * 499
        cases.append({
            "kind": "equality",
            "sql": f"select id,payload from bench where id={key};",
            "expected": [(str(key), str(key * 7))],
        })
    for lower in range(1, 10_001, 1000):
        cases.append({
            "kind": "range",
            "sql": (f"select id,payload from bench where id>={lower} "
                    f"and id<{lower + 5} order by id;"),
            "expected": [(str(key), str(key * 7)) for key in range(lower, lower + 5)],
        })
    return cases


def run_batch(server, client, cases):
    start = time.perf_counter_ns()
    results = [server.rows(client, case["sql"]) for case in cases]
    elapsed = (time.perf_counter_ns() - start) / 1_000_000_000
    # Validate outside the timed interval against values derived from the fixture,
    # never against an earlier result that could share the same server defect.
    for case, result in zip(cases, results):
        if result != case["expected"]:
            raise AssertionError(f"Incorrect result for {case['sql']}: {result!r}")
    return elapsed, results


def measure(server, client, cases):
    warmup, reference = run_batch(server, client, cases)
    batches = []
    for _ in range(REPEATS):
        elapsed, results = run_batch(server, client, cases)
        if results != reference:
            raise AssertionError("Query results changed between measurement batches")
        batches.append(elapsed)
    return {
        "warmup_seconds_excluded": warmup,
        "batch_seconds": batches,
        "median_batch_seconds": statistics.median(batches),
    }, reference


def cpu_model():
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                return line.partition(":")[2].strip()
    return platform.processor()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default="build/bin/rmdb")
    parser.add_argument("--output", default="docs/index-benchmark.json")
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    binary_bytes = binary.read_bytes()
    binary_hash = hashlib.sha256(binary_bytes).hexdigest()
    started = datetime.now(timezone.utc).isoformat()
    cases = workload()

    with Server(binary) as server:
        client = server.start()
        setup_started = time.perf_counter()
        server.query(client, "create table bench (id int, payload int);")
        server.query(client, "begin;")
        for key in range(1, ROW_COUNT + 1):
            server.query(client, f"insert into bench values ({key},{key * 7});")
            if key % 2500 == 0:
                print(f"Prepared {key}/{ROW_COUNT} rows", file=sys.stderr, flush=True)
        server.query(client, "commit;")
        setup_seconds = time.perf_counter() - setup_started
        if server.rows(client, "select COUNT(*) as n from bench;") != [(str(ROW_COUNT),)]:
            raise AssertionError("Fixture row count mismatch")

        print("Measuring sequential scans", file=sys.stderr, flush=True)
        without_index, before = measure(server, client, cases)
        index_started = time.perf_counter()
        server.query(client, "create index bench(id);")
        index_build_seconds = time.perf_counter() - index_started
        print("Measuring indexed scans", file=sys.stderr, flush=True)
        with_index, after = measure(server, client, cases)
        if before != after:
            raise AssertionError("Index creation changed query results")

    if hashlib.sha256(binary.read_bytes()).hexdigest() != binary_hash:
        raise RuntimeError("Server binary changed during measurement; rerun with a stable build")

    ratio = with_index["median_batch_seconds"] / without_index["median_batch_seconds"]
    report = {
        "schema_version": 1,
        "started_at_utc": started,
        "completed_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Local reproducible measurement only; not an official grading result.",
        "environment": {
            "platform": platform.platform(),
            "kernel": platform.release(),
            "machine": platform.machine(),
            "cpu_model": cpu_model(),
            "logical_cpu_count": os.cpu_count(),
            "python": platform.python_version(),
            "binary": str(binary),
            "binary_sha256": binary_hash,
            "binary_bytes": len(binary_bytes),
            "temporary_database_parent": tempfile.gettempdir(),
            "transport": "TCP loopback 127.0.0.1:8765, one client, shared harness port lock",
            "cache_policy": "One complete warmup batch per phase; OS/DB caches are not cleared.",
        },
        "workload": {
            "row_count": ROW_COUNT,
            "schema": "bench(id INT, payload INT)",
            "data": "id=1..10000, payload=id*7; inserted in one explicit transaction",
            "index": "unique single-column index bench(id)",
            "queries_per_batch": len(cases),
            "equality_queries_per_batch": 20,
            "range_queries_per_batch": 10,
            "expected_rows_per_batch": sum(len(case["expected"]) for case in cases),
            "warmup_batches_per_phase": 1,
            "measured_batches_per_phase": REPEATS,
            "queries": [{"kind": case["kind"], "sql": case["sql"],
                         "expected_row_count": len(case["expected"])} for case in cases],
        },
        "measurement": {
            "clock": "time.perf_counter_ns",
            "unit": "seconds",
            "timed_scope": ("Total end-to-end latency of all 30 queries in a batch, including "
                            "TCP request/response, server execution/output/auto-commit, and "
                            "harness output.txt reading/parsing. Result assertions, setup, "
                            "index creation, and warmup are excluded."),
            "setup_seconds_excluded": setup_seconds,
            "index_build_seconds_excluded": index_build_seconds,
            "without_index": without_index,
            "with_index": with_index,
            "indexed_to_unindexed_median_ratio": ratio,
            "indexed_to_unindexed_percent": ratio * 100,
            "criterion_ratio_from_test_document": THRESHOLD,
            "local_measurement_meets_criterion": ratio <= THRESHOLD,
        },
        "correctness": {
            "all_batches_match_fixture": True,
            "before_after_results_identical": True,
            "validated_query_executions": len(cases) * (REPEATS + 1) * 2,
            "result_sha256": hashlib.sha256(json.dumps(before, separators=(",", ":")).encode()).hexdigest(),
        },
        "limitations": [
            "The official test document requires time_b/time_a <= 0.70; this local workload and host differ from grading.",
            "Only a single INT-column index is measured; the official composite-index speed criterion is not measured.",
            "Latency varies with host load, scheduling, caches, and filesystem behavior; no timing assertion is added to CTest.",
            "The existing index uses an ordered in-memory vector; this measurement does not demonstrate a disk B+ tree.",
            "Index creation and inserts are excluded from query latency, so this is not an overall workload-speedup claim.",
        ],
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(output), "queries_per_batch": len(cases),
                      "without_index_median_seconds": without_index["median_batch_seconds"],
                      "with_index_median_seconds": with_index["median_batch_seconds"],
                      "ratio": ratio, "local_criterion_met": ratio <= THRESHOLD}, indent=2))


if __name__ == "__main__":
    main()
