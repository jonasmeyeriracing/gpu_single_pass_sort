#pragma once

#include "Results.h"

#include <filesystem>
#include <string>

// Machine-readable results next to the human-readable results .txt (columns: tools/CSV_FORMAT.md).
// All files: UTF-8 (no BOM), a header row, CRLF line ends, RFC 4180 quoting, '.' as the decimal
// separator whatever the locale.
//
//   <stem>.csv             one row per GPU x algorithm x workload (x sweep size for the sweep
//                          workload) with the summary statistics; schema_version first
//   <stem>_samples.csv     one row per measured iteration (--no-samples: not written)
//   <stem>_wave_probe.csv  one row per GPU x probed wave configuration
// A --wave-probe run writes only the wave probe CSV, as <stem>.csv.

// Bump when a column is renamed, removed or changes meaning (adding columns at the end is fine;
// tools/aggregate_results.py checks it). 2: the results CSV has the columns stable_power and
// drain_spin_iters_per_us, and flush_mode can carry a drain suffix (full_d20, ...). 3: the results
// CSV has the column dispatch_info (threads per group / groupshared bytes per dispatch). 4 (the
// final set): the results CSV has the columns pass, description and tags (algorithms.txt metadata),
// iterations_requested is per GPU (--iterations-integrated), and the default flush_mode is full_d50.
constexpr int kCsvSchemaVersion = 4;

// Git commit of this build ("<short sha>", "<short sha>-dirty" or "unknown").
const char* PackageCommit();

// Each returns false and sets 'error' if the file cannot be written.
bool WriteResultsCsv(const RunInfo& info, const std::filesystem::path& path, std::string& error);
bool WriteSamplesCsv(const RunInfo& info, const std::filesystem::path& path, std::string& error);
bool WriteWaveProbeCsv(const RunInfo& info, const std::filesystem::path& path, std::string& error);
