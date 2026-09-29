#!/usr/bin/env python3
"""Merges the GpuSort CSV files of several runs / machines into one set of CSV files.

Usage:
    python tools/aggregate_results.py <input> [<input> ...] [-o <out dir>] [--include-smoke]

<input>: results folders, zips (the results\\<COMPUTER>_<date>.zip that run_all.bat writes, or any
zip / folder containing them) or glob patterns such as "G:\\My Drive\\*.zip". Folders are searched
recursively, zips too (also zips inside zips). Every GpuSort CSV found is used, recognized by its
header (not its name): the results CSV (<stem>.csv, schema_version + median_us), the samples CSV
(<stem>_samples.csv) and the wave probe CSV (<stem>_wave_probe.csv, or wave_probe.csv of a
--wave-probe run). Other CSV files are ignored.

Writes into <out dir> (default: _test/final/ in the repository):
    all_results.csv      every results row (+ column 'source': the file it came from)
    all_samples.csv      every samples row (columns as in the samples CSV)
    all_wave_probe.csv   every wave probe row (+ column 'source')
    aggregate_summary.txt  the summary printed at the end
Runs are identified by run_id (<timestamp>_<computer>, one per GpuSort.exe run). The same run found
twice (e.g. the results folder and its zip) is used once. Smoke runs (run_kind 'smoke': a few serial
iterations, a safety check) are left out unless --include-smoke; their verification failures are
still reported. The columns are described in tools/CSV_FORMAT.md.

Python 3.8+ standard library only.
"""

import argparse
import csv
import glob
import io
import os
import sys
import zipfile
from collections import OrderedDict, defaultdict
from pathlib import Path

SUPPORTED_SCHEMA_VERSIONS = {"1", "2", "3", "4"}

# Required in every results CSV (schema_version 1 to 4).
RESULTS_COLUMNS = [
    "schema_version", "run_id", "run_timestamp", "computer", "label", "package_commit", "shader_set",
    "gpu_index", "gpu_name", "vendor_id", "device_id", "driver_version", "is_integrated", "dedicated_vram_mb",
    "wave_lane_min", "wave_lane_max", "wave_size", "wave_size_attr", "timestamp_freq_hz", "algorithm",
    "flush_mode", "workload", "sweep_size", "sorts_per_iteration", "iterations", "warmup", "min_us",
    "median_us", "mean_us", "p95_us", "p99_us", "max_us", "stddev_us", "verify_failures",
    "total_elements_per_iter_mean", "algorithm_id", "run_kind", "iterations_requested", "wave_probe_ok",
    "gpu_error",
]
# Added in schema_version 2 (results CSV); empty in the output for version 1 rows.
RESULTS_COLUMNS_V2 = ["stable_power", "drain_spin_iters_per_us"]
# Added in schema_version 3 (results CSV); empty in the output for version 1 / 2 rows.
RESULTS_COLUMNS_V3 = ["dispatch_info"]
# Added in schema_version 4 (results CSV, the final set); empty in the output for version 1 to 3 rows.
# (Also since 4: iterations_requested is per GPU, and the default flush_mode is full_d50.)
RESULTS_COLUMNS_V4 = ["pass", "description", "tags"]
SAMPLES_COLUMNS = [
    "run_id", "gpu_index", "wave_size", "algorithm", "flush_mode", "workload", "iteration", "time_us",
    "largest_sort", "total_elements", "sweep_size",
]
PROBE_REQUIRED = ["schema_version", "run_id", "computer", "gpu_index", "gpu_name", "probe_variant",
                  "observed_lanes", "verdict"]


# --- input discovery ------------------------------------------------------------------------------

class CsvSource:
    """One CSV file on disk or inside a (possibly nested) zip."""

    def __init__(self, name, opener):
        self.name = name      # for messages and the 'source' column
        self._opener = opener  # () -> binary file object

    def open_text(self):
        return io.TextIOWrapper(self._opener(), encoding="utf-8-sig", newline="")


