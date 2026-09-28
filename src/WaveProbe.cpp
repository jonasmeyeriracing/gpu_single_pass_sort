#include "WaveProbe.h"

#include "Verify.h"
#include "Workloads.h"

#include <algorithm>
#include <map>

namespace
{
// Output words per thread (must match kWaveProbeSource).
enum ProbeWord : uint32_t
{
    kWordLaneCount = 0,
    kWordLaneIndex,
    kWordGroupIndex,
    kWordFirstLane, // WaveReadLaneFirst(SV_GroupIndex): identifies the wave
    kWordXor32,     // WaveReadLaneAt(SV_GroupIndex, lane ^ 32), only if lane count >= 64
    kWordXor1,      // WaveReadLaneAt(SV_GroupIndex, lane ^ 1)
    kWordPlus16,    // WaveReadLaneAt(SV_GroupIndex, (lane + 16) % count)
    kWordLast,      // WaveReadLaneAt(SV_GroupIndex, count - 1): same source for every lane
    kWordPrefixSum, // WavePrefixSum(1)
    kWordActiveSum, // WaveActiveSum(1)
    kWordCountBits, // WaveActiveCountBits(true)
    kWordBallot,    // countbits of WaveActiveBallot(true)
    kProbeWords
};

const char* const kWaveProbeSource = R"(
cbuffer ProbeConstants : register(b0)
{
    uint gBase; // first output word of this dispatch
    uint gPad0;
    uint gPad1;
    uint gPad2;
};
RWStructuredBuffer<uint> gOut : register(u0);

// Same attribute handling as the sort shaders (common.hlsli: WAVE_SIZE_ATTR).
#if defined(WAVE_SIZE_REQUIRED) && WAVE_SIZE_REQUIRED
#define PROBE_WAVE_SIZE_ATTR [WaveSize(WAVE_SIZE)]
#else
#define PROBE_WAVE_SIZE_ATTR
#endif

[numthreads(GROUP_SIZE, 1, 1)]
PROBE_WAVE_SIZE_ATTR
void main(uint gi : SV_GroupIndex)
{
    const uint count = WaveGetLaneCount();
    const uint lane = WaveGetLaneIndex();
    uint xor32 = 0xFFFFFFFFu;
    if (count >= 64) // wave-uniform
        xor32 = WaveReadLaneAt(gi, lane ^ 32u);
    const uint xor1 = WaveReadLaneAt(gi, lane ^ 1u);
    const uint plus16 = WaveReadLaneAt(gi, (lane + 16u) % count);
    const uint last = WaveReadLaneAt(gi, count - 1u);
    const uint4 ballot = WaveActiveBallot(true);
    const uint o = gBase + gi * 12u;
    gOut[o + 0] = count;
    gOut[o + 1] = lane;
    gOut[o + 2] = gi;
    gOut[o + 3] = WaveReadLaneFirst(gi);
    gOut[o + 4] = xor32;
    gOut[o + 5] = xor1;
    gOut[o + 6] = plus16;
    gOut[o + 7] = last;
    gOut[o + 8] = WavePrefixSum(1u);
    gOut[o + 9] = WaveActiveSum(1u);
    gOut[o + 10] = WaveActiveCountBits(true);
    gOut[o + 11] = countbits(ballot.x) + countbits(ballot.y) + countbits(ballot.z) + countbits(ballot.w);
}
)";
static_assert(kProbeWords == 12, "kWaveProbeSource writes 12 words per thread");

enum Test
{
    kTestWritten,
    kTestMapping,
    kTestWaves,
    kTestXor32,
    kTestXor1,
    kTestPlus16,
    kTestLast,
    kTestPrefix,
    kTestSum,
    kTestCountBits,
    kTestBallot,
    kTestCount
};
const char* const kTestNames[kTestCount] = {"all threads wrote", "lane mapping", "waves",        "readlane^32",
                                            "readlane^1",        "readlane+16",  "readlane(last)", "prefixsum",
                                            "activesum",         "countbits",    "ballot"};

struct TestResult
{
    uint64_t checked = 0;
    uint64_t wrong = 0;
    std::string first; // first mismatch
};

struct VariantResult
{
    uint32_t attributeSize = 0; // [WaveSize(attributeSize)]; 0 = without [WaveSize]
    std::vector<uint32_t> groupSizes;                // that ran
    std::map<uint32_t, std::vector<uint32_t>> lanes; // observed lane count -> group sizes
    TestResult tests[kTestCount];
    std::vector<std::string> errors; // PSO failures
};

