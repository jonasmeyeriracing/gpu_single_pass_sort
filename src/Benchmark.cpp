#include "Benchmark.h"

#include "Verify.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

// Read-only variant (flush mode full_ro): the same loads, no stores, so the flush leaves no dirty
// lines behind. The store condition keeps the loads alive and is never true: every element holds
// the number of read+write flush passes so far (the same in all 4 components), so the sum of the
// 16 components a thread reads is a multiple of 16, never the odd 0xDEADBEEF.
[numthreads(256, 1, 1)]
void main_ro(uint3 id : SV_DispatchThreadID)
{
    uint4 acc = 0;
    [unroll]
    for (uint k = 0; k < 4; ++k)
    {
        const uint i = id.x + k * gNumThreads;
        if (i < gNumElements)
            acc += gFlush[i];
    }
    if (acc.x + acc.y + acc.z + acc.w == 0xDEADBEEFu)
        gFlush[id.x] = acc;
}

// Spin drain (DrainKind::Spin), on the 4 KB drain buffer: gNumElements = loop iterations (clamped
// to GpuBenchmark::kMaxSpinIterations). A dependent integer chain per thread with no memory access
// in the loop, then one store per thread (4 groups x 64 threads = the buffer's 256 uint4) so the
// loop cannot be removed.
[numthreads(64, 1, 1)]
void spin(uint3 id : SV_DispatchThreadID)
{
    const uint n = min(gNumElements, 524288u);
    uint x = id.x * 2654435761u + 1u;
    [loop]
    for (uint i = 0; i < n; ++i)
    {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 15;
    }
    gFlush[id.x] = uint4(x, n, 0, 0);
}
)";