def _zip_sources(zf, prefix, sources, warnings):
    for info in sorted(zf.infolist(), key=lambda i: i.filename):
        if info.is_dir():
            continue
        name = info.filename
        lower = name.lower()
        display = prefix + "!" + name
        if lower.endswith(".csv"):
            sources.append(CsvSource(display, lambda zf=zf, name=name: zf.open(name)))
        elif lower.endswith(".zip"):
            try:
                inner = zipfile.ZipFile(io.BytesIO(zf.read(name)))
            except (zipfile.BadZipFile, OSError) as e:
                warnings.append(f"cannot read zip {display}: {e}")
                continue
            _zip_sources(inner, display, sources, warnings)


def discover(inputs, out_dir, warnings):
    """Returns the CSV sources of all inputs, in a stable order."""
    paths = []
    for pattern in inputs:
        # Expanded here: cmd.exe / PowerShell pass wildcards through unexpanded.
        matches = glob.glob(pattern) if any(c in pattern for c in "*?[") else [pattern]
        if not matches:
            warnings.append(f"no match for {pattern}")
        paths.extend(sorted(matches))

    out_dir = os.path.normcase(os.path.abspath(out_dir))

    def in_out_dir(p):
        p = os.path.normcase(os.path.abspath(p))
        return p == out_dir or p.startswith(out_dir + os.sep)

    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs.sort()
                for n in sorted(names):
                    if n.lower().endswith((".csv", ".zip")):
                        files.append(os.path.join(root, n))
        elif os.path.isfile(p):
            files.append(p)
        else:
            warnings.append(f"not found: {p}")

    sources = []
    seen = set()
    for f in files:
        key = os.path.normcase(os.path.abspath(f))
        if key in seen or in_out_dir(f):
            continue
        seen.add(key)
        if f.lower().endswith(".zip"):
            try:
                zf = zipfile.ZipFile(f)
            except (zipfile.BadZipFile, OSError) as e:
                warnings.append(f"cannot read zip {f}: {e}")
                continue
            _zip_sources(zf, f, sources, warnings)
        elif f.lower().endswith(".csv"):
            sources.append(CsvSource(f, lambda f=f: open(f, "rb")))
    return sources


def classify(header):
    if not header:
        return None
    if header[0] == "schema_version" and "median_us" in header:
        return "results"
    if header[0] == "schema_version" and "observed_lanes" in header:
        return "probe"
    if header[0] == "run_id" and "time_us" in header:
        return "samples"
    return None


# --- reading ----------------------------------------------------------------------------------------

def read_rows(src):
    with src.open_text() as f:
        reader = csv.reader(f)
        header = next(reader, None)
        rows = [r for r in reader if r]
    return header, rows


def load_keyed(src, header, rows, kind, store, columns, stats, warnings):
    """Adds the rows of one results / probe file to store[run_id] (de-duplicated by run_id)."""
    missing = [c for c in (RESULTS_COLUMNS if kind == "results" else PROBE_REQUIRED) if c not in header]
    if missing:
        warnings.append(f"{src.name}: missing columns {', '.join(missing)}; file skipped")
        return
    dicts = []
    for r in rows:
        if len(r) != len(header):
            warnings.append(f"{src.name}: a row has {len(r)} fields, the header {len(header)}; file skipped")
            return
        d = OrderedDict(zip(header, r))
        d.pop("source", None)  # re-aggregating an all_*.csv: 'source' is set again below
        if d["schema_version"] not in SUPPORTED_SCHEMA_VERSIONS:
            warnings.append(f"{src.name}: schema_version {d['schema_version']} is not supported "
                            f"(supported: {', '.join(sorted(SUPPORTED_SCHEMA_VERSIONS))}); file skipped")
            return
        dicts.append(d)
    for c in header:
        if c != "source" and c not in columns:
            columns.append(c)
    by_run = OrderedDict()
    for d in dicts:
        by_run.setdefault(d["run_id"], []).append(d)
    for run_id, run_rows in by_run.items():
        for d in run_rows:
            d["source"] = src.name
        if run_id in store:
            first = store[run_id]
            same = [tuple((k, v) for k, v in d.items() if k != "source") for d in first] == \
                   [tuple((k, v) for k, v in d.items() if k != "source") for d in run_rows]
            if same:
                stats[kind + "_duplicate_runs"] += 1
            else:
                warnings.append(f"run {run_id}: {kind} rows in {src.name} differ from {first[0]['source']} "
                                f"(same run_id, different content); kept the first")
            continue
        store[run_id] = run_rows


# --- summary helpers -------------------------------------------------------------------------------