void Check(TestResult& t, bool ok, const std::string& detail)
{
    ++t.checked;
    if (ok)
        return;
    if (t.wrong++ == 0)
        t.first = detail;
}

// Checks the output of one probe dispatch (groupSize threads x kProbeWords words).
void Analyze(const std::vector<uint32_t>& words, uint32_t groupSize, VariantResult& r)
{
    auto w = [&](uint32_t t, uint32_t k) { return words[size_t(t) * kProbeWords + k]; };
    std::vector<bool> written(groupSize);
    std::map<uint32_t, std::vector<uint32_t>> waves; // first lane's SV_GroupIndex -> threads
    std::map<uint32_t, bool> countsSeen;
    for (uint32_t t = 0; t < groupSize; ++t)
    {
        written[t] = w(t, kWordGroupIndex) == t && w(t, kWordLaneCount) != kPoisonValue;
        Check(r.tests[kTestWritten], written[t],
              Format("group %u, thread %u: output word SV_GroupIndex = 0x%08X, lane count = 0x%08X", groupSize, t,
                     w(t, kWordGroupIndex), w(t, kWordLaneCount)));
        if (!written[t])
            continue;
        countsSeen[w(t, kWordLaneCount)] = true;
        waves[w(t, kWordFirstLane)].push_back(t);
    }
    for (const auto& [count, unused] : countsSeen)
    {
        (void)unused;
        r.lanes[count].push_back(groupSize);
    }

    for (const auto& [first, threads] : waves)
    {
        // A consistent wave: every member reports the same lane count, and distinct lanes < count.
        const uint32_t count = w(threads.front(), kWordLaneCount);
        std::vector<uint32_t> laneToThread(std::max(count, 1u), UINT32_MAX);
        bool consistent = count > 0 && count <= 128 && threads.size() <= count;
        std::string why;
        for (uint32_t t : threads)
        {
            const uint32_t lane = w(t, kWordLaneIndex);
            if (!consistent)
                break;
            if (w(t, kWordLaneCount) != count)
            {
                consistent = false;
                why = Format("thread %u reports %u lanes, thread %u reports %u", t, w(t, kWordLaneCount),
                             threads.front(), count);
            }
            else if (lane >= count || laneToThread[lane] != UINT32_MAX)
            {
                consistent = false;
                why = Format("thread %u reports lane %u (lane count %u)%s", t, lane, count,
                             lane < count ? Format(", already used by thread %u", laneToThread[lane]).c_str() : "");
            }
            else
            {
                laneToThread[lane] = t;
            }
        }
        if (!consistent && why.empty())
            why = Format("%zu threads, lane count %u", threads.size(), count);
        for (size_t i = 0; i < threads.size(); ++i)
            Check(r.tests[kTestWaves], consistent,
                  Format("group %u, wave whose first lane is thread %u (%zu threads): %s", groupSize, first,
                         threads.size(), why.c_str()));

        // Rank of each active lane among the active lanes (WavePrefixSum(1)).
        std::vector<uint32_t> rank(laneToThread.size(), 0);
        uint32_t active = 0;
        for (size_t lane = 0; lane < laneToThread.size(); ++lane)
        {
            rank[lane] = active;
            if (laneToThread[lane] != UINT32_MAX)
                ++active;
        }

        for (uint32_t t : threads)
        {
            const uint32_t lane = w(t, kWordLaneIndex);
            const uint32_t tc = w(t, kWordLaneCount);
            const std::string where = Format("group %u, thread %u (lane %u of %u, wave's first lane is thread %u)",
                                             groupSize, t, lane, tc, first);
            const bool mapOk = tc > 0 && lane == t % tc && first == t - lane;
            Check(r.tests[kTestMapping], mapOk,
                  Format("%s: expected lane %u and first lane thread %u (SV_GroupIndex %% lane count)", where.c_str(),
                         tc ? t % tc : 0, tc ? t - t % tc : 0));
            if (!consistent)
                continue;
            // x = SV_GroupIndex of the source lane; source lanes that are not active are not checked.
            auto readLane = [&](Test test, uint32_t word, uint32_t srcLane) {
                if (srcLane >= count || laneToThread[srcLane] == UINT32_MAX)
                    return;
                const uint32_t expected = laneToThread[srcLane];
                Check(r.tests[test], w(t, word) == expected,
                      Format("%s: got x=%u, expected x=%u (x = SV_GroupIndex of source lane %u)", where.c_str(),
                             w(t, word), expected, srcLane));
            };
            if (count >= 64)
                readLane(kTestXor32, kWordXor32, lane ^ 32u);
            readLane(kTestXor1, kWordXor1, lane ^ 1u);
            readLane(kTestPlus16, kWordPlus16, (lane + 16u) % count);
            readLane(kTestLast, kWordLast, count - 1u);
            auto value = [&](Test test, uint32_t word, uint32_t expected) {
                Check(r.tests[test], w(t, word) == expected,
                      Format("%s: got %u, expected %u", where.c_str(), w(t, word), expected));
            };
            value(kTestPrefix, kWordPrefixSum, rank[lane]);
            value(kTestSum, kWordActiveSum, active);
            value(kTestCountBits, kWordCountBits, active);
            value(kTestBallot, kWordBallot, active);
        }
    }
}