namespace
{
constexpr uint32_t kMaxFailureMessages = 5;
// Verification tasks (Process): up to this many sorts, or this many elements of the tail after the
// last sort, per task, so a large iteration is checked by several threads.
constexpr uint32_t kVerifySortsPerTask = 32;
constexpr uint32_t kVerifyTailElementsPerTask = 1u << 18;

constexpr uint32_t kFlushGroupSize = 256;
constexpr uint32_t kFlushElementsPerThread = 4;
constexpr uint32_t kFlushElements = static_cast<uint32_t>(GpuBenchmark::kFlushBytes / 16); // uint4 elements
constexpr uint32_t kFlushThreads = kFlushElements / kFlushElementsPerThread;
constexpr uint32_t kFlushGroups = kFlushThreads / kFlushGroupSize;

// Per-iteration sizes for 'numSorts' sorts. An upload slot holds the sort descriptors (in a region
// rounded up to 256 bytes) and then the elements; a readback slot the whole output.
constexpr uint64_t DescBytes(uint32_t numSorts)
{
    return sizeof(SortDesc) * uint64_t(numSorts);
}
constexpr uint64_t DescRegionBytes(uint32_t numSorts)
{
    return (DescBytes(numSorts) + 255) / 256 * 256;
}
constexpr uint64_t ElementBytes(uint32_t numSorts)
{
    return uint64_t(MaxElementsPerIteration(numSorts)) * sizeof(uint32_t);
}
constexpr uint64_t UploadSlotBytes(uint32_t numSorts)
{
    return DescRegionBytes(numSorts) + ElementBytes(numSorts);
}
uint32_t BatchIterations(uint32_t numSorts, bool serial)
{
    if (serial)
        return 1;
    const uint64_t perIteration = UploadSlotBytes(numSorts) + ElementBytes(numSorts);
    return static_cast<uint32_t>(
        std::clamp<uint64_t>(GpuBenchmark::kMaxBatchBytes / perIteration, 1, GpuBenchmark::kMaxBatchIterations));
}

static_assert(DescRegionBytes(kDefaultSortsPerIteration) == 256, "the 20-sort layout of the fixed-20 builds");
static_assert(kFlushGroups <= 65535);
static_assert(kMaxSortsPerIteration <= 65535, "one dispatch of numSorts groups");

// Drain (see Record): the flush shader, one group, on the 4 KB drain buffer (one uint4 per thread).
constexpr uint32_t kDrainElements = static_cast<uint32_t>(GpuBenchmark::kDrainBytes / 16);
static_assert(kDrainElements == kFlushGroupSize, "the drain is one flush group, one element per thread");
static_assert(GpuBenchmark::kSpinGroups * GpuBenchmark::kSpinGroupSize == kDrainElements,
              "the spin drain writes one drain element per thread");
static_assert(GpuBenchmark::kSpinGroupSize == 64, "must match numthreads of the spin entry point");
static_assert(GpuBenchmark::kMaxSpinIterations == 524288, "must match the clamp in the spin entry point");
static_assert(4 * GpuBenchmark::kSpinReps <= 2 * GpuBenchmark::kMaxBatchIterations,
              "spin calibration timestamps fit in a frame's query heap");

// Root signature layout (shared by the flush and all sort shaders).
enum RootParam : UINT
{
    kRootConstants = 0, // b0, 4 x uint (sort: numSorts)
    kRootTable = 1,     // descriptor table: t0 StructuredBuffer<uint2> {offset, count}, t1 StructuredBuffer<uint>
                        //                   input, u0 RWStructuredBuffer<uint> output (flush: <uint4> buffer)
    kRootParamCount
};

// Descriptor heap layout: four 3-descriptor tables {t0, t1, u0}. The drain table sits 128
// descriptors (>= 4 KB) after the others, so the drain dispatch's descriptor fetches do not share a
// cache line with the sort table (the timed sort's descriptors stay as cold as in legacy mode).
enum DescriptorSlot : UINT
{
    kSlotSortDescs = 0,
    kSlotSortInput = 1,
    kSlotSortOutput = 2,
    kSlotFlushDescs = 3,
    kSlotFlushInput = 4,
    kSlotFlushBuffer = 5,
    kSlotWarmDescs = 6,
    kSlotWarmInput = 7,
    kSlotWarmOutput = 8,
    kSlotDrainDescs = 128, // null SRV
    kSlotDrainInput = 129, // null SRV
    kSlotDrainBuffer = 130,
    kDescriptorCount
};

void SetName(ID3D12Object* object, const std::string& name)
{
    object->SetName(Utf8ToWide(name).c_str());
}

void CreateStructuredSrv(ID3D12Device* device, ID3D12Resource* resource, uint32_t numElements, uint32_t stride,
                         D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.FirstElement = 0;
    d.Buffer.NumElements = numElements;
    d.Buffer.StructureByteStride = stride;
    device->CreateShaderResourceView(resource, &d, handle);
}

void CreateStructuredUav(ID3D12Device* device, ID3D12Resource* resource, uint32_t numElements, uint32_t stride,
                         D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.FirstElement = 0;
    d.Buffer.NumElements = numElements;
    d.Buffer.StructureByteStride = stride;
    device->CreateUnorderedAccessView(resource, nullptr, &d, handle);
}

const char* RemovedReasonName(HRESULT hr)
{
    switch (hr)
    {
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    default: return "unknown";
    }
}

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

GpuBenchmark::GpuBenchmark(ID3D12Device* device, IDxcBlob* flushShader, IDxcBlob* flushReadOnlyShader,
                           IDxcBlob* spinShader, const std::vector<CompiledAlgorithm>& algorithms,
                           const std::vector<uint32_t>& sortCounts, const BenchmarkOptions& options)
    : m_options(options), m_device(device)
{
    // Buffer capacity: the largest sort count (at least the default 20: the wave probe's output).
    m_capacitySorts = kDefaultSortsPerIteration;
    for (uint32_t n : sortCounts)
    {
        if (n == 0 || n > kMaxSortsPerIteration)
            throw std::invalid_argument(Format("sorts per iteration %u is outside 1..%u", n, kMaxSortsPerIteration));
        m_capacitySorts = std::max(m_capacitySorts, n);
    }
    // Upload / readback heaps per frame: room for a full batch at every count this benchmark runs,
    // and for one iteration at the capacity (the wave probe reads back into the readback heap).
    for (uint32_t n : sortCounts)
    {
        const uint32_t batch = BatchIterations(n, m_options.serial);
        m_uploadFrameBytes = std::max(m_uploadFrameBytes, batch * UploadSlotBytes(n));
        m_readbackFrameBytes = std::max(m_readbackFrameBytes, batch * ElementBytes(n));
    }
    m_uploadFrameBytes = std::max(m_uploadFrameBytes, UploadSlotBytes(m_capacitySorts));
    m_readbackFrameBytes = std::max(m_readbackFrameBytes, ElementBytes(m_capacitySorts));
    const uint64_t descRegionBytes = DescRegionBytes(m_capacitySorts);
    const uint64_t elementBytes = ElementBytes(m_capacitySorts);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)));
    SetName(m_queue.Get(), "GpuSort direct queue");
    CHECK_HR(m_queue->GetTimestampFrequency(&m_timestampFrequency));
    CHECK_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    SetName(m_fence.Get(), "GpuSort fence");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent failed");

    // Root signature: constants + one descriptor table {t0, t1, u0}. Descriptor tables (unlike
    // root descriptors) are bounds-checked: out-of-bounds reads return 0, writes are dropped.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[kRootParamCount] = {};
    params[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootConstants].Constants = {0, 0, 4};
    params[kRootTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootTable].DescriptorTable = {_countof(ranges), ranges};
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
    SetName(m_rootSignature.Get(), "GpuSort root signature");

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
    sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &arg;
    CHECK_HR(m_device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&m_dispatchSignature)));
    SetName(m_dispatchSignature.Get(), "GpuSort dispatch command signature");

    m_flushPso = CreateComputePso(m_device.Get(), m_rootSignature.Get(), flushShader);
    SetName(m_flushPso.Get(), "PSO flush");
    m_flushReadOnlyPso = CreateComputePso(m_device.Get(), m_rootSignature.Get(), flushReadOnlyShader);
    SetName(m_flushReadOnlyPso.Get(), "PSO flush read-only");
    m_spinPso = CreateComputePso(m_device.Get(), m_rootSignature.Get(), spinShader);
    SetName(m_spinPso.Get(), "PSO drain spin");
    for (const auto& algorithm : algorithms)
    {
        std::vector<ComPtr<ID3D12PipelineState>> psos;
        for (const auto& shader : algorithm.shaders)
        {
            psos.push_back(CreateComputePso(m_device.Get(), m_rootSignature.Get(), shader.Get()));
            SetName(psos.back().Get(), Format("PSO %s dispatch %zu", algorithm.name.c_str(), psos.size() - 1));
        }
        m_algorithmPsos.push_back(std::move(psos));
        m_algorithmFlush.push_back(algorithm.flush);
    }

    m_descBuffer = CreateBuffer(m_device.Get(), descRegionBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_inputBuffer = CreateBuffer(m_device.Get(), elementBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_outputBuffer = CreateBuffer(m_device.Get(), elementBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_poisonBuffer = CreateBuffer(m_device.Get(), elementBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_flushBuffer = CreateBuffer(m_device.Get(), kFlushBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_drainBuffer = CreateBuffer(m_device.Get(), kDrainBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_argsBuffer = CreateBuffer(m_device.Get(), sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT);
    m_argsUpload = CreateBuffer(m_device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD);
    m_warmDescBuffer = CreateBuffer(m_device.Get(), descRegionBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_warmInputBuffer = CreateBuffer(m_device.Get(), elementBytes, D3D12_HEAP_TYPE_DEFAULT);
    m_warmOutputBuffer = CreateBuffer(m_device.Get(), elementBytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    SetName(m_descBuffer.Get(), "buffer sortDescs (t0)");
    SetName(m_inputBuffer.Get(), "buffer sortInput (t1)");
    SetName(m_outputBuffer.Get(), "buffer sortOutput (u0)");
    SetName(m_poisonBuffer.Get(), "buffer poison");
    SetName(m_flushBuffer.Get(), "buffer flush256MB (flush u0)");
    SetName(m_drainBuffer.Get(), "buffer drain4KB (drain u0)");
    SetName(m_argsBuffer.Get(), "buffer indirectArgs");
    SetName(m_argsUpload.Get(), "buffer indirectArgs staging");
    {
        const D3D12_RANGE noRead{0, 0};
        CHECK_HR(m_argsUpload->Map(0, &noRead, reinterpret_cast<void**>(&m_argsUploadPtr)));
    }
    SetName(m_warmDescBuffer.Get(), "buffer warmSortDescs (flush mode data, t0)");
    SetName(m_warmInputBuffer.Get(), "buffer warmSortInput (flush mode data, t1)");
    SetName(m_warmOutputBuffer.Get(), "buffer warmSortOutput (flush mode data, u0)");

    // Structured buffer views with exact sizes: numSorts sort descriptors and numSorts x 8192
    // elements (written by SetSortCount), the full flush buffer and the drain buffer. Accesses past
    // NumElements read 0 / are dropped.
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kDescriptorCount;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK_HR(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_descriptorHeap)));
        SetName(m_descriptorHeap.Get(), "GpuSort descriptor heap");
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        auto cpu = [&](UINT slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu0.ptr + SIZE_T(slot) * inc}; };
        ID3D12Device* d = m_device.Get();
        CreateStructuredUav(d, m_flushBuffer.Get(), kFlushElements, 16, cpu(kSlotFlushBuffer));
        for (UINT slot : {kSlotDrainDescs, kSlotDrainInput})
        {
            // Null descriptors (the flush shader does not read t0 / t1).
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
            nullSrv.Format = DXGI_FORMAT_UNKNOWN;
            nullSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrv.Buffer.NumElements = 1;
            nullSrv.Buffer.StructureByteStride = 4;
            d->CreateShaderResourceView(nullptr, &nullSrv, cpu(slot));
        }
        CreateStructuredUav(d, m_drainBuffer.Get(), kDrainElements, 16, cpu(kSlotDrainBuffer));
        m_sortTable = {gpu0.ptr + UINT64(kSlotSortDescs) * inc};
        m_flushTable = {gpu0.ptr + UINT64(kSlotFlushDescs) * inc};
        m_warmTable = {gpu0.ptr + UINT64(kSlotWarmDescs) * inc};
        m_drainTable = {gpu0.ptr + UINT64(kSlotDrainDescs) * inc};
    }

    for (uint32_t fi = 0; fi < kFrames; ++fi)
    {
        Frame& f = m_frames[fi];
        CHECK_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)));
        CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&f.list)));
        CHECK_HR(f.list->Close());
        f.upload = CreateBuffer(m_device.Get(), m_uploadFrameBytes, D3D12_HEAP_TYPE_UPLOAD);
        f.readback = CreateBuffer(m_device.Get(), m_readbackFrameBytes, D3D12_HEAP_TYPE_READBACK);
        f.timestampReadback =
            CreateBuffer(m_device.Get(), sizeof(uint64_t) * 2 * kMaxBatchIterations, D3D12_HEAP_TYPE_READBACK);
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * kMaxBatchIterations;
        CHECK_HR(m_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&f.queryHeap)));
        SetName(f.allocator.Get(), Format("frame%u allocator", fi));
        SetName(f.list.Get(), Format("frame%u list", fi));
        SetName(f.upload.Get(), Format("frame%u upload", fi));
        SetName(f.readback.Get(), Format("frame%u readback", fi));
        SetName(f.timestampReadback.Get(), Format("frame%u timestampReadback", fi));
        SetName(f.queryHeap.Get(), Format("frame%u timestamp queries", fi));
        const D3D12_RANGE noRead{0, 0};
        CHECK_HR(f.upload->Map(0, &noRead, reinterpret_cast<void**>(&f.uploadPtr)));
        f.data.resize(kMaxBatchIterations);
    }

    // One-time init: the poison pattern (the indirect args follow in SetSortCount).
    {
        ComPtr<ID3D12Resource> staging = CreateBuffer(m_device.Get(), elementBytes, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* ptr = nullptr;
        CHECK_HR(staging->Map(0, nullptr, reinterpret_cast<void**>(&ptr)));
        std::fill_n(reinterpret_cast<uint32_t*>(ptr), MaxElementsPerIteration(m_capacitySorts), kPoisonValue);
        staging->Unmap(0, nullptr);

        Frame& f = m_frames[0];
        CHECK_HR(f.allocator->Reset());
        CHECK_HR(f.list->Reset(f.allocator.Get(), nullptr));
        f.list->CopyBufferRegion(m_poisonBuffer.Get(), 0, staging.Get(), 0, elementBytes);
        CHECK_HR(f.list->Close());
        SubmitAndWait(f);
    }
    SetSortCount(m_capacitySorts);

    if (m_options.logAddresses)
    {
        const std::pair<const char*, ID3D12Resource*> buffers[] = {
            {"sortDescs", m_descBuffer.Get()},
            {"sortInput", m_inputBuffer.Get()},
            {"sortOutput", m_outputBuffer.Get()},
            {"poison", m_poisonBuffer.Get()},
            {"flush256MB", m_flushBuffer.Get()},
            {"drain4KB", m_drainBuffer.Get()},
            {"indirectArgs", m_argsBuffer.Get()},
            {"indirectArgs staging", m_argsUpload.Get()},
            {"warmSortDescs", m_warmDescBuffer.Get()},
            {"warmSortInput", m_warmInputBuffer.Get()},
            {"warmSortOutput", m_warmOutputBuffer.Get()},
            {"frame0 upload", m_frames[0].upload.Get()},
            {"frame0 readback", m_frames[0].readback.Get()},
            {"frame1 upload", m_frames[1].upload.Get()},
            {"frame1 readback", m_frames[1].readback.Get()},
        };
        Log("  Buffer GPU VAs (to match a DRED page-fault VA):\n");
        for (const auto& [name, res] : buffers)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS va = res->GetGPUVirtualAddress();
            Log("    %-16s 0x%016llX - 0x%016llX\n", name, static_cast<unsigned long long>(va),
                static_cast<unsigned long long>(va + res->GetDesc().Width));
        }
    }
}

