#include "ShaderCompiler.h"

#include <d3d12shader.h>

#include <cctype>
#include <cstring>
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

namespace
{
// Byte size of an LLVM IR type at 'p' (in groupshared: arrays / vectors of scalars, e.g.
// "[8192 x i32]", "[16 x [64 x float]]"); advances p. -1 for anything else (e.g. a struct type).
int64_t IrTypeBytes(const char*& p)
{
    while (*p == ' ')
        ++p;
    if (*p == '[' || *p == '<')
    {
        const char close = *p == '[' ? ']' : '>';
        ++p;
        int64_t n = 0;
        if (!std::isdigit(static_cast<unsigned char>(*p)))
            return -1;
        while (std::isdigit(static_cast<unsigned char>(*p)))
            n = n * 10 + (*p++ - '0');
        while (*p == ' ')
            ++p;
        if (*p != 'x')
            return -1;
        ++p;
        const int64_t element = IrTypeBytes(p);
        while (*p == ' ')
            ++p;
        if (element < 0 || *p != close)
            return -1;
        ++p;
        return n * element;
    }
    static const struct
    {
        const char* name;
        int64_t bytes;
    } kScalars[] = {{"i8", 1}, {"i16", 2}, {"i32", 4}, {"i64", 8}, {"half", 2}, {"float", 4}, {"double", 8}};
    for (const auto& t : kScalars)
    {
        const size_t len = strlen(t.name);
        if (strncmp(p, t.name, len) == 0 && !std::isalnum(static_cast<unsigned char>(p[len])))
        {
            p += len;
            return t.bytes;
        }
    }
    return -1;
}
} // namespace

std::string FormatDispatchStats(const std::vector<DispatchStats>& stats)
{
    std::string out;
    for (const auto& s : stats)
    {
        if (!out.empty())
            out += ';';
        out += s.threads ? std::to_string(s.threads) : std::string("?");
        out += "t/";
        out += s.groupsharedBytes >= 0 ? std::to_string(s.groupsharedBytes) : std::string("?");
        out += 'B';
    }
    return out;
}

DispatchStats ShaderCompiler::GetDispatchStats(IDxcBlob* shader)
{
    DispatchStats stats;
    const DxcBuffer buffer{shader->GetBufferPointer(), shader->GetBufferSize(), 0};
    ComPtr<ID3D12ShaderReflection> reflection;
    if (SUCCEEDED(m_utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection))) && reflection)
    {
        UINT x = 0, y = 0, z = 0;
        reflection->GetThreadGroupSize(&x, &y, &z);
        stats.threads = x * y * z;
    }

    // Groupshared variables are the addrspace(3) globals of the DXIL module, e.g.
    //   @<mangled gsP7 name> = external addrspace(3) global [8192 x i32], align 4
    ComPtr<IDxcResult> result;
    ComPtr<IDxcBlobUtf8> text;
    if (FAILED(m_compiler->Disassemble(&buffer, IID_PPV_ARGS(&result))) || !result ||
        FAILED(result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&text), nullptr)) || !text)
        return stats;
    const char* s = text->GetStringPointer();
    const char* end = s + text->GetStringLength();
    int64_t total = 0;
    for (const char* line = s; line < end;)
    {
        const char* eol = static_cast<const char*>(memchr(line, '\n', static_cast<size_t>(end - line)));
        if (!eol)
            eol = end;
        if (*line == '@')
        {
            const std::string l(line, eol);
            const size_t at = l.find(" addrspace(3) global ");
            if (at != std::string::npos)
            {
                const char* p = l.c_str() + at + strlen(" addrspace(3) global ");
                const int64_t bytes = IrTypeBytes(p);
                if (bytes < 0)
                    return stats; // unknown type: groupsharedBytes stays -1
                total += bytes;
            }
        }
        line = eol + 1;
    }
    stats.groupsharedBytes = total;
    return stats;
}
