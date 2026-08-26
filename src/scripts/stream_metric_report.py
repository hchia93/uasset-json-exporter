# Joins stream metric timings with level scale, so streaming cost can be read against what is
# actually in each sublevel instead of as bare milliseconds.
#
# Inputs:
#   Saved/StreamMetric/stream_metric_result.json      from run_stream_metric.ps1
#   Intermediate/UAssetExport/**/<level>.json         from LevelExport
#
# Usage:
#   python stream_metric_report.py [--project <dir>] [--result <path>] [--csv <path>]

import argparse
import glob
import json
import os
import sys

from level_budget_audit import audit_level, default_project_dir, latest_exports


def load_timings(result_path):
    with open(result_path, encoding="utf-8") as f:
        result = json.load(f)

    if result.get("status") != "done":
        print("probe did not finish, status:", result.get("status"))
        if result.get("error"):
            print(result["error"])

    return result.get("persistent", "?"), {entry["name"]: entry for entry in result.get("levels", [])}


def load_scale(project_dir):
    pattern = os.path.join(project_dir, "Intermediate", "UAssetExport", "Game", "**", "*.json")
    scale = {}

    for path in latest_exports(pattern):
        try:
            row = audit_level(path)
        except (ValueError, KeyError):
            continue
        scale[row["name"]] = row

    return scale


def main():
    parser = argparse.ArgumentParser(description="Join stream metric timings with level scale.")
    parser.add_argument("--project", default=default_project_dir())
    parser.add_argument("--result", default=None, help="stream metric result JSON, defaults under Saved/StreamMetric")
    parser.add_argument("--csv", default=None, help="also write the joined rows to this CSV path")
    args = parser.parse_args()

    result_path = args.result or os.path.join(args.project, "Saved", "StreamMetric", "stream_metric_result.json")
    if not os.path.isfile(result_path):
        print("no stream metric result at:", result_path)
        return 1

    persistent, timings = load_timings(result_path)
    if not timings:
        print("stream metric result carries no per level timings")
        return 1

    scale = load_scale(args.project)

    rows = []
    missing_scale = []
    for name, timing in timings.items():
        measured = scale.get(name)
        if measured is None:
            missing_scale.append(name)
            continue

        components = measured["components"]
        rows.append({
            "name": name,
            "actors": measured["actors"],
            "components": components,
            "unique_static_meshes": measured["unique_static_meshes"],
            "load_ms": timing.get("load_wall_ms", 0.0),
            "unload_ms": timing.get("unload_wall_ms", 0.0),
            "gc_ms": timing.get("gc_ms", 0.0),
            # Cost per unit of content is what shows whether load time tracks size at all.
            "load_ms_per_1k": (timing.get("load_wall_ms", 0.0) / components * 1000.0) if components else 0.0,
        })

    if missing_scale:
        print("no LevelExport output for, run LevelExport on these first:")
        for name in sorted(missing_scale):
            print("   ", name)
        print()

    if not rows:
        return 1

    rows.sort(key=lambda r: -r["components"])

    header = f"{'sublevel':38} {'actors':>7} {'comps':>7} {'uniqSM':>7} {'load_ms':>9} {'ms/1k':>8} {'unload_ms':>10} {'gc_ms':>8}"
    print("persistent:", persistent)
    print()
    print(header)
    print("-" * len(header))
    for row in rows:
        print(
            f"{row['name'][:38]:38} {row['actors']:>7} {row['components']:>7} {row['unique_static_meshes']:>7} "
            f"{row['load_ms']:>9.1f} {row['load_ms_per_1k']:>8.1f} {row['unload_ms']:>10.1f} {row['gc_ms']:>8.1f}"
        )
    print("-" * len(header))

    total_components = sum(r["components"] for r in rows)
    total_load = sum(r["load_ms"] for r in rows)
    total_unload = sum(r["unload_ms"] for r in rows)
    total_gc = sum(r["gc_ms"] for r in rows)
    overall_per_1k = (total_load / total_components * 1000.0) if total_components else 0.0
    print(
        f"{'TOTAL':38} {sum(r['actors'] for r in rows):>7} {total_components:>7} {'':>7} "
        f"{total_load:>9.1f} {overall_per_1k:>8.1f} {total_unload:>10.1f} {total_gc:>8.1f}"
    )

    if args.csv:
        import csv

        with open(args.csv, "w", encoding="utf-8", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print("\ncsv:", args.csv)

    return 0


if __name__ == "__main__":
    sys.exit(main())
