"""Build the compact data file for the GPU sort results page from the aggregated CSVs.

Usage: python tools/results_page/prep.py <agg dir> <out data.js> [--sorts N]

<agg dir> is an output folder of tools/aggregate_results.py (all_results.csv; all_samples.csv or
all_samples.csv.gz for the p25 / p75 quartiles). Without a samples file the quartiles are null
(the page shows them as "-"). --sorts: the sorts per iteration to use (default 20; the page shows
one batch size, and rows of other sizes, e.g. of a scale run, are left out).
"""
import argparse, csv, gzip, json, os, sys
from collections import defaultdict

ap = argparse.ArgumentParser(description="Build data.js for the results page")
ap.add_argument("agg")
ap.add_argument("out")
ap.add_argument("--sorts", default="20", help="sorts per iteration of the rows to use (default 20)")
args = ap.parse_args()
AGG = args.agg
OUT = args.out
SORTS = args.sorts

rows = [r for r in csv.DictReader(open(f"{AGG}/all_results.csv", encoding="utf-8"))
        if r["run_kind"] == "benchmark" and (r.get("sorts_per_iteration") or "20") == SORTS]

def cfg_key(r):
    return (r["gpu_name"], r["wave_size"])

# GPU configurations, fixed display order (discrete high end -> integrated).
ORDER = [("NVIDIA GeForce RTX 5080", "32"), ("NVIDIA GeForce RTX 3080 Ti", "32"), ("AMD Radeon RX 7900 XTX", "32"),
         ("AMD Radeon RX 7900 XTX", "64"), ("NVIDIA GeForce RTX 2060", "32"),
         ("AMD Radeon(TM) Graphics", "32"), ("Intel(R) UHD Graphics 770", "16")]
SHORT = {"NVIDIA GeForce RTX 5080": "RTX 5080", "AMD Radeon RX 7900 XTX": "RX 7900 XTX",
         "NVIDIA GeForce RTX 2060": "RTX 2060", "AMD Radeon(TM) Graphics": "Radeon iGPU",
         "NVIDIA GeForce RTX 3080 Ti": "RTX 3080 Ti", "Intel(R) UHD Graphics 770": "UHD 770"}
present = {cfg_key(r) for r in rows}
cfgs = [k for k in ORDER if k in present] + sorted(present - set(ORDER))
cfg_index = {k: i for i, k in enumerate(cfgs)}

cfg_meta = []
for k in cfgs:
    r = next(x for x in rows if cfg_key(x) == k)
    label = SHORT.get(k[0], k[0])
    if k[0] == "AMD Radeon RX 7900 XTX":
        label += f" · wave{k[1]}"
    cfg_meta.append({
        "label": label, "gpu": k[0], "wave": int(k[1]), "integrated": r["is_integrated"] == "1",
        "driver": r["driver_version"], "machine": r["computer"], "commit": r["package_commit"],
        "iterations": int(r["iterations_requested"] or r["iterations"]),
    })

algos = []
algo_index = {}
for r in rows:
    a = r["algorithm_id"]
    if a not in algo_index:
        algo_index[a] = len(algos)
        algos.append({"id": a, "pass": int(r["pass"] or 0), "desc": r["description"],
                      "tags": [t for t in r["tags"].split(";") if t], "flush": r["flush_mode"],
                      "dispatch": r["dispatch_info"]})
# Order algorithms by pass, then name.
algos.sort(key=lambda a: (a["pass"], a["id"]))
algo_index = {a["id"]: i for i, a in enumerate(algos)}

WORKLOADS = ["mostly_empty", "mostly_small", "mostly_medium", "mostly_mid", "realistic_mix",
             "mostly_large", "worst_case", "edges", "sparse_keys"]
sweep_sizes = sorted({int(r["sweep_size"]) for r in rows if r["sweep_size"]})
cols = WORKLOADS + [f"sweep:{s}" for s in sweep_sizes]
col_index = {c: i for i, c in enumerate(cols)}

def col_of(r):
    return f"sweep:{r['sweep_size']}" if r["sweep_size"] else r["workload"]

# Quartiles from the per-iteration samples (keyed like the results rows, incl. sorts_per_iteration:
# 20 for samples without the column, schema 1-4).
key_of_row = {}
for r in rows:
    key_of_row[(r["run_id"], r["gpu_index"], r["wave_size"], r["algorithm"], r["flush_mode"],
                r["workload"], r["sweep_size"], r.get("sorts_per_iteration") or "20")] = r
samples = defaultdict(list)
samples_path = next((p for p in (f"{AGG}/all_samples.csv", f"{AGG}/all_samples.csv.gz")
                     if os.path.exists(p)), None)
if samples_path is None:
    print(f"no all_samples.csv(.gz) in {AGG}: quartiles left out", file=sys.stderr)
else:
    opener = gzip.open if samples_path.endswith(".gz") else open
    with opener(samples_path, "rt", encoding="utf-8", newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        ix = {h: i for i, h in enumerate(hdr)}
        si = ix.get("sorts_per_iteration")
        for s in rd:
            k = (s[ix["run_id"]], s[ix["gpu_index"]], s[ix["wave_size"]], s[ix["algorithm"]],
                 s[ix["flush_mode"]], s[ix["workload"]], s[ix["sweep_size"]],
                 s[si] if si is not None else "20")
            if k in key_of_row:
                samples[k].append(float(s[ix["time_us"]]))

def q(v, p):
    if not v:
        return None
    v = sorted(v)
    i = (len(v) - 1) * p
    lo = int(i)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)

# values[cfg][algo][col] = [min, p25, median, mean, p75, p95, p99, max]
values = [[[None] * len(cols) for _ in algos] for _ in cfgs]
missing_q = 0
for k, r in key_of_row.items():
    s = samples.get(k, [])
    if not s:
        missing_q += 1
    rec = [float(r["min_us"]), q(s, 0.25), float(r["median_us"]), float(r["mean_us"]),
           q(s, 0.75), float(r["p95_us"]), float(r["p99_us"]), float(r["max_us"])]
    rec = [None if x is None else round(x, 2) for x in rec]
    values[cfg_index[cfg_key(r)]][algo_index[r["algorithm_id"]]][col_index[col_of(r)]] = rec

data = {"cfgs": cfg_meta, "algos": algos, "cols": cols, "workloads": WORKLOADS,
        "sweep": sweep_sizes, "values": values,
        "fails": sum(int(r["verify_failures"] or 0) for r in rows)}
js = "window.SORT_DATA=" + json.dumps(data, separators=(",", ":")) + ";"
open(OUT, "w", encoding="utf-8").write(js)
print("cfgs", [c["label"] for c in cfg_meta], "algos", len(algos), "cols", len(cols),
      "missing quartiles", missing_q, "bytes", len(js))