std::string JoinSizes(const std::vector<uint32_t>& v)
{
    std::string s;
    for (uint32_t x : v)
        s += Format("%s%u", s.empty() ? "" : "/", x);
    return s;
}

std::string LanesText(const VariantResult& r)
{
    if (r.lanes.empty())
        return "none";
    if (r.lanes.size() == 1)
        return Format("%u", r.lanes.begin()->first);
    std::string s;
    for (const auto& [count, groups] : r.lanes)
        s += Format("%s%u (group%s %s)", s.empty() ? "" : ", ", count, groups.size() == 1 ? "" : "s",
                    JoinSizes(groups).c_str());
    return s;
}

std::string VariantName(uint32_t attributeSize)
{
    return attributeSize ? Format("compiled WAVE_SIZE=%u [WaveSize(%u)]", attributeSize, attributeSize)
                         : std::string("compiled without [WaveSize] (driver's choice)");
}

std::string ShortName(uint32_t attributeSize)
{
    return attributeSize ? Format("[WaveSize(%u)]", attributeSize) : std::string("without [WaveSize]");
}

// Adds the variants for one attribute size (0 = without [WaveSize]) and every group size (with the
// attribute: only group sizes >= attributeSize). defineWaveSize: -D WAVE_SIZE.
void AddVariants(ShaderCompiler& compiler, WaveProbeSet& set, uint32_t defineWaveSize, uint32_t attributeSize,
                 const std::vector<uint32_t>& groupSizes)
{
    for (uint32_t gs : groupSizes)
    {
        if (attributeSize && gs < attributeSize)
            continue;
        ShaderDefines defines = {{"GROUP_SIZE", std::to_string(gs)}, {"WAVE_SIZE", std::to_string(defineWaveSize)}};
        if (attributeSize)
            defines.emplace_back("WAVE_SIZE_REQUIRED", "1");
        std::string log;
        ComPtr<IDxcBlob> blob = compiler.CompileSource(kWaveProbeSource, L"wave_probe.hlsl", "main", defines, log);
        if (!blob)
            throw std::runtime_error(Format("wave probe shader failed to compile (group size %u%s):\n%s", gs,
                                            attributeSize ? Format(", [WaveSize(%u)]", attributeSize).c_str() : "",
                                            log.c_str()));
        set.variants.push_back({attributeSize, gs, blob});
    }
}

// Problems of one variant's results. expectedLanes != 0: every group must run with exactly that
// many lanes; 0: any lane count in laneMin..laneMax (several are allowed, e.g. per group size).
std::vector<std::string> Problems(const VariantResult* r, uint32_t expectedLanes, uint32_t laneMin, uint32_t laneMax)
{
    std::vector<std::string> problems;
    if (!r || r->groupSizes.empty())
    {
        problems.push_back("the probe could not run in this configuration");
        return problems;
    }
    if (!r->errors.empty())
        problems.push_back("the probe failed for some group sizes");
    if (expectedLanes)
    {
        if (r->lanes.size() != 1 || r->lanes.begin()->first != expectedLanes)
            problems.push_back(Format("observed lanes %s, not %u", LanesText(*r).c_str(), expectedLanes));
    }
    else
    {
        for (const auto& [count, groups] : r->lanes)
        {
            (void)groups;
            if (count < laneMin || count > laneMax)
            {
                problems.push_back(Format("observed lanes %s, outside the device's wave lane range %u-%u",
                                          LanesText(*r).c_str(), laneMin, laneMax));
                break;
            }
        }
    }
    if (r->tests[kTestWritten].wrong)
        problems.push_back("some threads wrote no output");
    if (r->tests[kTestMapping].wrong || r->tests[kTestWaves].wrong)
        problems.push_back(expectedLanes ? Format("lane mapping broken (lane != SV_GroupIndex %% %u)", expectedLanes)
                                         : std::string("lane mapping broken (lane != SV_GroupIndex % lane count)"));
    return problems;
}

