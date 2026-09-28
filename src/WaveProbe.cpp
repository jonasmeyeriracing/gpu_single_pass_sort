#include "WaveProbe.h"

#include "Verify.h"

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
    bool attribute = false;
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

std::string VariantName(const WaveProbeSet& set, bool attribute)
{
    return attribute ? Format("compiled WAVE_SIZE=%u [WaveSize(%u)]", set.waveSize, set.waveSize)
                     : std::string("compiled without [WaveSize] (driver's choice)");
}
} // namespace

WaveProbeSet CompileWaveProbe(ShaderCompiler& compiler, uint32_t waveSize, bool attribute,
                              const std::vector<uint32_t>& groupSizes)
{
    WaveProbeSet set;
    set.waveSize = waveSize;
    set.attribute = attribute;
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool withAttr = pass == 0;
        if (withAttr && !attribute)
            continue;
        for (uint32_t gs : groupSizes)
        {
            if (withAttr && gs < waveSize)
                continue;
            ShaderDefines defines = {{"GROUP_SIZE", std::to_string(gs)}, {"WAVE_SIZE", std::to_string(waveSize)}};
            if (withAttr)
                defines.emplace_back("WAVE_SIZE_REQUIRED", "1");
            std::string log;
            ComPtr<IDxcBlob> blob = compiler.CompileSource(kWaveProbeSource, L"wave_probe.hlsl", "main", defines, log);
            if (!blob)
                throw std::runtime_error(Format("wave probe shader failed to compile (group size %u%s):\n%s", gs,
                                                withAttr ? ", [WaveSize]" : "", log.c_str()));
            set.variants.push_back({withAttr, gs, blob});
        }
    }
    return set;
}

WaveProbeReport RunWaveProbe(GpuBenchmark& bench, const WaveProbeSet& set)
{
    uint32_t maxGroup = 0;
    std::vector<IDxcBlob*> shaders;
    for (const auto& v : set.variants)
    {
        maxGroup = std::max(maxGroup, v.groupSize);
        shaders.push_back(v.shader.Get());
    }
    const uint32_t wordsPerDispatch = maxGroup * kProbeWords;
    const std::vector<GpuBenchmark::ProbeOutput> outputs = bench.RunProbe(shaders, wordsPerDispatch);

    std::vector<VariantResult> results;
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool withAttr = pass == 0;
        VariantResult r;
        r.attribute = withAttr;
        bool any = false;
        for (size_t i = 0; i < set.variants.size(); ++i)
        {
            const WaveProbeVariant& v = set.variants[i];
            if (v.attribute != withAttr)
                continue;
            any = true;
            if (!outputs[i].error.empty())
            {
                r.errors.push_back(Format("group %u: %s", v.groupSize, outputs[i].error.c_str()));
                continue;
            }
            r.groupSizes.push_back(v.groupSize);
            Analyze(outputs[i].words, v.groupSize, r);
        }
        if (any)
            results.push_back(std::move(r));
    }

    WaveProbeReport report;
    const VariantResult* config = nullptr;
    for (const auto& r : results)
    {
        if (r.attribute == set.attribute)
            config = &r;
        std::string line = Format("Wave probe: %s, groups %s: ", VariantName(set, r.attribute).c_str(),
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

    // Verdict for the configuration the sort shaders use.
    const std::string configName =
        Format("WAVE_SIZE=%u%s", set.waveSize, set.attribute ? Format(" + [WaveSize(%u)]", set.waveSize).c_str() : "");
    std::vector<std::string> problems;
    if (!config || config->groupSizes.empty())
        problems.push_back("the probe could not run in this configuration");
    else
    {
        if (!config->errors.empty())
            problems.push_back("the probe failed for some group sizes");
        if (config->lanes.size() != 1 || config->lanes.begin()->first != set.waveSize)
            problems.push_back(Format("observed lanes %s, not %u", LanesText(*config).c_str(), set.waveSize));
        if (config->tests[kTestWritten].wrong)
            problems.push_back("some threads wrote no output");
        if (config->tests[kTestMapping].wrong || config->tests[kTestWaves].wrong)
            problems.push_back(Format("lane mapping broken (lane != SV_GroupIndex %% %u)", set.waveSize));
    }
    if (problems.empty())
    {
        report.lines.push_back(Format("Wave probe verdict: OK, the sort shaders' configuration (%s) runs with %u "
                                      "lanes and lane = SV_GroupIndex %% %u",
                                      configName.c_str(), set.waveSize, set.waveSize));
        std::string crossLane;
        for (int t = kTestXor32; t < kTestCount; ++t)
        {
            if (config->tests[t].wrong)
                crossLane += Format("%s%s", crossLane.empty() ? "" : ", ", kTestNames[t]);
        }
        if (!crossLane.empty())
            report.lines.push_back(Format("Wave probe note: cross-lane mismatches in this configuration: %s",
                                          crossLane.c_str()));
    }
    else
    {
        report.configOk = false;
        for (const auto& p : problems)
            report.problem += (report.problem.empty() ? "" : "; ") + p;
        report.lines.push_back(Format("WAVE PROBE WARNING: the sort shaders are compiled for %s, but %s",
                                      configName.c_str(), report.problem.c_str()));
    }
    return report;
}