def fmt_set(values):
    return ", ".join(sorted(values, key=lambda v: (len(v), v))) if values else "-"


def main():
    repo = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("inputs", nargs="+", help="results folders, zips or glob patterns")
    ap.add_argument("-o", "--out", default=str(repo / "_test" / "final"),
                    help="output folder (default: %(default)s)")
    ap.add_argument("--include-smoke", action="store_true",
                    help="also include smoke runs (run_kind 'smoke') in the output files")
    args = ap.parse_args()

    warnings = []
    stats = defaultdict(int)
    sources = discover(args.inputs, args.out, warnings)

    results = OrderedDict()   # run_id -> rows
    probes = OrderedDict()
    results_columns = (list(RESULTS_COLUMNS) + list(RESULTS_COLUMNS_V2) + list(RESULTS_COLUMNS_V3) +
                       list(RESULTS_COLUMNS_V4))
    probe_columns = []
    sample_sources = []
    for src in sources:
        try:
            with src.open_text() as f:
                header = next(csv.reader(f), None)
        except (OSError, UnicodeDecodeError, zipfile.BadZipFile) as e:
            warnings.append(f"cannot read {src.name}: {e}")
            continue
        kind = classify(header)
        if kind is None:
            stats["other_csv"] += 1
            continue
        stats[kind + "_files"] += 1
        if kind == "samples":
            missing = [c for c in SAMPLES_COLUMNS if c not in header]
            if missing:
                warnings.append(f"{src.name}: missing columns {', '.join(missing)}; file skipped")
                continue
            sample_sources.append(src)
            continue
        header, rows = read_rows(src)
        if kind == "results":
            load_keyed(src, header, rows, kind, results, results_columns, stats, warnings)
        else:
            load_keyed(src, header, rows, kind, probes, probe_columns, stats, warnings)

    # Runs and their kind (from the results or the probe rows).
    run_info = OrderedDict()
    for store in (results, probes):
        for run_id, rows in store.items():
            r = rows[0]
            info = run_info.setdefault(run_id, {
                "computer": r.get("computer", ""), "kind": r.get("run_kind", ""), "label": r.get("label", ""),
                "commit": r.get("package_commit", ""), "shader_sets": set(), "timestamp": r.get("run_timestamp", ""),
            })
            for row in rows:
                if row.get("shader_set"):
                    info["shader_sets"].add(row["shader_set"])
    smoke_runs = {rid for rid, i in run_info.items() if i["kind"] == "smoke"}
    excluded = set() if args.include_smoke else smoke_runs

    # --- write ----------------------------------------------------------------------------------------
    os.makedirs(args.out, exist_ok=True)
    out_results = os.path.join(args.out, "all_results.csv")
    out_samples = os.path.join(args.out, "all_samples.csv")
    out_probe = os.path.join(args.out, "all_wave_probe.csv")

    n_results = 0
    with open(out_results, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        cols = results_columns + ["source"]
        w.writerow(cols)
        for run_id, rows in results.items():
            if run_id in excluded:
                continue
            for d in rows:
                w.writerow([d.get(c, "") for c in cols])
                n_results += 1

    n_probe = 0
    if not probe_columns:
        probe_columns = list(PROBE_REQUIRED)
    with open(out_probe, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        cols = probe_columns + ["source"]
        w.writerow(cols)
        for run_id, rows in probes.items():
            if run_id in excluded:
                continue
            for d in rows:
                w.writerow([d.get(c, "") for c in cols])
                n_probe += 1

    # Samples: streamed (large); de-duplicated by run_id (a run's samples come from one file).
    n_samples = 0
    sample_runs_done = set()
    samples_per_run = defaultdict(int)
    samples_without_results = set()
    with open(out_samples, "w", newline="", encoding="utf-8") as fo:
        w = csv.writer(fo)
        w.writerow(SAMPLES_COLUMNS)
        for src in sample_sources:
            this_file = set()
            skipped_dup = 0
            try:
                with src.open_text() as f:
                    reader = csv.reader(f)
                    header = next(reader)
                    idx = [header.index(c) for c in SAMPLES_COLUMNS]
                    rid_i = header.index("run_id")
                    for r in reader:
                        if not r:
                            continue
                        rid = r[rid_i]
                        if rid in sample_runs_done:
                            skipped_dup += 1
                            continue
                        this_file.add(rid)
                        if rid in excluded:
                            continue
                        if rid not in results:
                            samples_without_results.add(rid)
                        w.writerow([r[i] for i in idx])
                        samples_per_run[rid] += 1
                        n_samples += 1
            except (OSError, UnicodeDecodeError, zipfile.BadZipFile, IndexError) as e:
                warnings.append(f"cannot read {src.name}: {e}")
            if skipped_dup and not this_file:
                stats["samples_duplicate_files"] += 1
            elif skipped_dup:
                warnings.append(f"{src.name}: {skipped_dup} rows of runs already read from another file skipped")
            sample_runs_done |= this_file

    # --- summary ----------------------------------------------------------------------------------------
    out = []
    p = out.append
    p("GpuSort results aggregation")
    p("===========================")
    p(f"Inputs: {', '.join(args.inputs)}")
    p(f"CSV files found: {stats['results_files']} results, {stats['samples_files']} samples, "
      f"{stats['probe_files']} wave probe, {stats['other_csv']} other (ignored)")
    dups = stats["results_duplicate_runs"] + stats["probe_duplicate_runs"] + stats["samples_duplicate_files"]
    if dups:
        p(f"Duplicates skipped (same run_id found again, e.g. folder + zip): {stats['results_duplicate_runs']} "
          f"results, {stats['probe_duplicate_runs']} wave probe, {stats['samples_duplicate_files']} samples")
    kinds = defaultdict(int)
    for i in run_info.values():
        kinds[i["kind"]] += 1
    p(f"Runs: {len(run_info)} (" + ", ".join(f"{n} {k}" for k, n in sorted(kinds.items())) + ")"
      + ("" if args.include_smoke or not smoke_runs else f"; {len(smoke_runs)} smoke run(s) left out "
                                                            f"of the output files (--include-smoke)"))
    p("")

    all_rows = [d for rows in results.values() for d in rows]
    used_rows = [d for rid, rows in results.items() if rid not in excluded for d in rows]

    # Machines, runs, commits.
    by_computer = OrderedDict()
    for rid, i in sorted(run_info.items(), key=lambda kv: (kv[1]["computer"], kv[1]["timestamp"])):
        by_computer.setdefault(i["computer"], []).append((rid, i))
    p(f"Machines ({len(by_computer)}):")
    for comp, runs in by_computer.items():
        commits = sorted({i["commit"] for _, i in runs})
        p(f"  {comp}: {len(runs)} run(s), package commit {', '.join(commits)}")
        for rid, i in runs:
            p(f"      {rid}  {i['kind']:<10} {fmt_set(i['shader_sets']):<10} {i['label']}")
    p("")

    # GPUs and wave configurations.
    gpus = OrderedDict()
    for d in all_rows:
        key = (d["computer"], d["gpu_name"], d["vendor_id"], d["device_id"])
        g = gpus.setdefault(key, {"drivers": set(), "integrated": d["is_integrated"], "waves": set(), "rows": 0})
        g["drivers"].add(d["driver_version"])
        g["waves"].add(f"{d['wave_size']}{'+attr' if d['wave_size_attr'] == '1' else ''}")
        g["rows"] += 1 if d["run_id"] not in excluded else 0
    p(f"GPUs ({len(gpus)}):")
    for (comp, name, ven, dev), g in gpus.items():
        p(f"  {comp}: {name} ({ven}:{dev}{', integrated' if g['integrated'] == '1' else ''}), driver "
          f"{fmt_set(g['drivers'])}, wave configs {fmt_set(g['waves'])} (WAVE_SIZE; +attr = [WaveSize]), "
          f"{g['rows']} result rows")
    p("")

    p(f"Shader sets: {fmt_set({d['shader_set'] for d in used_rows})}")
    p(f"Algorithms ({len({d['algorithm_id'] for d in used_rows})}): "
      f"{', '.join(sorted({d['algorithm_id'] for d in used_rows}))}")
    p(f"Flush modes: {fmt_set({d['flush_mode'] for d in used_rows})}")
    p(f"Stable power: {fmt_set({d.get('stable_power') or 'n/a (schema 1)' for d in used_rows})}")
    p(f"Workloads: {', '.join(sorted({d['workload'] for d in used_rows}))}")
    p(f"Sweep sizes: {fmt_set({d['sweep_size'] for d in used_rows if d['sweep_size']})}")
    iters = sorted({int(d["iterations_requested"]) for d in used_rows if d["iterations_requested"].isdigit()})
    p(f"Iterations requested per combo: {', '.join(map(str, iters)) or '-'}")
    p("")
    p("Rows written:")
    p(f"  {out_results}: {n_results}")
    p(f"  {out_samples}: {n_samples}")
    p(f"  {out_probe}: {n_probe}")
    p("")

    # Checks.
    problems = []
    fails = [d for d in all_rows if d["verify_failures"] not in ("", "0")]
    if fails:
        problems.append(f"VERIFICATION FAILURES in {len(fails)} result row(s):")
        for d in fails:
            size = f" size {d['sweep_size']}" if d["sweep_size"] else ""
            problems.append(f"    {d['computer']} / {d['gpu_name']} / wave {d['wave_size']} / {d['run_kind']} "
                            f"{d['shader_set']}: {d['algorithm_id']} on {d['workload']}{size}: "
                            f"{d['verify_failures']} failure(s)")
    errors = {(d["computer"], d["gpu_name"], d["run_id"], d["gpu_error"]) for d in all_rows if d["gpu_error"]}
    for comp, name, rid, err in sorted(errors):
        problems.append(f"GPU ERROR in run {rid} on {comp} / {name}: {err}")
    for rows in probes.values():
        for d in rows:
            if d.get("verdict") == "WARNING":
                problems.append(f"WAVE PROBE WARNING in run {d['run_id']} ({d.get('run_kind', '')}) on "
                                f"{d['computer']} / {d['gpu_name']}, {d['probe_variant']}: {d.get('problem', '')}")
    # Join keys must be unique (results <-> samples).
    keys = defaultdict(int)
    for d in used_rows:
        keys[(d["run_id"], d["gpu_index"], d["wave_size"], d["algorithm"], d["flush_mode"], d["workload"],
              d["sweep_size"])] += 1
    dup_keys = [k for k, n in keys.items() if n > 1]
    if dup_keys:
        problems.append(f"{len(dup_keys)} duplicate join key(s) (run_id, gpu_index, wave_size, algorithm, "
                        f"flush_mode, workload, sweep_size) in the results, e.g. {dup_keys[0]}; use algorithm_id")
    # Every non-excluded run with results should have samples, and vice versa.
    no_samples = [rid for rid in results if rid not in excluded and rid not in samples_per_run]
    if no_samples:
        problems.append(f"{len(no_samples)} run(s) without samples (--no-samples or missing file): "
                        f"{', '.join(no_samples)}")
    if samples_without_results:
        problems.append(f"samples of {len(samples_without_results)} run(s) without a results CSV: "
                        f"{', '.join(sorted(samples_without_results))}")
    for rid in results:
        if rid in excluded or rid not in samples_per_run:
            continue
        expected = sum(int(d["iterations"]) for d in results[rid] if d["iterations"].isdigit())
        if expected != samples_per_run[rid]:
            problems.append(f"run {rid}: {samples_per_run[rid]} samples, the results rows count {expected}")

    commits = defaultdict(set)
    for i in run_info.values():
        commits[i["commit"]].add(i["computer"])
    bad_commits = [c for c in commits if c == "unknown" or c.endswith("-dirty")]
    if len(commits) > 1:
        p("#" * 100)
        p("#  WARNING: THE MACHINES RAN DIFFERENT PACKAGE COMMITS - results may not be comparable:")
        for c, comps in sorted(commits.items()):
            p(f"#      {c}: {', '.join(sorted(comps))}")
        p("#" * 100)
        p("")
    if bad_commits:
        problems.append(f"package commit(s) {', '.join(sorted(bad_commits))}: not a clean committed build")

    if problems:
        p("Problems:")
        for line in problems:
            p("  " + line)
    else:
        p("Problems: none (no verification failures, GPU errors or wave probe warnings; one package commit)")
    if warnings:
        p("")
        p("Warnings:")
        for line in warnings:
            p("  " + line)

    text = "\n".join(out) + "\n"
    with open(os.path.join(args.out, "aggregate_summary.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    sys.stdout.write(text)
    return 1 if (problems or warnings or len(commits) > 1) else 0


if __name__ == "__main__":
    sys.exit(main())
