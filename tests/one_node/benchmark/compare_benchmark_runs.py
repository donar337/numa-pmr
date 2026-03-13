#!/usr/bin/env python3
"""
Pair Google Benchmark JSON rows by scenario (all name segments except backend:N)
and compare NUMA (default backend 0) vs std::pmr::new_delete (default backend 1).

Input: one JSON document from Google Benchmark (--benchmark_format=json), either a
file path or stdin (use -).

Example:
  ./one_node_bench_allocators --benchmark_format=json 2>/dev/null > bench.json
  python3 compare_benchmark_runs.py bench.json

Сортировка строк таблицы/CSV: сначала алфавит по имени бенча (первый сегмент ключа),
затем по числам в сегментах вида name:value слева направо.

./build-bench-release/tests/one_node/one_node_bench_allocators --benchmark_format=json 2>/dev/null | python3 tests/one_node/benchmark/compare_benchmark_runs.py
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import sys
from dataclasses import dataclass
from typing import Any, TextIO


@dataclass(frozen=True)
class BenchRow:
    name: str
    scenario_key: str
    backend: int
    metrics: dict[str, float]


BACKEND_SEG_RE = re.compile(r"^backend:(\d+)$")


def split_scenario_backend(name: str) -> tuple[str, int | None]:
    parts = name.split("/")
    backend: int | None = None
    kept: list[str] = []
    for p in parts:
        m = BACKEND_SEG_RE.match(p)
        if m:
            backend = int(m.group(1))
        else:
            kept.append(p)
    return "/".join(kept), backend


def scenario_sort_key(scenario_key: str) -> tuple[str, tuple[int, ...]]:
    """Первый сегмент (имя бенча) — по алфавиту; далее числа из пар key:value подряд, по возрастанию покомпонентно."""
    parts = scenario_key.split("/")
    family = parts[0]
    nums: list[int] = []
    for seg in parts[1:]:
        if ":" not in seg:
            continue
        _, _, rhs = seg.partition(":")
        try:
            nums.append(int(rhs))
        except ValueError:
            nums.append(0)
    return (family, tuple(nums))


def get_json_metric(obj: dict[str, Any], metric: str) -> float:
    if metric in obj:
        v = obj[metric]
        if v is None:
            raise KeyError(metric)
        return float(v)
    cur: Any = obj
    for part in metric.split("."):
        if not isinstance(cur, dict) or part not in cur:
            raise KeyError(metric)
        cur = cur[part]
    return float(cur)


def infer_higher_is_better(metric: str, override: str | None) -> bool:
    if override == "lower":
        return False
    if override == "higher":
        return True
    m = metric.lower()
    if "time" in m and "per_second" not in m:
        return False
    if m.endswith("_per_second") or "items_per_second" in m or "bytes_per_second" in m:
        return True
    return False


def extract_rows_from_json(data: Any) -> list[BenchRow]:
    benchmarks = data["benchmarks"]
    rows: list[BenchRow] = []
    for b in benchmarks:
        if b.get("run_type") != "iteration":
            continue
        name = str(b["name"])
        key, backend = split_scenario_backend(name)
        if backend is None:
            continue
        metrics: dict[str, float] = {}
        for k, v in b.items():
            if k in ("name", "run_name", "run_type", "family_index", "per_family_instance_index"):
                continue
            if isinstance(v, bool):
                continue
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                try:
                    metrics[k] = float(v)
                except (TypeError, ValueError):
                    pass
        rows.append(BenchRow(name=name, scenario_key=key, backend=backend, metrics=metrics))
    return rows


def load_rows(input_path: str) -> list[BenchRow]:
    if input_path == "-":
        text_io: TextIO = io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8", errors="replace")
        data = json.load(text_io)
    else:
        with open(input_path, encoding="utf-8", errors="replace") as f:
            data = json.load(f)
    return extract_rows_from_json(data)


def pair_rows(
    rows: list[BenchRow],
    numa_backend: int,
    baseline_backend: int,
) -> dict[str, tuple[BenchRow, BenchRow]]:
    by_key: dict[str, dict[int, BenchRow]] = {}
    for r in rows:
        by_key.setdefault(r.scenario_key, {})[r.backend] = r
    pairs: dict[str, tuple[BenchRow, BenchRow]] = {}
    for key, m in sorted(by_key.items()):
        if numa_backend not in m or baseline_backend not in m:
            have = sorted(m.keys())
            print(f"warning: incomplete pair for {key!r}: backends present {have}", file=sys.stderr)
            continue
        pairs[key] = (m[numa_backend], m[baseline_backend])
    return pairs


def advantage_ratio(numa_val: float, baseline_val: float, higher_is_better: bool) -> float:
    if baseline_val == 0:
        return float("nan")
    if higher_is_better:
        return numa_val / baseline_val
    return baseline_val / numa_val


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "json_path",
        nargs="?",
        default="-",
        metavar="PATH",
        help=(
            "Path to a JSON file produced by Google Benchmark (--benchmark_format=json). "
            "Use - to read JSON from stdin (default: -)."
        ),
    )
    p.add_argument(
        "--metric",
        default="cpu_time",
        help=(
            "Metric key from JSON benchmark objects (default: cpu_time). "
            "Examples: real_time, bytes_per_second, items_per_second"
        ),
    )
    p.add_argument(
        "--prefer-better-direction",
        choices=("auto", "lower", "higher"),
        default="auto",
        help="Whether larger metric is better (throughput) or smaller (time). Default: infer from metric name.",
    )
    p.add_argument("--numa-backend", type=int, default=0, help="Backend id for NUMA / primary allocator (default: 0)")
    p.add_argument(
        "--baseline-backend",
        type=int,
        default=1,
        help="Backend id for baseline / new_delete (default: 1)",
    )
    p.add_argument("--top", type=int, default=5, help="How many best/worst scenarios to list (default: 5)")
    p.add_argument("--csv", action="store_true", help="Print comparison table as CSV on stdout")
    args = p.parse_args()

    rows = load_rows(args.json_path)
    higher = infer_higher_is_better(args.metric, args.prefer_better_direction)
    pairs = pair_rows(rows, args.numa_backend, args.baseline_backend)

    results: list[tuple[str, float, float, float]] = []
    for key, (numa_r, base_r) in pairs.items():
        try:
            nv = get_json_metric(numa_r.metrics, args.metric)
            bv = get_json_metric(base_r.metrics, args.metric)
        except KeyError:
            print(
                f"warning: metric {args.metric!r} missing for scenario {key!r} — skipping",
                file=sys.stderr,
            )
            continue
        ratio = advantage_ratio(nv, bv, higher)
        results.append((key, nv, bv, ratio))

    if not results:
        print("No paired scenarios with requested metric.", file=sys.stderr)
        return 1

    by_ratio = sorted(results, key=lambda t: t[3], reverse=True)
    results_display = sorted(results, key=lambda t: scenario_sort_key(t[0]))

    if args.csv:
        w = csv.writer(sys.stdout)
        w.writerow(["scenario", f"numa_{args.metric}", f"baseline_{args.metric}", "numa_advantage_ratio", "better"])
        for key, nv, bv, ratio in results_display:
            better = "numa" if ratio > 1 else ("tie" if ratio == 1 else "baseline")
            w.writerow([key, nv, bv, ratio, better])
        return 0

    dir_note = "higher metric is better" if higher else "lower metric is better"
    print(f"Metric: {args.metric} ({dir_note})")
    print(f"NUMA backend {args.numa_backend}, baseline backend {args.baseline_backend}")
    print("advantage_ratio: >1 means NUMA is better for this metric.")
    print()
    print(f"{'scenario':<64} {'numa':>14} {'baseline':>14} {'advantage':>12}")
    print("-" * 106)
    for key, nv, bv, ratio in results_display:
        print(f"{key:<64} {nv:14.6g} {bv:14.6g} {ratio:12.4f}")

    k = min(args.top, len(by_ratio))
    print()
    print(f"Top {k} scenarios for NUMA (largest advantage_ratio):")
    for key, nv, bv, ratio in by_ratio[:k]:
        print(f"  {ratio:8.4f}  {key}")

    print()
    print(f"Bottom {k} scenarios for NUMA (smallest advantage_ratio / regressions):")
    for key, nv, bv, ratio in by_ratio[-k:][::-1]:
        print(f"  {ratio:8.4f}  {key}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
