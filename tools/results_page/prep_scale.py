"""Build scale.js (sorts-per-batch scaling data) for the results page from one or more scale.csv files."""
import csv, json, sys

OUT = sys.argv[1]
files = sys.argv[2:]
rows = []
for f in files:
    rows += [r for r in csv.DictReader(open(f, encoding="utf-8"))
             if r["run_kind"] == "benchmark" and not r["sweep_size"]]

ORDER = [("NVIDIA GeForce RTX 5080", "32"), ("AMD Radeon RX 7900 XTX", "32"),
         ("AMD Radeon RX 7900 XTX", "64"), ("NVIDIA GeForce RTX 2060", "32"),
         ("AMD Radeon(TM) Graphics", "32")]
SHORT = {"NVIDIA GeForce RTX 5080": "RTX 5080", "AMD Radeon RX 7900 XTX": "RX 7900 XTX",
         "NVIDIA GeForce RTX 2060": "RTX 2060", "AMD Radeon(TM) Graphics": "Radeon iGPU"}
present = {(r["gpu_name"], r["wave_size"]) for r in rows}
cfgs = [k for k in ORDER if k in present] + sorted(present - set(ORDER))
labels = []
for k in cfgs:
    lab = SHORT.get(k[0], k[0])
    if k[0] == "AMD Radeon RX 7900 XTX":
        lab += f" · wave{k[1]}"
    labels.append(lab)

algos = sorted({r["algorithm_id"] for r in rows}, key=lambda a: (int(next(x["pass"] for x in rows if x["algorithm_id"] == a) or 0), a))
counts = sorted({int(r["sorts_per_iteration"]) for r in rows})
WORKLOADS = ["mostly_empty", "mostly_small", "mostly_medium", "mostly_mid", "realistic_mix",
             "mostly_large", "worst_case", "edges", "sparse_keys"]
wls = [w for w in WORKLOADS if any(r["workload"] == w for r in rows)]

# values[cfg][algo][workload][count] = [median, p95, iterations]
values = [[[[None] * len(counts) for _ in wls] for _ in algos] for _ in cfgs]
ci_of = {k: i for i, k in enumerate(cfgs)}
ai_of = {a: i for i, a in enumerate(algos)}
wi_of = {w: i for i, w in enumerate(wls)}
ni_of = {n: i for i, n in enumerate(counts)}
fails = 0
for r in rows:
    fails += int(r["verify_failures"] or 0)
    values[ci_of[(r["gpu_name"], r["wave_size"])]][ai_of[r["algorithm_id"]]][wi_of[r["workload"]]][ni_of[int(r["sorts_per_iteration"])]] = \
        [round(float(r["median_us"]), 2), round(float(r["p95_us"]), 2), int(r["iterations"])]

data = {"cfgs": labels, "algos": algos, "counts": counts, "workloads": wls, "values": values, "fails": fails,
        "commits": sorted({r["package_commit"] for r in rows})}
js = "window.SCALE_DATA=" + json.dumps(data, separators=(",", ":")) + ";"
open(OUT, "w", encoding="utf-8").write(js)
print(labels, len(algos), counts, wls, "fails", fails, "bytes", len(js))
