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

bool EnableD3D12DebugLayer()
{
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        return false;
    debug->EnableDebugLayer();
    return true;
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
