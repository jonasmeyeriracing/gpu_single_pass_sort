#include "Benchmark.h"

#include "Verify.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <execution>
#include <numeric>

const char* const kFlushShaderSource = R"(
cbuffer FlushConstants : register(b0)
{
    uint gNumElements;
    uint gNumThreads;
    uint gPad0;
    uint gPad1;
};
RWStructuredBuffer<uint4> gFlush : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    [unroll]
    for (uint k = 0; k < 4; ++k)
    {
        const uint i = id.x + k * gNumThreads;
        if (i < gNumElements)
            gFlush[i] = gFlush[i] + 1;
    }
}
)";

namespace
{
constexpr uint32_t kBatchSize = 32; // iterations per command list
constexpr uint32_t kMaxFailureMessages = 5;

constexpr uint32_t kFlushGroupSize = 256;
constexpr uint32_t kFlushElementsPerThread = 4;
constexpr uint32_t kFlushElements = static_cast<uint32_t>(GpuBenchmark::kFlushBytes / 16); // uint4 elements
constexpr uint32_t kFlushThreads = kFlushElements / kFlushElementsPerThread;
constexpr uint32_t kFlushGroups = kFlushThreads / kFlushGroupSize;

constexpr uint64_t kDescBytes = sizeof(SortDesc) * kSortsPerIteration;
constexpr uint64_t kDescRegionBytes = 256;
constexpr uint64_t kElementBytes = uint64_t(kMaxElementsPerIteration) * sizeof(uint32_t);
constexpr uint64_t kUploadSlotBytes = kDescRegionBytes + kElementBytes;

static_assert(kDescBytes <= kDescRegionBytes);
static_assert(kFlushGroups <= 65535);

// Root signature layout (shared by the flush and all sort shaders).
enum RootParam : UINT
{
    kRootConstants = 0, // b0, 4 x uint (sort: numSorts)
    kRootSortDescs = 1, // t0, StructuredBuffer<uint2> {offset, count}
    kRootInput = 2,     // t1, StructuredBuffer<uint>
    kRootOutput = 3,    // u0, RWStructuredBuffer<uint>
    kRootParamCount
};

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    const D3D12_RESOURCE_STATES state = heapType == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : heapType == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                               : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> resource;
    CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&resource)));
    return resource;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    return b;
}

ComPtr<ID3D12PipelineState> CreateComputePso(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                             IDxcBlob* shader)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature;
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

constexpr D3D12_RESOURCE_STATES kSrvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

