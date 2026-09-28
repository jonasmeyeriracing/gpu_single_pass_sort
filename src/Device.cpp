#include "Device.h"

#include <winternl.h>
#include <d3dkmthk.h>

#include <algorithm>
#include <cctype>

namespace
{
struct KmtAdapterInfo
{
    bool valid = false;
    D3DKMT_ADAPTERTYPE type{};
    D3DKMT_ADAPTERADDRESS address{};
};

KmtAdapterInfo QueryKmtAdapter(LUID luid)
{
    KmtAdapterInfo info;
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0)
        return info;
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = open.hAdapter;
    q.Type = KMTQAITYPE_ADAPTERTYPE;
    q.pPrivateDriverData = &info.type;
    q.PrivateDriverDataSize = sizeof(info.type);
    const bool typeOk = D3DKMTQueryAdapterInfo(&q) == 0;
    q.Type = KMTQAITYPE_ADAPTERADDRESS;
    q.pPrivateDriverData = &info.address;
    q.PrivateDriverDataSize = sizeof(info.address);
    const bool addrOk = D3DKMTQueryAdapterInfo(&q) == 0;
    info.valid = typeOk && addrOk;
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
    return info;
}
} // namespace

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string DriverVersion(IDXGIAdapter1* adapter)
{
    LARGE_INTEGER v{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
        return "unknown";
    return Format("%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
}

bool EnableD3D12DebugLayer(bool gpuBasedValidation)
{
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        return false;
    debug->EnableDebugLayer();
    if (gpuBasedValidation)
    {
        ComPtr<ID3D12Debug1> debug1;
        if (FAILED(debug.As(&debug1)))
            return false;
        debug1->SetEnableGPUBasedValidation(TRUE);
    }
    return true;
}

bool EnableDred()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
        return false;
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    return true;
}

namespace
{
const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
    case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return "SetPipelineState1";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
    case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST: return "BeginCommandList";
    default: return nullptr;
    }
}

std::string OpString(D3D12_AUTO_BREADCRUMB_OP op)
{
    const char* name = BreadcrumbOpName(op);
    return name ? name : Format("op#%d", static_cast<int>(op));
}

const char* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
{
    switch (type)
    {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "CommandQueue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PipelineState";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "CommandList";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "Fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DescriptorHeap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "Heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QueryHeap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "CommandSignature";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "Resource";
    default: return "other";
    }
}

std::string NodeName(const char* a, const wchar_t* w)
{
    if (w)
        return WideToUtf8(w);
    if (a)
        return a;
    return "(unnamed)";
}

void AppendAllocations(std::string& out, const char* title, const D3D12_DRED_ALLOCATION_NODE1* node)
{
    out += Format("  %s:\n", title);
    if (!node)
        out += "    (none)\n";
    for (; node; node = node->pNext)
        out += Format("    %-16s %s\n", AllocationTypeName(node->AllocationType),
                      NodeName(node->ObjectNameA, node->ObjectNameW).c_str());
}

// Context string (SetMarker text) recorded exactly at breadcrumb index 'index', or empty.
std::string ContextAt(const D3D12_AUTO_BREADCRUMB_NODE1& n, UINT index)
{
    for (UINT c = 0; c < n.BreadcrumbContextsCount; ++c)
    {
        if (n.pBreadcrumbContexts[c].BreadcrumbIndex == index && n.pBreadcrumbContexts[c].pContextString)
            return WideToUtf8(n.pBreadcrumbContexts[c].pContextString);
    }
    return {};
}
} // namespace

