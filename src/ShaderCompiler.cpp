#include "ShaderCompiler.h"

#include <d3d12shader.h>

#include <vector>

ShaderCompiler::ShaderCompiler()
{
    CHECK_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)));
    CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)));
    CHECK_HR(m_utils->CreateDefaultIncludeHandler(&m_includeHandler));
}

ComPtr<IDxcBlob> ShaderCompiler::CompileFile(const std::filesystem::path& file, const std::string& entry,
                                             const ShaderDefines& defines, std::string& log)
{
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(m_utils->LoadFile(file.c_str(), nullptr, &blob)))
    {
        log = "cannot read " + file.string();
        return nullptr;
    }
    BOOL known = FALSE;
    UINT32 codePage = DXC_CP_ACP;
    blob->GetEncoding(&known, &codePage);
    DxcBuffer buffer{blob->GetBufferPointer(), blob->GetBufferSize(), known ? codePage : DXC_CP_ACP};
    return Compile(buffer, file.wstring(), entry, defines, file.parent_path(), log);
}

ComPtr<IDxcBlob> ShaderCompiler::CompileSource(const std::string& source, const std::wstring& name,
                                               const std::string& entry, const ShaderDefines& defines,
                                               std::string& log)
{
    DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    return Compile(buffer, name, entry, defines, {}, log);
}

ComPtr<IDxcBlob> ShaderCompiler::Compile(const DxcBuffer& source, const std::wstring& name, const std::string& entry,
                                         const ShaderDefines& defines, const std::filesystem::path& includeDir,
                                         std::string& log)
{
    log.clear();
    std::vector<std::wstring> args = {name, L"-T", L"cs_6_6", L"-E", Utf8ToWide(entry), L"-O3", L"-HV", L"2021"};
    if (!includeDir.empty())
    {
        args.push_back(L"-I");
        args.push_back(includeDir.wstring());
    }
    for (const auto& d : defines)
    {
        args.push_back(L"-D");
        args.push_back(Utf8ToWide(d.second.empty() ? d.first : d.first + "=" + d.second));
    }
    std::vector<LPCWSTR> argv;
    for (const auto& a : args)
        argv.push_back(a.c_str());

    ComPtr<IDxcResult> result;
    CHECK_HR(m_compiler->Compile(&source, argv.data(), static_cast<UINT32>(argv.size()), m_includeHandler.Get(),
                                 IID_PPV_ARGS(&result)));

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors &&
        errors->GetStringLength() > 0)
        log = errors->GetStringPointer();

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status))
        return nullptr;

    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object ||
        object->GetBufferSize() == 0)
        return nullptr;
    return object;
}

bool ShaderCompiler::UsesWaveOps(IDxcBlob* shader)
{
    const DxcBuffer buffer{shader->GetBufferPointer(), shader->GetBufferSize(), 0};
    ComPtr<ID3D12ShaderReflection> reflection;
    if (FAILED(m_utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection))) || !reflection)
        return true;
    return (reflection->GetRequiresFlags() & D3D_SHADER_REQUIRES_WAVE_OPS) != 0;
}
