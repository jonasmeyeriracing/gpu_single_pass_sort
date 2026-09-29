# Results page

Interactive page of the final benchmark results (_test/final/notes.md). Published at
https://claude.ai/artifact/3PHgWLiq9ej94L2pM3wWZZ.

| file | content |
|---|---|
| index.html | the page source as published (an HTML fragment: the artifact host adds the document skeleton; only external resource: Google Fonts, with fallbacks); loads `data.js` from the same folder |
| data.js | generated data (`window.SORT_DATA = {...}`), built from _test/final; committed so the page can be opened locally |
| prep.py | builds data.js from an aggregation folder |

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