std::string CrossLaneMismatches(const VariantResult& r)
{
    std::string crossLane;
    for (int t = kTestXor32; t < kTestCount; ++t)
    {
        if (r.tests[t].wrong)
            crossLane += Format("%s%s", crossLane.empty() ? "" : ", ", kTestNames[t]);
    }
    return crossLane;
}

std::string Join(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& x : v)
        s += (s.empty() ? "" : "; ") + x;
    return s;
}
} // namespace

WaveProbeSet CompileWaveProbe(ShaderCompiler& compiler, uint32_t waveSize, bool attribute,
                              const std::vector<uint32_t>& groupSizes)
{
    WaveProbeSet set;
    set.waveSize = waveSize;
    set.attribute = attribute;
    if (attribute)
        AddVariants(compiler, set, waveSize, waveSize, groupSizes);
    AddVariants(compiler, set, waveSize, 0, groupSizes);
    return set;
}

WaveProbeSet CompileWaveProbeSurvey(ShaderCompiler& compiler, uint32_t laneMin, uint32_t laneMax, uint32_t waveSize,
                                    bool attribute, const std::vector<uint32_t>& groupSizes)
{
    WaveProbeSet set;
    set.waveSize = waveSize;
    set.attribute = attribute;
    set.survey = true;
    set.laneMin = laneMin;
    set.laneMax = laneMax;
    AddVariants(compiler, set, waveSize, 0, groupSizes);
    for (uint32_t n = 4; n <= 128; n *= 2) // [WaveSize] accepts powers of two 4..128
    {
        if (n >= laneMin && n <= laneMax)
            AddVariants(compiler, set, n, n, groupSizes);
    }
    return set;
}

std::string DescribeWaveProbe(const WaveProbeSet& set)
{
    std::vector<uint32_t> sizes; // attribute sizes in variant order
    std::vector<uint32_t> groups;
    for (const auto& v : set.variants)
    {
        if (std::find(sizes.begin(), sizes.end(), v.attributeSize) == sizes.end())
            sizes.push_back(v.attributeSize);
        if (std::find(groups.begin(), groups.end(), v.groupSize) == groups.end())
            groups.push_back(v.groupSize);
    }
    std::sort(groups.begin(), groups.end());
    std::string s;
    for (uint32_t n : sizes)
        s += (s.empty() ? "" : ", ") + ShortName(n);
    return s + "; groups " + JoinSizes(groups);
}

