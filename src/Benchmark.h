#pragma once

#include "Algorithms.h"
#include "Common.h"
#include "Workloads.h"

#include <d3d12.h>
#include <dxcapi.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct CompiledAlgorithm
{
    std::string name;
    std::vector<ComPtr<IDxcBlob>> shaders; // one per dispatch, in order
    FlushMode flush = kDefaultFlushMode;   // see FlushMode (Algorithms.h)
    bool usesWaveOps = false;              // some dispatch uses wave intrinsics (depends on the wave size)
    std::string dispatchInfo;              // threads / groupshared bytes per dispatch (FormatDispatchStats)
};

struct ComboResult
{
    std::vector<double> timesUs;        // measured iterations only (warmup excluded)
    uint32_t iterationsRun = 0;         // including warmup
    uint32_t failures = 0;              // verification failures (including warmup)
    std::vector<uint32_t> failedIterations; // their iteration indices as generated (measured: 0..,
                                            // warmup: kWarmupIterationBase + i), for the results CSV
    std::vector<std::string> failureMessages; // first few
    double wallSeconds = 0.0;
};

// done / total iterations (including warmup), failures so far
using ProgressFn = std::function<void(uint32_t done, uint32_t total, uint32_t failures)>;

// The device was removed, or a fence wait timed out (hung GPU). Once thrown, the GpuBenchmark
// submits nothing more to the device (not even from its destructor).
class DeviceLostError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct BenchmarkOptions
{
    uint32_t fenceTimeoutMs = 10000; // a fence wait longer than this is treated as a hung GPU
    bool serial = false;             // one iteration per command list, wait for it before recording the next
    bool markers = false;            // SetMarker before every flush / sort dispatch (DRED breadcrumb contexts)
    bool logAddresses = false;       // print the GPU VA ranges of all buffers (to match DRED page faults)
    bool testRemoveDevice = false;   // call ID3D12Device5::RemoveDevice after the first submission
};

// Owns the queue, pipelines and buffers for one device and runs (workload, algorithm) combos.
//
// Per iteration, recorded into batched command lists (two batches in flight):
//   1) copy the iteration's descriptors + elements from the upload heap, poison the whole output
//   2) cache flush: a compute pass reading + writing a 256 MB buffer, UAV barrier
//   3) drain: a one-group dispatch on a private 4 KB buffer (own descriptor table), UAV barrier
//   4) timestamp, the algorithm's ExecuteIndirect dispatches, timestamp
//   5) copy the whole output to a readback slot; the CPU verifies it after the batch completes
// That is flush mode 'full' (FlushKind::Full + DrainKind::Group, the default). The drain (pass5)
// makes the GPU execute the barriers before it, and whatever tail of the flush they wait for, before
// the start timestamp: the timed window only holds the sort. The algorithm's flush mode can change
// steps 1-3: FlushKind::Code does the flush before the upload, Data adds an untimed run of the same
// dispatches on a private copy of the iteration (warm buffers + descriptor table) after the flush,
// None skips the flush, FullRo uses a read-only flush; DrainKind::None skips step 3 ('full_legacy' =
// the pre-pass5 'full'), DrainKind::Spin replaces it by an ALU spin dispatch of a calibrated length
// on the same private buffer (see CalibrateDrainSpin).
//
// All buffers are bound through a descriptor table (structured buffer views with exact sizes), not
// root descriptors, so an out-of-bounds shader access reads 0 / is dropped instead of page-faulting.
// Every fence wait has a timeout and checks GetDeviceRemovedReason(); failures throw DeviceLostError.
class GpuBenchmark
{
public:
    // flushShader / flushReadOnlyShader / spinShader: kFlushShaderSource entry points "main" /
    // "main_ro" / "spin".
    GpuBenchmark(ID3D12Device* device, IDxcBlob* flushShader, IDxcBlob* flushReadOnlyShader, IDxcBlob* spinShader,
                 const std::vector<CompiledAlgorithm>& algorithms, const BenchmarkOptions& options);
    ~GpuBenchmark();
    GpuBenchmark(const GpuBenchmark&) = delete;
    GpuBenchmark& operator=(const GpuBenchmark&) = delete;