std::string FormatDredReport(ID3D12Device* device)
{
    std::string out = "DRED report\n";
    out += Format("  device removed reason: 0x%08X\n", static_cast<unsigned>(device->GetDeviceRemovedReason()));

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return out + "  DRED data not available (run with --dred)\n";

    ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
    if (SUCCEEDED(dred.As(&dred2)))
    {
        const D3D12_DRED_DEVICE_STATE st = dred2->GetDeviceState();
        const char* name = st == D3D12_DRED_DEVICE_STATE_HUNG        ? "hung"
                           : st == D3D12_DRED_DEVICE_STATE_FAULT     ? "fault"
                           : st == D3D12_DRED_DEVICE_STATE_PAGEFAULT ? "page fault"
                                                                     : "unknown";
        out += Format("  device state: %s (%d)\n", name, static_cast<int>(st));
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    const HRESULT hrCrumbs = dred->GetAutoBreadcrumbsOutput1(&crumbs);
    if (FAILED(hrCrumbs))
        out += Format("  auto-breadcrumbs: not available (0x%08X)\n", static_cast<unsigned>(hrCrumbs));
    else
    {
        out += "  auto-breadcrumbs (command lists still tracked by DRED):\n";
        if (!crumbs.pHeadAutoBreadcrumbNode)
            out += "    (none)\n";
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
        {
            const UINT count = n->BreadcrumbCount;
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            const std::string list = NodeName(n->pCommandListDebugNameA, n->pCommandListDebugNameW);
            const std::string queue = NodeName(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW);
            if (done >= count)
            {
                out += Format("    [complete]   list '%s' on queue '%s': all %u ops completed\n", list.c_str(),
                              queue.c_str(), count);
                continue;
            }
            out += Format("    [INCOMPLETE] list '%s' on queue '%s': %u of %u ops completed\n", list.c_str(),
                          queue.c_str(), done, count);
            // Latest marker at or before the first incomplete op identifies the iteration / dispatch.
            const wchar_t* lastContext = nullptr;
            UINT lastContextIndex = 0;
            for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c)
            {
                const auto& ctx = n->pBreadcrumbContexts[c];
                if (ctx.pContextString && ctx.BreadcrumbIndex <= done &&
                    (!lastContext || ctx.BreadcrumbIndex >= lastContextIndex))
                {
                    lastContext = ctx.pContextString;
                    lastContextIndex = ctx.BreadcrumbIndex;
                }
            }
            if (lastContext)
                out += Format("      last marker at or before the first incomplete op (op %u): %s\n",
                              lastContextIndex, WideToUtf8(lastContext).c_str());
            const UINT first = done > 8 ? done - 8 : 0;
            const UINT last = std::min(count, done + 4);
            for (UINT i = first; i < last; ++i)
            {
                const std::string ctx = ContextAt(*n, i);
                const char* mark = i + 1 == done ? "<- last completed" : i == done ? "<- first NOT completed" : "";
                out += Format("      %6u %-18s %-24s%s%s%s\n", i, OpString(n->pCommandHistory[i]).c_str(), mark,
                              ctx.empty() ? "" : "\"", ctx.c_str(), ctx.empty() ? "" : "\"");
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    const HRESULT hrFault = dred->GetPageFaultAllocationOutput1(&fault);
    if (FAILED(hrFault))
        out += Format("  page fault: not available (0x%08X)\n", static_cast<unsigned>(hrFault));
    else if (fault.PageFaultVA == 0 && !fault.pHeadExistingAllocationNode && !fault.pHeadRecentFreedAllocationNode)
        out += "  page fault: none reported\n";
    else
    {
        out += Format("  page fault VA: 0x%016llX\n", static_cast<unsigned long long>(fault.PageFaultVA));
        AppendAllocations(out, "existing allocations at that VA", fault.pHeadExistingAllocationNode);
        AppendAllocations(out, "recently freed allocations at that VA", fault.pHeadRecentFreedAllocationNode);
    }
    return out;
}

std::vector<std::string> DrainDebugMessages(ID3D12Device* device)
{
    std::vector<std::string> result;
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return result;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)) || size == 0)
            continue;
        std::vector<uint8_t> buf(size);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(queue->GetMessage(i, msg, &size)))
        {
            static const char* severities[] = {"CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE"};
            const char* sev = msg->Severity <= D3D12_MESSAGE_SEVERITY_MESSAGE ? severities[msg->Severity] : "?";
            result.push_back(Format("D3D12 %s: %s", sev, msg->pDescription));
        }
    }
    queue->ClearStoredMessages();
    return result;
}