GpuBenchmark::GpuBenchmark(ID3D12Device* device, IDxcBlob* flushShader,
                           const std::vector<CompiledAlgorithm>& algorithms)
    : m_device(device)
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)));
    CHECK_HR(m_queue->GetTimestampFrequency(&m_timestampFrequency));
    CHECK_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent failed");

    // Root signature: constants + root SRV/SRV/UAV (no descriptor heaps needed).
    D3D12_ROOT_PARAMETER params[kRootParamCount] = {};
    params[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootConstants].Constants = {0, 0, 4};
    params[kRootSortDescs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kRootSortDescs].Descriptor = {0, 0};
    params[kRootInput].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kRootInput].Descriptor = {1, 0};
    params[kRootOutput].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[kRootOutput].Descriptor = {0, 0};
    for (auto& p : params)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = kRootParamCount;
    rsDesc.pParameters = params;
    ComPtr<ID3DBlob> rsBlob, rsError;
    const HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
    if (FAILED(hr))
        throw std::runtime_error(std::string("D3D12SerializeRootSignature failed: ") +
                                 (rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : ""));
    CHECK_HR(m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_rootSignature)));

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
    sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &arg;
    CHECK_HR(m_device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&m_dispatchSignature)));

    m_flushPso = CreateComputePso(m_device.Get(), m_rootSignature.Get(), flushShader);
    for (const auto& algorithm : algorithms)
    {
        std::vector<ComPtr<ID3D12PipelineState>> psos;
        for (const auto& shader : algorithm.shaders)
            psos.push_back(CreateComputePso(m_device.Get(), m_rootSignature.Get(), shader.Get()));
        m_algorithmPsos.push_back(std::move(psos));
    }

    m_descBuffer = CreateBuffer(m_device.Get(), kDescRegionBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_inputBuffer = CreateBuffer(m_device.Get(), kElementBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_outputBuffer = CreateBuffer(m_device.Get(), kElementBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_poisonBuffer = CreateBuffer(m_device.Get(), kElementBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_flushBuffer = CreateBuffer(m_device.Get(), kFlushBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_argsBuffer = CreateBuffer(m_device.Get(), sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT);

    for (auto& f : m_frames)
    {
        CHECK_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)));
        CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&f.list)));
        CHECK_HR(f.list->Close());
        f.upload = CreateBuffer(m_device.Get(), kUploadSlotBytes * kBatchSize, D3D12_HEAP_TYPE_UPLOAD);
        f.readback = CreateBuffer(m_device.Get(), kElementBytes * kBatchSize, D3D12_HEAP_TYPE_READBACK);
        f.timestampReadback =
            CreateBuffer(m_device.Get(), sizeof(uint64_t) * 2 * kBatchSize, D3D12_HEAP_TYPE_READBACK);
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * kBatchSize;
        CHECK_HR(m_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&f.queryHeap)));
        const D3D12_RANGE noRead{0, 0};
        CHECK_HR(f.upload->Map(0, &noRead, reinterpret_cast<void**>(&f.uploadPtr)));
        f.data.resize(kBatchSize);
    }

    // One-time init: poison pattern and the static indirect args {numSorts, 1, 1}.
    {
        ComPtr<ID3D12Resource> staging =
            CreateBuffer(m_device.Get(), kElementBytes + 256, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* ptr = nullptr;
        CHECK_HR(staging->Map(0, nullptr, reinterpret_cast<void**>(&ptr)));
        std::fill_n(reinterpret_cast<uint32_t*>(ptr), kMaxElementsPerIteration, kPoisonValue);
        const D3D12_DISPATCH_ARGUMENTS args{kSortsPerIteration, 1, 1};
        memcpy(ptr + kElementBytes, &args, sizeof(args));
        staging->Unmap(0, nullptr);

        Frame& f = m_frames[0];
        CHECK_HR(f.allocator->Reset());
        CHECK_HR(f.list->Reset(f.allocator.Get(), nullptr));
        f.list->CopyBufferRegion(m_poisonBuffer.Get(), 0, staging.Get(), 0, kElementBytes);
        f.list->CopyBufferRegion(m_argsBuffer.Get(), 0, staging.Get(), kElementBytes, sizeof(args));
        CHECK_HR(f.list->Close());
        ID3D12CommandList* lists[] = {f.list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        CHECK_HR(m_queue->Signal(m_fence.Get(), ++m_fenceValue));
        WaitForFence(m_fenceValue);
    }
}

GpuBenchmark::~GpuBenchmark()
{
    if (m_queue && m_fence)
    {
        if (SUCCEEDED(m_queue->Signal(m_fence.Get(), ++m_fenceValue)))
        {
            try
            {
                WaitForFence(m_fenceValue);
            }
            catch (...)
            {
            }
        }
    }
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
}

void GpuBenchmark::WaitForFence(uint64_t value)
{
    if (m_fence->GetCompletedValue() >= value)
        return;
    CHECK_HR(m_fence->SetEventOnCompletion(value, m_fenceEvent));
    for (int seconds = 0; WaitForSingleObject(m_fenceEvent, 1000) == WAIT_TIMEOUT; ++seconds)
    {
        const HRESULT removed = m_device->GetDeviceRemovedReason();
        if (FAILED(removed))
            ThrowHr(removed, "GPU device removed", __FILE__, __LINE__);
        if (seconds >= 120)
            throw std::runtime_error("timed out waiting for the GPU (120 s)");
    }
}

void GpuBenchmark::Record(Frame& f, size_t algorithmIndex)
{
    ID3D12GraphicsCommandList* cl = f.list.Get();
    CHECK_HR(f.allocator->Reset());
    CHECK_HR(cl->Reset(f.allocator.Get(), nullptr));
    cl->SetComputeRootSignature(m_rootSignature.Get());

    ID3D12Resource* desc = m_descBuffer.Get();
    ID3D12Resource* input = m_inputBuffer.Get();
    ID3D12Resource* output = m_outputBuffer.Get();

    // Buffers decay to COMMON after every ExecuteCommandLists; start each list from there.
    {
        const D3D12_RESOURCE_BARRIER b[] = {
            Transition(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(m_poisonBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(m_flushBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(m_argsBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        };
        cl->ResourceBarrier(_countof(b), b);
    }

    const auto& psos = m_algorithmPsos[algorithmIndex];
    for (uint32_t s = 0; s < f.count; ++s)
    {
        const IterationData& d = f.data[s];
        const uint64_t bytes = d.elements.size() * sizeof(uint32_t);
        const uint64_t uploadOffset = s * kUploadSlotBytes;
        memcpy(f.uploadPtr + uploadOffset, d.sorts, kDescBytes);
        if (bytes)
            memcpy(f.uploadPtr + uploadOffset + kDescRegionBytes, d.elements.data(), bytes);

        // 1) upload + poison the output
        cl->CopyBufferRegion(desc, 0, f.upload.Get(), uploadOffset, kDescBytes);
        if (bytes)
        {
            cl->CopyBufferRegion(input, 0, f.upload.Get(), uploadOffset + kDescRegionBytes, bytes);
            cl->CopyBufferRegion(output, 0, m_poisonBuffer.Get(), 0, bytes);
        }
        {
            const D3D12_RESOURCE_BARRIER b[] = {
                Transition(desc, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState),
                Transition(input, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState),
                Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(_countof(b), b);
        }

        // 2) cache flush
        const uint32_t flushConstants[4] = {kFlushElements, kFlushThreads, 0, 0};
        cl->SetPipelineState(m_flushPso.Get());
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, flushConstants, 0);
        cl->SetComputeRootShaderResourceView(kRootSortDescs, desc->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(kRootInput, input->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(kRootOutput, m_flushBuffer->GetGPUVirtualAddress());
        cl->Dispatch(kFlushGroups, 1, 1);
        {
            const D3D12_RESOURCE_BARRIER b = UavBarrier(nullptr);
            cl->ResourceBarrier(1, &b);
        }

        // 3) timed sort
        const uint32_t sortConstants[4] = {kSortsPerIteration, 0, 0, 0};
        cl->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * s);
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, sortConstants, 0);
        cl->SetComputeRootUnorderedAccessView(kRootOutput, output->GetGPUVirtualAddress());
        for (const auto& pso : psos)
        {
            cl->SetPipelineState(pso.Get());
            cl->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_argsBuffer.Get(), 0, nullptr, 0);
        }
        cl->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * s + 1);

        // 4) readback
        {
            const D3D12_RESOURCE_BARRIER b[] = {
                Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(desc, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(input, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cl->ResourceBarrier(_countof(b), b);
        }
        if (bytes)
            cl->CopyBufferRegion(f.readback.Get(), s * kElementBytes, output, 0, bytes);
        {
            const D3D12_RESOURCE_BARRIER b =
                Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->ResourceBarrier(1, &b);
        }
    }

    cl->ResolveQueryData(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * f.count, f.timestampReadback.Get(),
                         0);
    {
        const D3D12_RESOURCE_BARRIER b[] = {
            Transition(desc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_poisonBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_flushBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_argsBuffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON),
        };
        cl->ResourceBarrier(_countof(b), b);
    }
    CHECK_HR(cl->Close());
}

void GpuBenchmark::Submit(Frame& f)
{
    ID3D12CommandList* lists[] = {f.list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    CHECK_HR(m_queue->Signal(m_fence.Get(), ++m_fenceValue));
    f.fenceValue = m_fenceValue;
    f.pending = true;
}

void GpuBenchmark::Process(Frame& f, uint32_t warmup, ComboResult& result)
{
    WaitForFence(f.fenceValue);
    f.pending = false;

    const D3D12_RANGE tsRange{0, sizeof(uint64_t) * 2 * f.count};
    uint64_t* timestamps = nullptr;
    CHECK_HR(f.timestampReadback->Map(0, &tsRange, reinterpret_cast<void**>(&timestamps)));
    const D3D12_RANGE outRange{0, kElementBytes * f.count};
    uint8_t* outputs = nullptr;
    CHECK_HR(f.readback->Map(0, &outRange, reinterpret_cast<void**>(&outputs)));

    std::vector<uint32_t> slots(f.count);
    std::iota(slots.begin(), slots.end(), 0u);
    std::vector<std::string> messages(f.count);
    std::vector<char> ok(f.count, 0);
    std::for_each(std::execution::par, slots.begin(), slots.end(), [&](uint32_t s) {
        ok[s] = VerifyIteration(f.data[s], reinterpret_cast<const uint32_t*>(outputs + s * kElementBytes),
                                &messages[s])
                    ? 1
                    : 0;
    });

    for (uint32_t s = 0; s < f.count; ++s)
    {
        const uint32_t global = f.firstIteration + s;
        if (global >= warmup)
        {
            const uint64_t ticks = timestamps[2 * s + 1] - timestamps[2 * s];
            result.timesUs.push_back(static_cast<double>(ticks) * 1e6 / static_cast<double>(m_timestampFrequency));
        }
        if (!ok[s])
        {
            ++result.failures;
            if (result.failureMessages.size() < kMaxFailureMessages)
            {
                const std::string where = global < warmup ? Format("warmup iteration %u", global)
                                                          : Format("iteration %u", global - warmup);
                result.failureMessages.push_back(where + ": " + messages[s]);
            }
        }
    }
    result.iterationsRun += f.count;

    const D3D12_RANGE noWrite{0, 0};
    f.readback->Unmap(0, &noWrite);
    f.timestampReadback->Unmap(0, &noWrite);
}

ComboResult GpuBenchmark::Run(uint32_t workloadId, size_t algorithmIndex, uint32_t iterations, uint32_t warmup,
                              const ProgressFn& progress)
{
    const auto start = std::chrono::steady_clock::now();
    ComboResult result;
    result.timesUs.reserve(iterations);
    const uint32_t total = warmup + iterations;

    uint32_t next = 0;
    uint32_t frameIndex = 0;
    for (;;)
    {
        Frame& f = m_frames[frameIndex];
        if (f.pending)
        {
            Process(f, warmup, result);
            if (progress)
                progress(result.iterationsRun, total, result.failures);
        }
        if (next < total)
        {
            f.firstIteration = next;
            f.count = std::min(kBatchSize, total - next);
            std::vector<uint32_t> slots(f.count);
            std::iota(slots.begin(), slots.end(), 0u);
            std::for_each(std::execution::par, slots.begin(), slots.end(), [&](uint32_t s) {
                const uint32_t global = f.firstIteration + s;
                const uint32_t iteration = global < warmup ? kWarmupIterationBase + global : global - warmup;
                GenerateIteration(workloadId, iteration, f.data[s]);
            });
            Record(f, algorithmIndex);
            Submit(f);
            next += f.count;
        }
        frameIndex = (frameIndex + 1) % kFrames;

        bool anyPending = false;
        for (const auto& fr : m_frames)
            anyPending |= fr.pending;
        if (next >= total && !anyPending)
            break;
    }

    result.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}
