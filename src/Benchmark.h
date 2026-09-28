#pragma once

#include "Common.h"
#include "Workloads.h"

#include <d3d12.h>
#include <dxcapi.h>

#include <functional>
#include <string>
#include <vector>

struct CompiledAlgorithm
{
    std::string name;
    std::vector<ComPtr<IDxcBlob>> shaders; // one per dispatch, in order
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

// Owns the queue, pipelines and buffers for one device and runs (workload, algorithm) combos.
//
// Per iteration, recorded into batched command lists (two batches in flight):
//   1) copy the iteration's descriptors + elements from the upload heap, poison the output
//   2) cache flush: a compute pass reading + writing a 256 MB buffer
//   3) UAV barrier, timestamp, the algorithm's ExecuteIndirect dispatches, timestamp
//   4) copy the output to a readback slot; the CPU verifies it after the batch completes
class GpuBenchmark
{
public:
    GpuBenchmark(ID3D12Device* device, IDxcBlob* flushShader, const std::vector<CompiledAlgorithm>& algorithms);
    ~GpuBenchmark();
    GpuBenchmark(const GpuBenchmark&) = delete;
    GpuBenchmark& operator=(const GpuBenchmark&) = delete;

    ComboResult Run(uint32_t workloadId, size_t algorithmIndex, uint32_t iterations, uint32_t warmup,
                    const ProgressFn& progress);

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

    void Record(Frame& frame, size_t algorithmIndex);
    void Submit(Frame& frame);
    void WaitForFence(uint64_t value);
    void Process(Frame& frame, uint32_t warmup, ComboResult& result);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_timestampFrequency = 0;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    ComPtr<ID3D12PipelineState> m_flushPso;
    std::vector<std::vector<ComPtr<ID3D12PipelineState>>> m_algorithmPsos;

    ComPtr<ID3D12Resource> m_descBuffer;
    ComPtr<ID3D12Resource> m_inputBuffer;
    ComPtr<ID3D12Resource> m_outputBuffer;
    ComPtr<ID3D12Resource> m_poisonBuffer;
    ComPtr<ID3D12Resource> m_flushBuffer;
    ComPtr<ID3D12Resource> m_argsBuffer;

    static constexpr uint32_t kFrames = 2;
    Frame m_frames[kFrames];
};

// HLSL source of the cache flush shader (compiled at runtime together with the sort shaders).
extern const char* const kFlushShaderSource;