    // Fills 'result' as it goes, so it holds the partial result if this throws. 'label'
    // ("gpu/algorithm/workload") names command lists and markers for DRED.
    void Run(uint32_t workloadId, size_t algorithmIndex, uint32_t iterations, uint32_t warmup,
             const std::string& label, const ProgressFn& progress, ComboResult& result);

    // Wave probe (see WaveProbe.h): one command list with one group of each shader, dispatched with
    // root constant 0 = i * wordsPerDispatch (the shader's first output word). The output buffer is
    // poisoned first. Returns per shader its wordsPerDispatch output words, or an empty vector and an
    // error message if its PSO could not be created. Must be called before (not during) Run.
    // Throws DeviceLostError on a device loss / fence timeout.
    struct ProbeOutput
    {
        std::vector<uint32_t> words;
        std::string error;
    };
    std::vector<ProbeOutput> RunProbe(const std::vector<IDxcBlob*>& shaders, uint32_t wordsPerDispatch);

    // Run-time estimate: runs 'iterations' iterations of the per-iteration work without a sort
    // (upload, poison, 256 MB flush, drain, timestamps, readback; flush mode 'full') with the data of
    // 'workloadId', after a short untimed warm-up batch, and returns the wall seconds per iteration.
    // That fixed cost dominates an iteration on every GPU measured so far (the sort adds 0.1-40 %).
    // Nothing is verified. Must be called before (not during) Run. Throws DeviceLostError.
    double Calibrate(uint32_t workloadId, uint32_t iterations);

    // Spin drain calibration (DrainKind::Spin): times the spin dispatch (kSpinGroups groups, UAV
    // barrier, between two timestamps) at 1 loop iteration and at n iterations, kSpinReps times each
    // in one command list, n doubled / scaled until the difference of the medians is >= 50 us; the
    // rate is (n - 1) / that difference, so the fixed dispatch cost cancels. Then it times the spin
    // for kSpinCheckUs at that rate as a check. Sets the rate used by Record (SetDrainSpinRate).
    // Run it with the GPU busy (clocks up), e.g. right after Calibrate. Must be called before (not
    // during) Run. Throws DeviceLostError on a device loss, std::runtime_error if no time was measured.
    struct SpinCalibration
    {
        double iterationsPerUs = 0; // the rate
        uint32_t iterations = 0;    // n of the final measurement
        double spanUs = 0;          // median time of n iterations minus that of 1 iteration
        double overheadUs = 0;      // median time of the 1-iteration dispatch (dispatch + barrier cost)
        uint32_t checkIterations = 0; // SpinIterations(kSpinCheckUs)
        double checkUs = 0;         // its median time, including the overhead
    };
    SpinCalibration CalibrateDrainSpin();
    void SetDrainSpinRate(double iterationsPerUs) { m_spinIterationsPerUs = iterationsPerUs; }
    // Loop iterations of a spin drain of 'us' microseconds at the set rate (1..kMaxSpinIterations).
    // Throws std::logic_error if no rate was set.
    uint32_t SpinIterations(uint32_t us) const;
    bool DeviceLost() const { return m_deviceLost; }

    uint64_t TimestampFrequency() const { return m_timestampFrequency; }