WaveProbeReport RunWaveProbe(GpuBenchmark& bench, const WaveProbeSet& set)
{
    uint32_t maxGroup = 0;
    for (const auto& v : set.variants)
        maxGroup = std::max(maxGroup, v.groupSize);
    const uint32_t wordsPerDispatch = maxGroup * kProbeWords;
    // As many dispatches per submission as the output buffer holds.
    const size_t perBatch = std::max<size_t>(1, kMaxElementsPerIteration / std::max(wordsPerDispatch, 1u));
    std::vector<GpuBenchmark::ProbeOutput> outputs;
    for (size_t first = 0; first < set.variants.size(); first += perBatch)
    {
        std::vector<IDxcBlob*> shaders;
        for (size_t i = first; i < std::min(set.variants.size(), first + perBatch); ++i)
            shaders.push_back(set.variants[i].shader.Get());
        for (auto& o : bench.RunProbe(shaders, wordsPerDispatch))
            outputs.push_back(std::move(o));
    }

    // One result per attribute size, in variant order.
    std::vector<VariantResult> results;
    for (size_t i = 0; i < set.variants.size(); ++i)
    {
        const WaveProbeVariant& v = set.variants[i];
        auto it = std::find_if(results.begin(), results.end(),
                               [&](const VariantResult& r) { return r.attributeSize == v.attributeSize; });
        if (it == results.end())
        {
            results.emplace_back();
            results.back().attributeSize = v.attributeSize;
            it = results.end() - 1;
        }
        VariantResult& r = *it;
        if (!outputs[i].error.empty())
        {
            r.errors.push_back(Format("group %u: %s", v.groupSize, outputs[i].error.c_str()));
            continue;
        }
        r.groupSizes.push_back(v.groupSize);
        Analyze(outputs[i].words, v.groupSize, r);
    }

    WaveProbeReport report;
    const uint32_t configSize = set.attribute ? set.waveSize : 0;
    const VariantResult* config = nullptr;
    for (const auto& r : results)
    {
        if (r.attributeSize == configSize)
            config = &r;
        std::string line = Format("Wave probe: %s, groups %s: ", VariantName(r.attributeSize).c_str(),
                                  JoinSizes(r.groupSizes).c_str());
        if (r.groupSizes.empty())
            line += "did not run";
        else
        {
            line += "observed lanes " + LanesText(r);
            for (int t = 0; t < kTestCount; ++t)
            {
                const TestResult& tr = r.tests[t];
                if (t == kTestWritten && tr.wrong == 0)
                    continue; // only shown when it fails
                if (tr.checked == 0)
                    line += Format(", %s n/a", kTestNames[t]);
                else if (tr.wrong == 0)
                    line += Format(", %s OK", kTestNames[t]);
                else
                    line += Format(", %s FAIL (%llu of %llu)", kTestNames[t], static_cast<unsigned long long>(tr.wrong),
                                   static_cast<unsigned long long>(tr.checked));
            }
        }
        report.lines.push_back(line);
        for (const auto& e : r.errors)
            report.lines.push_back("    Wave probe error: " + e);
        for (int t = 0; t < kTestCount; ++t)
        {
            if (r.tests[t].wrong)
                report.lines.push_back(
                    Format("    Wave probe mismatch, %s: first at %s", kTestNames[t], r.tests[t].first.c_str()));
        }
    }

    if (set.survey)
    {
        // One verdict per variant.
        for (const auto& r : results)
        {
            const std::string name = ShortName(r.attributeSize);
            const std::vector<std::string> problems = Problems(&r, r.attributeSize, set.laneMin, set.laneMax);
            const std::string configNote =
                r.attributeSize == configSize
                    ? Format(" (the sort shaders' configuration: WAVE_SIZE=%u%s)", set.waveSize,
                             set.attribute ? Format(" + [WaveSize(%u)]", set.waveSize).c_str() : "")
                    : std::string();
            if (problems.empty())
            {
                const std::string lanes = LanesText(r);
                if (r.attributeSize)
                    report.lines.push_back(Format("Wave probe verdict, %s: OK, runs with %u lanes and lane = "
                                                  "SV_GroupIndex %% %u%s",
                                                  name.c_str(), r.attributeSize, r.attributeSize, configNote.c_str()));
                else
                    report.lines.push_back(Format("Wave probe verdict, %s: OK, the driver chose lanes %s and lane = "
                                                  "SV_GroupIndex %% lane count%s",
                                                  name.c_str(), lanes.c_str(), configNote.c_str()));
                const std::string crossLane = CrossLaneMismatches(r);
                if (!crossLane.empty())
                    report.lines.push_back(
                        Format("Wave probe note, %s: cross-lane mismatches: %s", name.c_str(), crossLane.c_str()));
                report.summary.push_back(Format("%s: OK, lanes %s%s%s", name.c_str(), lanes.c_str(),
                                                r.attributeSize ? "" : " (driver's choice)",
                                                crossLane.empty() ? "" : ", cross-lane mismatches"));
            }
            else
            {
                const std::string problem = Join(problems);
                report.configOk = false;
                report.problem += (report.problem.empty() ? "" : "; ") + name + ": " + problem;
                report.lines.push_back(
                    Format("WAVE PROBE WARNING, %s: %s%s", name.c_str(), problem.c_str(), configNote.c_str()));
                report.summary.push_back(Format("%s: WARNING, %s", name.c_str(), problem.c_str()));
            }
        }
        return report;
    }

    // Verdict for the configuration the sort shaders use.
    const std::string configName =
        Format("WAVE_SIZE=%u%s", set.waveSize, set.attribute ? Format(" + [WaveSize(%u)]", set.waveSize).c_str() : "");
    const std::vector<std::string> problems = Problems(config, set.waveSize, set.laneMin, set.laneMax);
    if (problems.empty())
    {
        report.lines.push_back(Format("Wave probe verdict: OK, the sort shaders' configuration (%s) runs with %u "
                                      "lanes and lane = SV_GroupIndex %% %u",
                                      configName.c_str(), set.waveSize, set.waveSize));
        const std::string crossLane = CrossLaneMismatches(*config);
        if (!crossLane.empty())
            report.lines.push_back(Format("Wave probe note: cross-lane mismatches in this configuration: %s",
                                          crossLane.c_str()));
        report.summary.push_back(Format("%s: OK, lanes %u%s", configName.c_str(), set.waveSize,
                                        crossLane.empty() ? "" : ", cross-lane mismatches"));
    }
    else
    {
        report.configOk = false;
        report.problem = Join(problems);
        report.lines.push_back(Format("WAVE PROBE WARNING: the sort shaders are compiled for %s, but %s",
                                      configName.c_str(), report.problem.c_str()));
        report.summary.push_back(Format("%s: WARNING, %s", configName.c_str(), report.problem.c_str()));
    }
    return report;
}