GpuBenchmark::~GpuBenchmark()
{
    // Never touch a removed / hung device again: no Signal, no wait.
    if (m_queue && m_fence && !m_deviceLost)
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

void GpuBenchmark::CheckDevice(const char* where)
{
    const HRESULT reason = m_device->GetDeviceRemovedReason();
    if (FAILED(reason))
    {
        m_deviceLost = true;
        throw DeviceLostError(Format("GPU device removed (detected %s): GetDeviceRemovedReason = 0x%08X (%s)", where,
                                     static_cast<unsigned>(reason), RemovedReasonName(reason)));
    }
}

void GpuBenchmark::SubmitAndWait(Frame& f)
{
    if (m_deviceLost)
        throw DeviceLostError("GPU device already lost");
    CheckDevice("before submitting");
    ID3D12CommandList* lists[] = {f.list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    CHECK_HR(m_queue->Signal(m_fence.Get(), ++m_fenceValue));
    WaitForFence(m_fenceValue);
}

void GpuBenchmark::SetSortCount(uint32_t numSorts)
{
    if (numSorts == 0 || numSorts > m_capacitySorts)
        throw std::invalid_argument(
            Format("SetSortCount(%u): the buffers hold 1..%u sorts per iteration", numSorts, m_capacitySorts));
    if (m_frames[0].pending || m_frames[1].pending)
        throw std::runtime_error("SetSortCount: called while iterations are in flight");
    // Iterations per command list: as BatchIterations, and never more than the frame heaps hold
    // (they are sized for the constructor's sort counts).
    uint32_t batch = BatchIterations(numSorts, m_options.serial);
    batch = static_cast<uint32_t>(std::min<uint64_t>(
        {batch, m_uploadFrameBytes / UploadSlotBytes(numSorts), m_readbackFrameBytes / ElementBytes(numSorts)}));
    if (batch == 0)
        throw std::invalid_argument(Format("SetSortCount(%u): not one of the counts the benchmark was created for",
                                           numSorts));
    m_numSorts = numSorts;
    m_batchSize = batch;
    // Release the data of slots the smaller batch no longer uses (large counts: 16 MB each).
    for (Frame& fr : m_frames)
    {
        for (size_t i = batch; i < fr.data.size(); ++i)
            fr.data[i] = IterationData{};
    }

    // Views with exactly numSorts descriptors and numSorts x 8192 elements (no command list that
    // uses the heap is in flight).
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpu = [&](UINT slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu0.ptr + SIZE_T(slot) * inc}; };
    ID3D12Device* d = m_device.Get();
    static_assert(sizeof(SortDesc) == 8, "must match StructuredBuffer<uint2>");
    constexpr uint32_t kDescStride = sizeof(SortDesc);
    const uint32_t elements = MaxElementsPerIteration(numSorts);
    CreateStructuredSrv(d, m_descBuffer.Get(), numSorts, kDescStride, cpu(kSlotSortDescs));
    CreateStructuredSrv(d, m_inputBuffer.Get(), elements, 4, cpu(kSlotSortInput));
    CreateStructuredUav(d, m_outputBuffer.Get(), elements, 4, cpu(kSlotSortOutput));
    CreateStructuredSrv(d, m_descBuffer.Get(), numSorts, kDescStride, cpu(kSlotFlushDescs));
    CreateStructuredSrv(d, m_inputBuffer.Get(), elements, 4, cpu(kSlotFlushInput));
    CreateStructuredSrv(d, m_warmDescBuffer.Get(), numSorts, kDescStride, cpu(kSlotWarmDescs));
    CreateStructuredSrv(d, m_warmInputBuffer.Get(), elements, 4, cpu(kSlotWarmInput));
    CreateStructuredUav(d, m_warmOutputBuffer.Get(), elements, 4, cpu(kSlotWarmOutput));

    // Indirect args {numSorts, 1, 1}.
    const D3D12_DISPATCH_ARGUMENTS args{numSorts, 1, 1};
    memcpy(m_argsUploadPtr, &args, sizeof(args));
    Frame& f = m_frames[0];
    CHECK_HR(f.allocator->Reset());
    CHECK_HR(f.list->Reset(f.allocator.Get(), nullptr));
    SetName(f.list.Get(), Format("set sort count %u", numSorts));
    f.list->CopyBufferRegion(m_argsBuffer.Get(), 0, m_argsUpload.Get(), 0, sizeof(args));
    CHECK_HR(f.list->Close());
    SubmitAndWait(f);
}

void GpuBenchmark::WaitForFence(uint64_t value)
{
    if (m_deviceLost)
        throw DeviceLostError("GPU device already lost");
    // A removed device reports every fence as complete (UINT64_MAX), so the removal check at the
    // end must run even when no wait was needed.
    if (m_fence->GetCompletedValue() < value)
    {
        CHECK_HR(m_fence->SetEventOnCompletion(value, m_fenceEvent));
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            const DWORD r = WaitForSingleObject(m_fenceEvent, 100);
            if (r == WAIT_OBJECT_0)
                break;
            if (r != WAIT_TIMEOUT)
            {
                m_deviceLost = true;
                throw DeviceLostError(Format("waiting for the fence event failed (%lu)", GetLastError()));
            }
            CheckDevice("while waiting for a fence");
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms >= m_options.fenceTimeoutMs)
            {
                m_deviceLost = true;
                throw DeviceLostError(Format("GPU did not reach fence %llu within %u ms (completed value %llu); "
                                             "treating the device as hung",
                                             static_cast<unsigned long long>(value), m_options.fenceTimeoutMs,
                                             static_cast<unsigned long long>(m_fence->GetCompletedValue())));
            }
        }
    }
    CheckDevice("after a fence wait");
}

