# Results page

Interactive page of the final benchmark results (_test/final/notes.md) and of the scale run
(_test/scale/notes.md, 20 to 512 sorts per batch). Published at
https://claude.ai/artifact/3PHgWLiq9ej94L2pM3wWZZ.

| file | content |
|---|---|
| index.html | the page source as published (an HTML fragment: the artifact host adds the document skeleton; only external resource: Google Fonts, with fallbacks); loads `data.js` and `scale.js` from the same folder |
| data.js | generated data (`window.SORT_DATA = {...}`), built from _test/final; committed so the page can be opened locally |
| scale.js | generated data for the section "Scaling with the number of sorts" (`window.SCALE_DATA = {...}`), built from _test/scale; without it the section shows a "no scaling run" note |
| prep.py | builds data.js from an aggregation folder |
| prep_scale.py | builds scale.js from one or more results CSV files of scale runs |

Open `index.html` in a browser to view the data locally.

## Regenerate

```
python tools/aggregate_results.py <zips/folders> -o <dir>
python tools/results_page/prep.py <dir> tools/results_page/data.js
```

The first step merges the run results (results folders or the zips that run_all.bat writes) into
`<dir>/all_results.csv` and `<dir>/all_samples.csv` (see tools/aggregate_results.py). prep.py
reads `all_results.csv` (benchmark rows only) and, for the 25th / 75th percentiles,
`all_samples.csv` or `all_samples.csv.gz`. Without a samples file the quartiles are null and the
page shows "–" for them. For the archived final data:

```
python tools/results_page/prep.py _test/final tools/results_page/data.js
```

The GPU display order and short names are set in prep.py (`ORDER`, `SHORT`); GPUs not listed
there are appended in name order.

### scale.js

```
python tools/results_page/prep_scale.py <out.js> <results.csv> [<results.csv> ...]
```

prep_scale.py reads results CSV files (the aggregated `all_results.csv` or the per-run
`scale.csv` files of `run_all.bat scale`; same columns), keeps the benchmark rows of the size
distributions (no sweep rows, no smoke runs) and writes median, p95 and iteration count per GPU x
algorithm x workload x sorts per batch. For the archived scale run:

```
python tools/results_page/prep_scale.py tools/results_page/scale.js _test/scale/all_results.csv
```

To add GPUs from a new scale run, aggregate all scale runs into one folder (or pass every
results CSV) and rerun it. The scaling chart matches its GPU to the page's GPU selector by the
label, so `ORDER` / `SHORT` in prep_scale.py must produce the same labels as prep.py (the RX 7900
XTX gets the ` · wave<N>` suffix in both).
