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
    FlushMode flush = FlushMode::Full;     // see FlushMode (Algorithms.h)
    bool usesWaveOps = false;              // some dispatch uses wave intrinsics (depends on the wave size)
};

struct ComboResult
{
    std::vector<double> timesUs;        // measured iterations only (warmup excluded)
    uint32_t iterationsRun = 0;         // including warmup
    uint32_t failures = 0;              // verification failures (including warmup)
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
//   2) cache flush: a compute pass reading + writing a 256 MB buffer
//   3) UAV barrier, timestamp, the algorithm's ExecuteIndirect dispatches, timestamp
//   4) copy the whole output to a readback slot; the CPU verifies it after the batch completes
// That is FlushMode::Full (the default). The algorithm's flush mode can change steps 1-2: Code does
// the flush before the upload, Data adds an untimed run of the same dispatches on a private copy of
// the iteration (warm buffers + descriptor table) after the flush, None skips the flush.
//
// All buffers are bound through a descriptor table (structured buffer views with exact sizes), not
// root descriptors, so an out-of-bounds shader access reads 0 / is dropped instead of page-faulting.
// Every fence wait has a timeout and checks GetDeviceRemovedReason(); failures throw DeviceLostError.
class GpuBenchmark
{
public:
    GpuBenchmark(ID3D12Device* device, IDxcBlob* flushShader, const std::vector<CompiledAlgorithm>& algorithms,
                 const BenchmarkOptions& options);
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

    bool DeviceLost() const { return m_deviceLost; }

    uint64_t TimestampFrequency() const { return m_timestampFrequency; }

    static constexpr uint64_t kFlushBytes = 256ull << 20;

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

    void Record(Frame& frame, size_t algorithmIndex, const std::string& label);
    void Submit(Frame& frame);
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
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap; // shader visible: [sort table][flush table][warm table]
    D3D12_GPU_DESCRIPTOR_HANDLE m_sortTable{};     // t0 descs, t1 input, u0 output
    D3D12_GPU_DESCRIPTOR_HANDLE m_flushTable{};    // t0 descs, t1 input, u0 flush buffer
    D3D12_GPU_DESCRIPTOR_HANDLE m_warmTable{};     // FlushMode::Data: t0 warm descs, t1 warm input, u0 warm output
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    ComPtr<ID3D12PipelineState> m_flushPso;
    std::vector<std::vector<ComPtr<ID3D12PipelineState>>> m_algorithmPsos;

    ComPtr<ID3D12Resource> m_descBuffer;
    ComPtr<ID3D12Resource> m_inputBuffer;
    ComPtr<ID3D12Resource> m_outputBuffer;
    ComPtr<ID3D12Resource> m_poisonBuffer;
    ComPtr<ID3D12Resource> m_flushBuffer;
    ComPtr<ID3D12Resource> m_argsBuffer;
    ComPtr<ID3D12Resource> m_warmDescBuffer;   // FlushMode::Data: private copy of the iteration for the
    ComPtr<ID3D12Resource> m_warmInputBuffer;  // untimed code warm-up run
    ComPtr<ID3D12Resource> m_warmOutputBuffer;
    std::vector<FlushMode> m_algorithmFlush;

    static constexpr uint32_t kFrames = 2;
    Frame m_frames[kFrames];
    uint32_t m_batchSize = 0; // iterations per command list (1 in serial mode)
};

// HLSL source of the cache flush shader (compiled at runtime together with the sort shaders).
extern const char* const kFlushShaderSource;