void PrintAdapterList()
{
    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d{};
        CHECK_HR(adapter->GetDesc1(&d));
        Log("[%u] %s  LUID %08X:%08X  vendor %04X device %04X subsys %08X rev %u  flags 0x%X  "
            "vram %llu MB  sysmem %llu MB  shared %llu MB\n",
            i, WideToUtf8(d.Description).c_str(), static_cast<unsigned>(d.AdapterLuid.HighPart),
            static_cast<unsigned>(d.AdapterLuid.LowPart), d.VendorId, d.DeviceId, d.SubSysId, d.Revision,
            d.Flags, static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20),
            static_cast<unsigned long long>(d.DedicatedSystemMemory >> 20),
            static_cast<unsigned long long>(d.SharedSystemMemory >> 20));
        ComPtr<IDXGIAdapter4> adapter4;
        if (SUCCEEDED(adapter.As(&adapter4)))
        {
            DXGI_ADAPTER_DESC3 d3{};
            if (SUCCEEDED(adapter4->GetDesc3(&d3)))
                Log("      desc3 flags 0x%X\n", static_cast<unsigned>(d3.Flags));
        }
        const KmtAdapterInfo kmt = QueryKmtAdapter(d.AdapterLuid);
        if (kmt.valid)
            Log("      KMT type 0x%X (render %u display %u software %u indirectDisplay %u)  PCI %u:%u.%u\n",
                kmt.type.Value, kmt.type.RenderSupported, kmt.type.DisplaySupported, kmt.type.SoftwareDevice,
                kmt.type.IndirectDisplayDevice, kmt.address.BusNumber, kmt.address.DeviceNumber,
                kmt.address.FunctionNumber);
        else
            Log("      KMT query failed\n");
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            const LUID luid = device->GetAdapterLuid();
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
            device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
            Log("      D3D12 device LUID %08X:%08X  highest SM 0x%X  nodes %u\n", static_cast<unsigned>(luid.HighPart),
                static_cast<unsigned>(luid.LowPart), static_cast<unsigned>(sm.HighestShaderModel),
                device->GetNodeCount());
        }
        else
        {
            Log("      D3D12CreateDevice failed\n");
        }
    }
}

// Returns an empty string if the adapter qualifies, else the reason it was skipped.
static std::string TryCreate(IDXGIAdapter1* adapter, GpuInfo& info)
{
    DXGI_ADAPTER_DESC1 desc{};
    CHECK_HR(adapter->GetDesc1(&desc));
    info.adapter = adapter;
    info.name = WideToUtf8(desc.Description);
    info.driver = DriverVersion(adapter);
    info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
    info.isWarp = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&info.device))))
        return "D3D12CreateDevice failed";

    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(info.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
    {
        info.device.Reset();
        return "shader model 6.6 not supported";
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (FAILED(info.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) ||
        !options1.WaveOps || options1.WaveLaneCountMin == 0)
    {
        info.device.Reset();
        return "wave operations not supported";
    }
    info.waveLaneCountMin = options1.WaveLaneCountMin;
    info.waveLaneCountMax = options1.WaveLaneCountMax;
    return {};
}

std::vector<GpuInfo> EnumerateGpus(bool warp, const std::string& filter, std::vector<std::string>& skipped)
{
    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    std::vector<GpuInfo> result;
    if (warp)
    {
        ComPtr<IDXGIAdapter1> adapter;
        CHECK_HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        GpuInfo info;
        const std::string reason = TryCreate(adapter.Get(), info);
        if (reason.empty())
            result.push_back(std::move(info));
        else
            skipped.push_back(info.name + ": " + reason);
        return result;
    }

    const std::string lowerFilter = ToLower(filter);
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        CHECK_HR(adapter->GetDesc1(&desc));
        const std::string name = WideToUtf8(desc.Description);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            skipped.push_back(name + ": software adapter");
            continue;
        }
        // Indirect display adapters (e.g. the Parsec Virtual Display Adapter) show up under the
        // name of the GPU that renders for them, and D3D12 device creation succeeds, but they are
        // not a separate GPU. Skip anything the kernel says is IDD or not render-capable.
        const KmtAdapterInfo kmt = QueryKmtAdapter(desc.AdapterLuid);
        if (kmt.valid && (kmt.type.IndirectDisplayDevice || !kmt.type.RenderSupported))
        {
            skipped.push_back(name + Format(" (LUID %08X:%08X): indirect display / non-render adapter",
                                            static_cast<unsigned>(desc.AdapterLuid.HighPart),
                                            static_cast<unsigned>(desc.AdapterLuid.LowPart)));
            continue;
        }
        GpuInfo info;
        const std::string reason = TryCreate(adapter.Get(), info);
        if (!reason.empty())
        {
            skipped.push_back(name + ": " + reason);
            continue;
        }
        if (!lowerFilter.empty() && ToLower(name).find(lowerFilter) == std::string::npos)
        {
            skipped.push_back(name + ": excluded by --gpu filter");
            continue;
        }
        result.push_back(std::move(info));
    }
    return result;
}
