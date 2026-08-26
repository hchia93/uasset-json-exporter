# Static level budget audit over UAssetWorkbench LevelExport output.
# Reads the exported level JSON and prints a per-level table against a component budget.
#
# Usage:
#   python level_budget_audit.py [--project <dir>] [--levels <glob>] [--budget <n>]
#
# Defaults assume this script still sits under <Project>/Plugins/UAssetWorkbench/scripts/.

import argparse
import glob
import json
import os
import re
import sys

DEFAULT_COMPONENT_BUDGET = 2000
STATIC_MESH_PATH = re.compile(r"'(/Game/[^']+)'")
LIGHT_CLASSES = ("PointLight", "SpotLight", "RectLight")


EXPORT_STAMP = re.compile(r"_r(?:\d+|NA)_\d{8}-\d{6}$")


def latest_exports(pattern):
    """One capture per asset. Export names carry revision and capture time, so the same asset
    accumulates many files, only the newest describes what is on disk now."""
    newest = {}
    for path in glob.glob(pattern, recursive=True):
        stem = os.path.splitext(os.path.basename(path))[0]
        key = (os.path.dirname(path), EXPORT_STAMP.sub("", stem))
        if key not in newest or os.path.getmtime(path) > os.path.getmtime(newest[key]):
            newest[key] = path
    return sorted(newest.values())


def default_project_dir():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def audit_level(path):
    with open(path, encoding="utf-8") as f:
        level = json.load(f)

    actors = level.get("Actors", [])
    component_count = 0
    lights = 0
    spline_mesh_components = 0
    static_meshes = set()

    for actor in actors:
        actor_class = actor.get("Class", "").split(".")[-1]
        components = actor.get("Components", []) or []
        component_count += len(components)

        if actor_class in LIGHT_CLASSES:
            lights += 1

        for component in components:
            if "SplineMesh" in component.get("Class", ""):
                spline_mesh_components += 1

            delta = component.get("DeltaProperties", {}) or {}
            found = STATIC_MESH_PATH.search(delta.get("StaticMesh", "") or "")
            if found:
                static_meshes.add(found.group(1).split(".")[0])

    return {
        "name": level.get("LevelName", os.path.basename(path)),
        "exported": level.get("ExportTimestamp", "?"),
        "actors": len(actors),
        "components": component_count,
        "lights": lights,
        "spline_mesh_components": spline_mesh_components,
        "unique_static_meshes": len(static_meshes),
    }


def main():
    parser = argparse.ArgumentParser(description="Component budget audit over LevelExport JSON.")
    parser.add_argument("--project", default=default_project_dir(), help="project directory holding Intermediate/UAssetExport")
    parser.add_argument("--levels", default="Game/**/*.json", help="glob under Intermediate/UAssetExport, relative")
    parser.add_argument("--budget", type=int, default=DEFAULT_COMPONENT_BUDGET, help="component budget per level")
    args = parser.parse_args()

    pattern = os.path.join(args.project, "Intermediate", "UAssetExport", args.levels)
    paths = latest_exports(pattern)
    if not paths:
        print("no level exports found:", pattern)
        return 1

    rows = []
    for path in paths:
        try:
            rows.append(audit_level(path))
        except (ValueError, KeyError):
            print("skipped unreadable export:", path)

    if not rows:
        print("no readable level exports under:", pattern)
        return 1

    header = f"{'Level':40} {'Actors':>6} {'Comps':>7} {'Lights':>6} {'SplineC':>7} {'UniqSM':>6} {'xBudget':>7}"
    print(header)
    for row in rows:
        ratio = row["components"] / args.budget
        flag = " <== OVER" if ratio > 1.0 else ""
        print(
            f"{row['name']:40} {row['actors']:>6} {row['components']:>7} {row['lights']:>6} "
            f"{row['spline_mesh_components']:>7} {row['unique_static_meshes']:>6} {ratio:>6.1f}x{flag}"
        )

    print(f"\ncomponent budget: {args.budget}")
    export_dates = {row["exported"].split("-")[0] for row in rows}
    print("export dates:", ", ".join(sorted(export_dates)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
