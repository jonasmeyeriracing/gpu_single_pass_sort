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
    kRootTable = 1,     // descriptor table: t0 StructuredBuffer<uint2> {offset, count}, t1 StructuredBuffer<uint>
                        //                   input, u0 RWStructuredBuffer<uint> output (flush: <uint4> buffer)
    kRootParamCount
};

// Descriptor heap layout: two 3-descriptor tables {t0, t1, u0}.
enum DescriptorSlot : UINT
{
    kSlotSortDescs = 0,
    kSlotSortInput = 1,
    kSlotSortOutput = 2,
    kSlotFlushDescs = 3,
    kSlotFlushInput = 4,
    kSlotFlushBuffer = 5,
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

GpuBenchmark::GpuBenchmark(ID3D12Device* device, IDxcBlob* flushShader,
                           const std::vector<CompiledAlgorithm>& algorithms, const BenchmarkOptions& options)
    : m_options(options), m_device(device)
{
    m_batchSize = m_options.serial ? 1 : kBatchSize;

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
    for (const auto& algorithm : algorithms)
    {
        std::vector<ComPtr<ID3D12PipelineState>> psos;
        for (const auto& shader : algorithm.shaders)
        {
            psos.push_back(CreateComputePso(m_device.Get(), m_rootSignature.Get(), shader.Get()));
            SetName(psos.back().Get(), Format("PSO %s dispatch %zu", algorithm.name.c_str(), psos.size() - 1));
        }
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
    SetName(m_descBuffer.Get(), "buffer sortDescs (t0)");
    SetName(m_inputBuffer.Get(), "buffer sortInput (t1)");
    SetName(m_outputBuffer.Get(), "buffer sortOutput (u0)");
    SetName(m_poisonBuffer.Get(), "buffer poison");
    SetName(m_flushBuffer.Get(), "buffer flush256MB (flush u0)");
    SetName(m_argsBuffer.Get(), "buffer indirectArgs");

    // Structured buffer views with exact sizes: 20 sort descriptors, the full element buffers and
    // the full flush buffer. Accesses past NumElements read 0 / are dropped.
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
        static_assert(sizeof(SortDesc) == 8, "must match StructuredBuffer<uint2>");
        constexpr uint32_t kDescStride = sizeof(SortDesc);
        CreateStructuredSrv(d, m_descBuffer.Get(), kSortsPerIteration, kDescStride, cpu(kSlotSortDescs));
        CreateStructuredSrv(d, m_inputBuffer.Get(), kMaxElementsPerIteration, 4, cpu(kSlotSortInput));
        CreateStructuredUav(d, m_outputBuffer.Get(), kMaxElementsPerIteration, 4, cpu(kSlotSortOutput));
        CreateStructuredSrv(d, m_descBuffer.Get(), kSortsPerIteration, kDescStride, cpu(kSlotFlushDescs));
        CreateStructuredSrv(d, m_inputBuffer.Get(), kMaxElementsPerIteration, 4, cpu(kSlotFlushInput));
        CreateStructuredUav(d, m_flushBuffer.Get(), kFlushElements, 16, cpu(kSlotFlushBuffer));
        m_sortTable = {gpu0.ptr + UINT64(kSlotSortDescs) * inc};
        m_flushTable = {gpu0.ptr + UINT64(kSlotFlushDescs) * inc};
    }

    for (uint32_t fi = 0; fi < kFrames; ++fi)
    {
        Frame& f = m_frames[fi];
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
        SetName(f.allocator.Get(), Format("frame%u allocator", fi));
        SetName(f.list.Get(), Format("frame%u list", fi));
        SetName(f.upload.Get(), Format("frame%u upload", fi));
        SetName(f.readback.Get(), Format("frame%u readback", fi));
        SetName(f.timestampReadback.Get(), Format("frame%u timestampReadback", fi));
        SetName(f.queryHeap.Get(), Format("frame%u timestamp queries", fi));
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

    if (m_options.logAddresses)
    {
        const std::pair<const char*, ID3D12Resource*> buffers[] = {
            {"sortDescs", m_descBuffer.Get()},
            {"sortInput", m_inputBuffer.Get()},
            {"sortOutput", m_outputBuffer.Get()},
            {"poison", m_poisonBuffer.Get()},
            {"flush256MB", m_flushBuffer.Get()},
            {"indirectArgs", m_argsBuffer.Get()},
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

void GpuBenchmark::Record(Frame& f, size_t algorithmIndex, const std::string& label)
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

        // 1) upload + poison the whole output (stray writes anywhere in it are detected)
        marker("upload", s);
        cl->CopyBufferRegion(desc, 0, f.upload.Get(), uploadOffset, kDescBytes);
        if (bytes)
            cl->CopyBufferRegion(input, 0, f.upload.Get(), uploadOffset + kDescRegionBytes, bytes);
        cl->CopyBufferRegion(output, 0, m_poisonBuffer.Get(), 0, kElementBytes);
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
        marker("flush", s);
        cl->SetPipelineState(m_flushPso.Get());
        cl->SetComputeRoot32BitConstants(kRootConstants, 4, flushConstants, 0);
        cl->SetComputeRootDescriptorTable(kRootTable, m_flushTable);
        cl->Dispatch(kFlushGroups, 1, 1);
        {
            const D3D12_RESOURCE_BARRIER b = UavBarrier(nullptr);
            cl->ResourceBarrier(1, &b);
        }

        // 3) timed sort
        const uint32_t sortConstants[4] = {kSortsPerIteration, 0, 0, 0};
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

        // 4) readback
        {
            const D3D12_RESOURCE_BARRIER b[] = {
                Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(desc, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(input, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cl->ResourceBarrier(_countof(b), b);
        }
        marker("readback", s);
        cl->CopyBufferRegion(f.readback.Get(), s * kElementBytes, output, 0, kElementBytes);
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
    if (m_deviceLost)
        throw DeviceLostError("GPU device already lost");
    CheckDevice("before submitting");
    ID3D12CommandList* lists[] = {f.list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    CHECK_HR(m_queue->Signal(m_fence.Get(), ++m_fenceValue));
    f.fenceValue = m_fenceValue;
    f.pending = true;

    if (m_options.testRemoveDevice && !m_removeRequested)
    {
        m_removeRequested = true;
        ComPtr<ID3D12Device5> device5;
        CHECK_HR(m_device.As(&device5));
        Log("  --test-device-removal: calling ID3D12Device5::RemoveDevice()\n");
        device5->RemoveDevice();
    }
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
            std::vector<uint32_t> slots(f.count);
            std::iota(slots.begin(), slots.end(), 0u);
            std::for_each(std::execution::par, slots.begin(), slots.end(), [&](uint32_t s) {
                const uint32_t global = f.firstIteration + s;
                const uint32_t iteration = global < warmup ? kWarmupIterationBase + global : global - warmup;
                GenerateIteration(workloadId, iteration, f.data[s]);
            });
            Record(f, algorithmIndex, label);
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