void GpuBenchmark::Record(Frame& f, const std::vector<ComPtr<ID3D12PipelineState>>& psos, FlushMode flushMode,
                          const std::string& label)
{
    ID3D12GraphicsCommandList* cl = f.list.Get();
    CHECK_HR(f.allocator->Reset());
    CHECK_HR(cl->Reset(f.allocator.Get(), nullptr));
    SetName(cl, Format("%s iterations %u-%u", label.c_str(), f.firstIteration, f.firstIteration + f.count - 1));
    ID3D12DescriptorHeap* heaps[] = {m_descriptorHeap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(m_rootSignature.Get());
    auto marker = [&](const char* what, uint32_t s) {
        if (!m_options.markers)
            return;
        const std::wstring w = Utf8ToWide(Format("%s iter %u %s", label.c_str(), f.firstIteration + s, what));
        cl->SetMarker(0, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
    };

    ID3D12Resource* desc = m_descBuffer.Get();
    ID3D12Resource* input = m_inputBuffer.Get();
    ID3D12Resource* output = m_outputBuffer.Get();
    ID3D12Resource* warmDesc = m_warmDescBuffer.Get();
    ID3D12Resource* warmInput = m_warmInputBuffer.Get();
    ID3D12Resource* warmOutput = m_warmOutputBuffer.Get();
    const FlushKind kind = flushMode.kind;
    const bool warmRun = kind == FlushKind::Data; // the warm buffers are only touched in this mode
    const uint32_t spinIterations = flushMode.drain == DrainKind::Spin ? SpinIterations(flushMode.drainUs) : 0;
    const uint64_t descBytes = DescBytes(m_numSorts);
    const uint64_t descRegionBytes = DescRegionBytes(m_numSorts);
    const uint64_t uploadSlotBytes = UploadSlotBytes(m_numSorts);
    const uint64_t elementBytes = ElementBytes(m_numSorts);

    // The iterations' descriptors + elements into their upload slots (in parallel: up to 16 MB each
    // at 512 sorts, into write-combined memory).
    {
        std::vector<uint32_t> slots(f.count);
        std::iota(slots.begin(), slots.end(), 0u);
        std::for_each(std::execution::par, slots.begin(), slots.end(), [&](uint32_t s) {
            const IterationData& d = f.data[s];
            uint8_t* dst = f.uploadPtr + s * uploadSlotBytes;
            memcpy(dst, d.sorts.data(), descBytes);
            if (!d.elements.empty())
                memcpy(dst + descRegionBytes, d.elements.data(), d.elements.size() * sizeof(uint32_t));
        });
    }

    // Buffers decay to COMMON after every ExecuteCommandLists; start each list from there.
    {
        std::vector<D3D12_RESOURCE_BARRIER> b = {
            Transition(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(m_poisonBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(m_flushBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(m_drainBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(m_argsBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        };
        if (warmRun)
        {
            b.push_back(Transition(warmDesc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
            b.push_back(Transition(warmInput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
            b.push_back(Transition(warmOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }
        cl->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
    }

    const uint32_t sortConstants[4] = {m_numSorts, 0, 0, 0};
    const D3D12_RESOURCE_BARRIER uavBarrier = UavBarrier(nullptr);
    auto flush = [&](uint32_t s, bool readOnly) {
        const uint32_t flushConstants[4] = {kFlushElements, kFlushThreads, 0, 0};
        marker(readOnly ? "flush (read-only)" : "flush", s);
        cl->SetPipelineState(readOnly ? m_flushReadOnlyPso.Get() : m_flushPso.Get());
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, flushConstants, 0);
        cl->SetComputeRootDescriptorTable(kRootTable, m_flushTable);
        cl->Dispatch(kFlushGroups, 1, 1);
        cl->ResourceBarrier(1, &uavBarrier);
    };
    // Drain: one group of the flush shader on the private 4 KB drain buffer (its own descriptor table,
    // nothing the sort uses), then a UAV barrier. A driver may defer a barrier's wait and cache
    // maintenance to the next dispatch, which without this is the timed sort (see FlushMode).
    auto drainDispatch = [&](uint32_t s) {
        const uint32_t drainConstants[4] = {kDrainElements, kFlushGroupSize, 0, 0};
        marker("drain", s);
        cl->SetPipelineState(m_flushPso.Get());
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, drainConstants, 0);
        cl->SetComputeRootDescriptorTable(kRootTable, m_drainTable);
        cl->Dispatch(1, 1, 1);
        cl->ResourceBarrier(1, &uavBarrier);
    };
    // Spin drain: an ALU-only dispatch of about flushMode.drainUs on the same private buffer and
    // table, then a UAV barrier: the GPU waits a known time after the flush without memory traffic.
    auto spinDispatch = [&](uint32_t s) {
        const uint32_t spinConstants[4] = {spinIterations, 0, 0, 0};
        marker("drain (spin)", s);
        cl->SetPipelineState(m_spinPso.Get());
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, spinConstants, 0);
        cl->SetComputeRootDescriptorTable(kRootTable, m_drainTable);
        cl->Dispatch(kSpinGroups, 1, 1);
        cl->ResourceBarrier(1, &uavBarrier);
    };
    for (uint32_t s = 0; s < f.count; ++s)
    {
        const IterationData& d = f.data[s];
        const uint64_t bytes = d.elements.size() * sizeof(uint32_t);
        const uint64_t uploadOffset = s * uploadSlotBytes;

        // FlushKind::Code: flush first, so the uploaded data is the most recent write (warm in L2).
        if (kind == FlushKind::Code)
            flush(s, false);

        // 1) upload + poison the whole output (stray writes anywhere in it are detected)
        marker("upload", s);
        cl->CopyBufferRegion(desc, 0, f.upload.Get(), uploadOffset, descBytes);
        if (bytes)
            cl->CopyBufferRegion(input, 0, f.upload.Get(), uploadOffset + descRegionBytes, bytes);
        cl->CopyBufferRegion(output, 0, m_poisonBuffer.Get(), 0, elementBytes);
        if (warmRun)
        {
            cl->CopyBufferRegion(warmDesc, 0, f.upload.Get(), uploadOffset, descBytes);
            if (bytes)
                cl->CopyBufferRegion(warmInput, 0, f.upload.Get(), uploadOffset + descRegionBytes, bytes);
        }
        {
            std::vector<D3D12_RESOURCE_BARRIER> b = {
                Transition(desc, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState),
                Transition(input, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState),
                Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            if (warmRun)
            {
                b.push_back(Transition(warmDesc, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
                b.push_back(Transition(warmInput, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
            }
            cl->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
        }

        // 2) cache flush (FlushKind::Full, FullRo and Data)
        if (kind == FlushKind::Full || kind == FlushKind::Data)
            flush(s, false);
        else if (kind == FlushKind::FullRo)
            flush(s, true);

        // FlushKind::Data: untimed run of the same dispatches on the private copy, so the shader code
        // (and everything else except the sort's own buffers) is warm for the timed run.
        if (warmRun)
        {
            cl->SetComputeRoot32BitConstants(kRootConstants, 4, sortConstants, 0);
            cl->SetComputeRootDescriptorTable(kRootTable, m_warmTable);
            for (size_t p = 0; p < psos.size(); ++p)
            {
                marker(Format("warm-up dispatch %zu", p).c_str(), s);
                cl->SetPipelineState(psos[p].Get());
                cl->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_argsBuffer.Get(), 0, nullptr, 0);
            }
            cl->ResourceBarrier(1, &uavBarrier);
        }

        // 3) drain: everything recorded so far has executed before the timed window opens (and with
        //    the spin drain, a known time has passed since)
        if (flushMode.drain == DrainKind::Group)
            drainDispatch(s);
        else if (flushMode.drain == DrainKind::Spin)
            spinDispatch(s);

        // 4) timed sort
        cl->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * s);
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, sortConstants, 0);
        cl->SetComputeRootDescriptorTable(kRootTable, m_sortTable);
        for (size_t p = 0; p < psos.size(); ++p)
        {
            marker(Format("sort dispatch %zu", p).c_str(), s);
            cl->SetPipelineState(psos[p].Get());
            cl->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_argsBuffer.Get(), 0, nullptr, 0);
        }
        cl->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * s + 1);

        // 5) readback
        {
            std::vector<D3D12_RESOURCE_BARRIER> b = {
                Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(desc, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(input, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            if (warmRun)
            {
                b.push_back(Transition(warmDesc, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
                b.push_back(Transition(warmInput, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
            }
            cl->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
        }
        marker("readback", s);
        cl->CopyBufferRegion(f.readback.Get(), s * elementBytes, output, 0, elementBytes);
        {
            const D3D12_RESOURCE_BARRIER b =
                Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->ResourceBarrier(1, &b);
        }
    }

    cl->ResolveQueryData(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * f.count, f.timestampReadback.Get(),
                         0);
    {
        std::vector<D3D12_RESOURCE_BARRIER> b = {
            Transition(desc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_poisonBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_flushBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_drainBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_argsBuffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON),
        };
        if (warmRun)
        {
            b.push_back(Transition(warmDesc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
            b.push_back(Transition(warmInput, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
            b.push_back(Transition(warmOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
        }
        cl->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
    }
    CHECK_HR(cl->Close());
}

void GpuBenchmark::SubmitList(Frame& f)
{
    if (m_deviceLost)
        throw DeviceLostError("GPU device already lost");
    CheckDevice("before submitting");
    ID3D12CommandList* lists[] = {f.list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    CHECK_HR(m_queue->Signal(m_fence.Get(), ++m_fenceValue));
    f.fenceValue = m_fenceValue;
    f.pending = true;
}

void GpuBenchmark::Submit(Frame& f)
{
    SubmitList(f);

    if (m_options.testRemoveDevice && !m_removeRequested)
    {
        m_removeRequested = true;
        ComPtr<ID3D12Device5> device5;
        CHECK_HR(m_device.As(&device5));
        Log("  --test-device-removal: calling ID3D12Device5::RemoveDevice()\n");
        device5->RemoveDevice();
    }
}

double GpuBenchmark::Calibrate(uint32_t workloadId, uint32_t iterations)
{
    if (m_frames[0].pending || m_frames[1].pending)
        throw std::runtime_error("calibration: called while iterations are in flight");
    Frame& f = m_frames[0];
    const std::vector<ComPtr<ID3D12PipelineState>> noSort;
    // Generates outside the timed part (a run generates the next batch while the GPU works).
    auto batch = [&](uint32_t first, uint32_t count) -> double {
        f.firstIteration = first;
        f.count = count;
        std::vector<uint32_t> slots(count);
        std::iota(slots.begin(), slots.end(), 0u);
        std::for_each(std::execution::par, slots.begin(), slots.end(), [&](uint32_t s) {
            GenerateIteration(workloadId, kWarmupIterationBase + first + s, m_numSorts, f.data[s]);
        });
        const auto start = std::chrono::steady_clock::now();
        Record(f, noSort, FlushMode{FlushKind::Full, DrainKind::Group, 0}, "calibration");
        SubmitList(f);
        WaitForFence(f.fenceValue);
        f.pending = false;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    // Untimed first batch: first use of the buffers, clocks ramping up.
    batch(0, std::min(m_batchSize, std::max(1u, iterations / 4)));
    double seconds = 0.0;
    for (uint32_t done = 0; done < iterations;)
    {
        const uint32_t n = std::min(m_batchSize, iterations - done);
        seconds += batch(done, n);
        done += n;
    }
    return iterations ? seconds / iterations : 0.0;
}

uint32_t GpuBenchmark::SpinIterations(uint32_t us) const
{
    if (!(m_spinIterationsPerUs > 0.0))
        throw std::logic_error("spin drain used before its rate was set (CalibrateDrainSpin / SetDrainSpinRate)");
    const double n = std::round(static_cast<double>(us) * m_spinIterationsPerUs);
    return static_cast<uint32_t>(std::clamp(n, 1.0, static_cast<double>(kMaxSpinIterations)));
}

GpuBenchmark::SpinCalibration GpuBenchmark::CalibrateDrainSpin()
{
    if (m_frames[0].pending || m_frames[1].pending)
        throw std::runtime_error("spin calibration: called while iterations are in flight");
    Frame& f = m_frames[0];

    // One command list: kSpinReps x {timestamp, spin(1), barrier, timestamp, timestamp, spin(n),
    // barrier, timestamp}; returns the median durations (us) of spin(1) and spin(n).
    auto measure = [&](uint32_t n, double& oneUs, double& nUs) {
        ID3D12GraphicsCommandList* cl = f.list.Get();
        CHECK_HR(f.allocator->Reset());
        CHECK_HR(cl->Reset(f.allocator.Get(), nullptr));
        SetName(cl, Format("drain spin calibration (%u iterations)", n));
        ID3D12DescriptorHeap* heaps[] = {m_descriptorHeap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(m_rootSignature.Get());
        {
            const D3D12_RESOURCE_BARRIER b =
                Transition(m_drainBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->ResourceBarrier(1, &b);
        }
        const D3D12_RESOURCE_BARRIER uavBarrier = UavBarrier(nullptr);
        cl->SetPipelineState(m_spinPso.Get());
        cl->SetComputeRootDescriptorTable(kRootTable, m_drainTable);
        for (uint32_t r = 0; r < kSpinReps; ++r)
        {
            for (uint32_t k = 0; k < 2; ++k)
            {
                const uint32_t q = 4 * r + 2 * k;
                const uint32_t constants[4] = {k == 0 ? 1u : n, 0, 0, 0};
                cl->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
                cl->SetComputeRoot32BitConstants(kRootConstants, 4, constants, 0);
                cl->Dispatch(kSpinGroups, 1, 1);
                cl->ResourceBarrier(1, &uavBarrier);
                cl->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
            }
        }
        cl->ResolveQueryData(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4 * kSpinReps,
                             f.timestampReadback.Get(), 0);
        {
            const D3D12_RESOURCE_BARRIER b =
                Transition(m_drainBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            cl->ResourceBarrier(1, &b);
        }
        CHECK_HR(cl->Close());
        SubmitList(f);
        WaitForFence(f.fenceValue);
        f.pending = false;

        const D3D12_RANGE range{0, sizeof(uint64_t) * 4 * kSpinReps};
        uint64_t* ts = nullptr;
        CHECK_HR(f.timestampReadback->Map(0, &range, reinterpret_cast<void**>(&ts)));
        std::vector<double> one, many;
        const double toUs = 1e6 / static_cast<double>(m_timestampFrequency);
        for (uint32_t r = 0; r < kSpinReps; ++r)
        {
            one.push_back(static_cast<double>(ts[4 * r + 1] - ts[4 * r]) * toUs);
            many.push_back(static_cast<double>(ts[4 * r + 3] - ts[4 * r + 2]) * toUs);
        }
        const D3D12_RANGE noWrite{0, 0};
        f.timestampReadback->Unmap(0, &noWrite);
        auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            const size_t m = v.size() / 2;
            return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
        };
        oneUs = median(one);
        nUs = median(many);
    };

    SpinCalibration c;
    uint32_t n = 1024;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        double oneUs = 0.0, nUs = 0.0;
        measure(n, oneUs, nUs);
        c.iterations = n;
        c.overheadUs = oneUs;
        c.spanUs = nUs - oneUs;
        if (c.spanUs >= 50.0 || n >= kMaxSpinIterations)
            break;
        // Aim for ~100 us; at least double.
        const double scale = c.spanUs > 1.0 ? 100.0 / c.spanUs : 16.0;
        n = static_cast<uint32_t>(std::min<double>(kMaxSpinIterations, std::max(2.0 * n, n * scale)));
    }
    if (!(c.spanUs > 0.0))
        throw std::runtime_error(Format("spin calibration: no measurable time (%u iterations: %.3f us, 1 iteration: "
                                        "%.3f us)",
                                        c.iterations, c.spanUs + c.overheadUs, c.overheadUs));
    c.iterationsPerUs = static_cast<double>(c.iterations - 1) / c.spanUs;
    SetDrainSpinRate(c.iterationsPerUs);

    c.checkIterations = SpinIterations(kSpinCheckUs);
    double oneUs = 0.0;
    measure(c.checkIterations, oneUs, c.checkUs);
    return c;
}

void GpuBenchmark::Process(Frame& f, uint32_t warmup, ComboResult& result)
{
    WaitForFence(f.fenceValue);
    f.pending = false;

    const D3D12_RANGE tsRange{0, sizeof(uint64_t) * 2 * f.count};
    uint64_t* timestamps = nullptr;
    CHECK_HR(f.timestampReadback->Map(0, &tsRange, reinterpret_cast<void**>(&timestamps)));
    const uint64_t elementBytes = ElementBytes(m_numSorts);
    const D3D12_RANGE outRange{0, static_cast<SIZE_T>(elementBytes * f.count)};
    uint8_t* outputs = nullptr;
    CHECK_HR(f.readback->Map(0, &outRange, reinterpret_cast<void**>(&outputs)));

    // Verification in tasks of up to kVerifySortsPerTask sorts or kVerifyTailElementsPerTask tail
    // elements, all iterations of the batch at once; per iteration the first failing task (in the
    // order VerifyIteration checks) gives the message.
    const auto verifyStart = std::chrono::steady_clock::now();
    struct VerifyTask
    {
        uint32_t slot = 0;
        bool tail = false;
        uint32_t begin = 0, end = 0; // sorts, or tail elements
        bool ok = true;
        std::string message;
    };
    std::vector<VerifyTask> tasks;
    const uint32_t bufferElements = MaxElementsPerIteration(m_numSorts);
    for (uint32_t s = 0; s < f.count; ++s)
    {
        for (uint32_t b = 0; b < m_numSorts; b += kVerifySortsPerTask)
            tasks.push_back({s, false, b, std::min(m_numSorts, b + kVerifySortsPerTask)});
        const uint32_t tailBegin = static_cast<uint32_t>(f.data[s].elements.size());
        for (uint32_t b = tailBegin; b < bufferElements; b += kVerifyTailElementsPerTask)
            tasks.push_back({s, true, b, std::min(bufferElements, b + kVerifyTailElementsPerTask)});
    }
    std::for_each(std::execution::par, tasks.begin(), tasks.end(), [&](VerifyTask& t) {
        const uint32_t* out = reinterpret_cast<const uint32_t*>(outputs + t.slot * elementBytes);
        t.ok = t.tail ? VerifyTail(f.data[t.slot], out, t.begin, t.end, &t.message)
                      : VerifySorts(f.data[t.slot], out, t.begin, t.end, &t.message);
    });
    std::vector<std::string> messages(f.count);
    std::vector<char> ok(f.count, 1);
    for (const VerifyTask& t : tasks) // per slot: the sorts in order, then the tail in order
    {
        if (!t.ok && ok[t.slot])
        {
            ok[t.slot] = 0;
            messages[t.slot] = t.message;
        }
    }
    result.verifySeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - verifyStart).count();

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
            result.failedIterations.push_back(global < warmup ? kWarmupIterationBase + global : global - warmup);
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

std::vector<GpuBenchmark::ProbeOutput> GpuBenchmark::RunProbe(const std::vector<IDxcBlob*>& shaders,
                                                              uint32_t wordsPerDispatch)
{
    if (uint64_t(wordsPerDispatch) * shaders.size() > ElementViewCount())
        throw std::runtime_error("wave probe: too many probe dispatches for the output buffer");
    Frame& f = m_frames[0];
    if (f.pending || m_frames[1].pending)
        throw std::runtime_error("wave probe: called while iterations are in flight");

    std::vector<ProbeOutput> outputs(shaders.size());
    std::vector<ComPtr<ID3D12PipelineState>> psos(shaders.size());
    for (size_t i = 0; i < shaders.size(); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature.Get();
        desc.CS = {shaders[i]->GetBufferPointer(), shaders[i]->GetBufferSize()};
        const HRESULT hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&psos[i]));
        if (FAILED(hr))
        {
            psos[i].Reset();
            outputs[i].error = Format("CreateComputePipelineState failed (HRESULT 0x%08X)", static_cast<unsigned>(hr));
        }
        else
        {
            SetName(psos[i].Get(), Format("PSO wave probe %zu", i));
        }
    }
    CheckDevice("after creating the wave probe PSOs");

    ID3D12GraphicsCommandList* cl = f.list.Get();
    ID3D12Resource* output = m_outputBuffer.Get();
    CHECK_HR(f.allocator->Reset());
    CHECK_HR(cl->Reset(f.allocator.Get(), nullptr));
    SetName(cl, "wave probe");
    ID3D12DescriptorHeap* heaps[] = {m_descriptorHeap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(m_rootSignature.Get());
    {
        const D3D12_RESOURCE_BARRIER b[] = {
            Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(m_poisonBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cl->ResourceBarrier(_countof(b), b);
    }
    cl->CopyBufferRegion(output, 0, m_poisonBuffer.Get(), 0, ElementBytes(m_numSorts));
    {
        const D3D12_RESOURCE_BARRIER b =
            Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->ResourceBarrier(1, &b);
    }
    cl->SetComputeRootDescriptorTable(kRootTable, m_sortTable);
    for (size_t i = 0; i < shaders.size(); ++i)
    {
        if (!psos[i])
            continue;
        const uint32_t constants[4] = {static_cast<uint32_t>(i) * wordsPerDispatch, 0, 0, 0};
        if (m_options.markers)
        {
            const std::wstring w = Utf8ToWide(Format("wave probe dispatch %zu", i));
            cl->SetMarker(0, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
        }
        cl->SetPipelineState(psos[i].Get());
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, constants, 0);
        cl->Dispatch(1, 1, 1);
    }
    const uint64_t bytes = uint64_t(wordsPerDispatch) * shaders.size() * sizeof(uint32_t);
    {
        const D3D12_RESOURCE_BARRIER b =
            Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->ResourceBarrier(1, &b);
    }
    if (bytes)
        cl->CopyBufferRegion(f.readback.Get(), 0, output, 0, bytes);
    {
        const D3D12_RESOURCE_BARRIER b[] = {
            Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(m_poisonBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        };
        cl->ResourceBarrier(_countof(b), b);
    }
    CHECK_HR(cl->Close());

    if (m_deviceLost)
        throw DeviceLostError("GPU device already lost");
    CheckDevice("before submitting the wave probe");
    ID3D12CommandList* lists[] = {cl};
    m_queue->ExecuteCommandLists(1, lists);
    CHECK_HR(m_queue->Signal(m_fence.Get(), ++m_fenceValue));
    WaitForFence(m_fenceValue);

    if (bytes)
    {
        const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        uint32_t* words = nullptr;
        CHECK_HR(f.readback->Map(0, &range, reinterpret_cast<void**>(&words)));
        for (size_t i = 0; i < shaders.size(); ++i)
        {
            if (psos[i])
                outputs[i].words.assign(words + i * wordsPerDispatch, words + (i + 1) * wordsPerDispatch);
        }
        const D3D12_RANGE noWrite{0, 0};
        f.readback->Unmap(0, &noWrite);
    }
    return outputs;
}

void GpuBenchmark::Run(uint32_t workloadId, size_t algorithmIndex, uint32_t iterations, uint32_t warmup,
                       const std::string& label, const ProgressFn& progress, ComboResult& result)
{
    const auto start = std::chrono::steady_clock::now();
    result = ComboResult{};
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
            f.count = std::min(m_batchSize, total - next);
            const auto generateStart = std::chrono::steady_clock::now();
            std::vector<uint32_t> slots(f.count);
            std::iota(slots.begin(), slots.end(), 0u);
            std::for_each(std::execution::par, slots.begin(), slots.end(), [&](uint32_t s) {
                const uint32_t global = f.firstIteration + s;
                const uint32_t iteration = global < warmup ? kWarmupIterationBase + global : global - warmup;
                GenerateIteration(workloadId, iteration, m_numSorts, f.data[s]);
            });
            const auto recordStart = std::chrono::steady_clock::now();
            Record(f, m_algorithmPsos[algorithmIndex], m_algorithmFlush[algorithmIndex], label);
            result.generateSeconds += std::chrono::duration<double>(recordStart - generateStart).count();
            result.recordSeconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - recordStart).count();
            Submit(f);
            next += f.count;
            if (m_options.serial)
            {
                // Smoke mode: this iteration must finish before anything else is recorded, so a
                // fault is attributed to exactly this iteration.
                Process(f, warmup, result);
                if (progress)
                    progress(result.iterationsRun, total, result.failures);
            }
        }
        frameIndex = (frameIndex + 1) % kFrames;

        bool anyPending = false;
        for (const auto& fr : m_frames)
            anyPending |= fr.pending;
        if (next >= total && !anyPending)
            break;
    }

    result.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