    static constexpr uint64_t kFlushBytes = 256ull << 20;
    static constexpr uint64_t kDrainBytes = 4096;
    // Spin drain dispatch: kSpinGroups x kSpinGroupSize threads, one uint4 store each (= the drain
    // buffer). Loop iterations are clamped to kMaxSpinIterations on the CPU and in the shader (a
    // miscalibrated rate cannot produce a TDR-length dispatch: 2^19 dependent iterations take a few
    // ms even at 1 GHz).
    static constexpr uint32_t kSpinGroups = 4;
    static constexpr uint32_t kSpinGroupSize = 64;
    static constexpr uint32_t kMaxSpinIterations = 1u << 19;
    static constexpr uint32_t kSpinReps = 8;
    static constexpr uint32_t kSpinCheckUs = 20;

private:
    struct Frame
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Resource> upload;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12Resource> timestampReadback;
        ComPtr<ID3D12QueryHeap> queryHeap;
        uint8_t* uploadPtr = nullptr;
        uint64_t fenceValue = 0;
        uint32_t firstIteration = 0; // global index (warmup first)
        uint32_t count = 0;
        bool pending = false;
        std::vector<IterationData> data;
    };

    // psos: the sort dispatches (empty: no sort, for Calibrate).
    void Record(Frame& frame, const std::vector<ComPtr<ID3D12PipelineState>>& psos, FlushMode flushMode,
                const std::string& label);
    void Submit(Frame& frame);
    void SubmitList(Frame& frame); // ExecuteCommandLists + Signal (no --test-device-removal hook)
    void WaitForFence(uint64_t value);
    void CheckDevice(const char* where);
    void Process(Frame& frame, uint32_t warmup, ComboResult& result);

    BenchmarkOptions m_options;
    bool m_deviceLost = false;
    bool m_removeRequested = false;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_timestampFrequency = 0;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap; // shader visible: [sort table][flush table][warm table] ...
                                                   // [drain table] (the drain table 4 KB+ away from the others)
    D3D12_GPU_DESCRIPTOR_HANDLE m_sortTable{};     // t0 descs, t1 input, u0 output
    D3D12_GPU_DESCRIPTOR_HANDLE m_flushTable{};    // t0 descs, t1 input, u0 flush buffer
    D3D12_GPU_DESCRIPTOR_HANDLE m_warmTable{};     // FlushKind::Data: t0 warm descs, t1 warm input, u0 warm output
    D3D12_GPU_DESCRIPTOR_HANDLE m_drainTable{};    // t0, t1 null SRVs, u0 the 4 KB drain buffer
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    ComPtr<ID3D12PipelineState> m_flushPso;         // also runs the drain (one group, drain table)
    ComPtr<ID3D12PipelineState> m_flushReadOnlyPso; // FlushKind::FullRo
    ComPtr<ID3D12PipelineState> m_spinPso;          // DrainKind::Spin (drain table)
    double m_spinIterationsPerUs = 0.0;             // SetDrainSpinRate / CalibrateDrainSpin
    std::vector<std::vector<ComPtr<ID3D12PipelineState>>> m_algorithmPsos;

    ComPtr<ID3D12Resource> m_descBuffer;
    ComPtr<ID3D12Resource> m_inputBuffer;
    ComPtr<ID3D12Resource> m_outputBuffer;
    ComPtr<ID3D12Resource> m_poisonBuffer;
    ComPtr<ID3D12Resource> m_flushBuffer;
    ComPtr<ID3D12Resource> m_drainBuffer; // 4 KB, only touched by the drain dispatch
    ComPtr<ID3D12Resource> m_argsBuffer;
    ComPtr<ID3D12Resource> m_warmDescBuffer;   // FlushKind::Data: private copy of the iteration for the
    ComPtr<ID3D12Resource> m_warmInputBuffer;  // untimed code warm-up run
    ComPtr<ID3D12Resource> m_warmOutputBuffer;
    std::vector<FlushMode> m_algorithmFlush;

    static constexpr uint32_t kFrames = 2;
    Frame m_frames[kFrames];
    uint32_t m_batchSize = 0; // iterations per command list (1 in serial mode)
};

// HLSL source of the cache flush shader (compiled at runtime together with the sort shaders).
// Entry points: "main" (read + write every element; also the group drain), "main_ro" (read only),
// "spin" (the spin drain).
extern const char* const kFlushShaderSource;
