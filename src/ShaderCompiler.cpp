#include "ShaderCompiler.h"

#include <d3d12shader.h>

#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

namespace
{
// A copy of a DXC text output (errors, disassembly) with its explicit length (GetStringLength, which
// excludes the terminating NUL): nothing relies on the blob being NUL-terminated.
std::string BlobText(IDxcBlobUtf8* blob)
{
    if (!blob || !blob->GetStringPointer() || blob->GetStringLength() == 0)
        return {};
    return std::string(blob->GetStringPointer(), blob->GetStringLength());
}

// Byte size of the LLVM IR type at text[pos] (in groupshared: arrays / vectors of scalars, e.g.
// "[8192 x i32]", "[16 x [64 x float]]"); advances pos. -1 for anything else (e.g. a struct type).
// Every read is bounds-checked against text.size().
int64_t IrTypeBytes(std::string_view text, size_t& pos)
{
    auto at = [&](size_t i) { return i < text.size() ? text[i] : '\0'; };
    auto skipSpaces = [&] {
        while (at(pos) == ' ')
            ++pos;
    };
    skipSpaces();
    if (at(pos) == '[' || at(pos) == '<')
    {
        const char close = at(pos) == '[' ? ']' : '>';
        ++pos;
        if (!std::isdigit(static_cast<unsigned char>(at(pos))))
            return -1;
        int64_t n = 0;
        while (std::isdigit(static_cast<unsigned char>(at(pos))))
        {
            if (n > (int64_t(1) << 40))
                return -1; // absurd count: not a groupshared array
            n = n * 10 + (at(pos++) - '0');
        }
        skipSpaces();
        if (at(pos) != 'x')
            return -1;
        ++pos;
        const int64_t element = IrTypeBytes(text, pos);
        skipSpaces();
        if (element < 0 || at(pos) != close)
            return -1;
        ++pos;
        return n * element;
    }
    static const struct
    {
        std::string_view name;
        int64_t bytes;
    } kScalars[] = {{"i8", 1}, {"i16", 2}, {"i32", 4}, {"i64", 8}, {"half", 2}, {"float", 4}, {"double", 8}};
    const std::string_view rest = pos < text.size() ? text.substr(pos) : std::string_view();
    for (const auto& t : kScalars)
    {
        if (rest.substr(0, t.name.size()) == t.name &&
            !std::isalnum(static_cast<unsigned char>(at(pos + t.name.size()))))
        {
            pos += t.name.size();
            return t.bytes;
        }
    }
    return -1;
}

// Total groupshared bytes of a DXIL disassembly: the sizes of its addrspace(3) globals, e.g.
//   @<mangled gsP7 name> = external addrspace(3) global [8192 x i32], align 4
// -1 if one of them has a type IrTypeBytes does not know.
int64_t GroupsharedBytes(std::string_view text)
{
    constexpr std::string_view kMarker = " addrspace(3) global ";
    int64_t total = 0;
    size_t lineStart = 0;
    while (lineStart < text.size())
    {
        size_t eol = text.find('\n', lineStart);
        if (eol == std::string_view::npos)
            eol = text.size();
        const std::string_view line = text.substr(lineStart, eol - lineStart);
        if (!line.empty() && line.front() == '@')
        {
            const size_t at = line.find(kMarker);
            if (at != std::string_view::npos)
            {
                size_t pos = at + kMarker.size();
                const int64_t bytes = IrTypeBytes(line, pos);
                if (bytes < 0)
                    return -1;
                total += bytes;
            }
        }
        lineStart = eol + 1;
    }
    return total;
}
} // namespace

ShaderCompiler::ShaderCompiler() : m_threadId(GetCurrentThreadId())
{
    CHECK_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)));
    CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)));
    CHECK_HR(m_utils->CreateDefaultIncludeHandler(&m_includeHandler));
}

void ShaderCompiler::CheckThread() const
{
    if (GetCurrentThreadId() != m_threadId)
        throw std::logic_error("ShaderCompiler used from a thread other than the one that created it "
                               "(the DXC objects are not thread-safe)");
}

void ShaderCompiler::CheckHeaps(const char* what) const
{
    // Only the process heap: the CRT (static, Release) and DXC allocate from it. Other heaps in the
    // process (e.g. the driver's) may be HEAP_NO_SERIALIZE heaps in use on other threads.
    if (!HeapValidate(GetProcessHeap(), 0, nullptr))
        throw std::runtime_error(
            Format("heap corruption detected (HeapValidate failed on the process heap) after %s of %s", what,
                   m_current.c_str()));
}

ComPtr<IDxcBlob> ShaderCompiler::CompileFile(const std::filesystem::path& file, const std::string& entry,
                                             const ShaderDefines& defines, std::string& log)
{
    CheckThread();
    log.clear();
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(m_utils->LoadFile(file.c_str(), nullptr, &blob)) || !blob)
    {
        log = "cannot read " + file.string();
        return nullptr;
    }
    BOOL known = FALSE;
    UINT32 codePage = DXC_CP_ACP;
    if (FAILED(blob->GetEncoding(&known, &codePage)))
        known = FALSE;
    const DxcBuffer buffer{blob->GetBufferPointer(), blob->GetBufferSize(), known ? codePage : DXC_CP_ACP};
    return Compile(buffer, file.wstring(), entry, defines, file.parent_path(), log); // 'blob' outlives the call
}

ComPtr<IDxcBlob> ShaderCompiler::CompileSource(const std::string& source, const std::wstring& name,
                                               const std::string& entry, const ShaderDefines& defines,
                                               std::string& log)
{
    CheckThread();
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
    m_current = WideToUtf8(name) + ":" + entry;
    for (const auto& d : defines)
    {
        const std::string define = d.second.empty() ? d.first : d.first + "=" + d.second;
        args.push_back(L"-D");
        args.push_back(Utf8ToWide(define));
        m_current += " " + define;
    }
    std::vector<LPCWSTR> argv;
    for (const auto& a : args)
        argv.push_back(a.c_str()); // 'args' is not modified any more: the pointers stay valid

    ComPtr<IDxcBlob> object;
    {
        ComPtr<IDxcResult> result;
        CHECK_HR(m_compiler->Compile(&source, argv.data(), static_cast<UINT32>(argv.size()), m_includeHandler.Get(),
                                     IID_PPV_ARGS(&result)));
        if (!result)
            throw std::runtime_error("IDxcCompiler3::Compile returned no result for " + m_current);

        if (result->HasOutput(DXC_OUT_ERRORS))
        {
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)))
                log = BlobText(errors.Get());
        }

        HRESULT status = E_FAIL;
        if (FAILED(result->GetStatus(&status)))
            status = E_FAIL;
        if (SUCCEEDED(status) && result->HasOutput(DXC_OUT_OBJECT))
        {
            // GetOutput AddRefs the blob: 'object' holds its own reference and stays valid after
            // 'result' is released at the end of this scope.
            if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object ||
                !object->GetBufferPointer() || object->GetBufferSize() == 0)
                object.Reset();
        }
    }
    CheckHeaps("IDxcCompiler3::Compile");
    return object;
}

bool ShaderCompiler::UsesWaveOps(IDxcBlob* shader)
{
    CheckThread();
    if (!shader)
        return true;
    bool uses = true; // conservative if the reflection cannot be read
    {
        const DxcBuffer buffer{shader->GetBufferPointer(), shader->GetBufferSize(), 0};
        ComPtr<ID3D12ShaderReflection> reflection;
        if (SUCCEEDED(m_utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection))) && reflection)
            uses = (reflection->GetRequiresFlags() & D3D_SHADER_REQUIRES_WAVE_OPS) != 0;
    }
    CheckHeaps("IDxcUtils::CreateReflection");
    return uses;
}

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
    CheckThread();
    DispatchStats stats;
    if (!shader)
        return stats;
    const DxcBuffer buffer{shader->GetBufferPointer(), shader->GetBufferSize(), 0};
    {
        ComPtr<ID3D12ShaderReflection> reflection;
        if (SUCCEEDED(m_utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection))) && reflection)
        {
            UINT x = 0, y = 0, z = 0;
            reflection->GetThreadGroupSize(&x, &y, &z);
            stats.threads = x * y * z;
        }
    }
    CheckHeaps("IDxcUtils::CreateReflection");

    // The disassembly is copied into a std::string (explicit length) and every DXC object of the
    // disassembly is released before it is parsed.
    std::string text;
    {
        ComPtr<IDxcResult> result;
        if (SUCCEEDED(m_compiler->Disassemble(&buffer, IID_PPV_ARGS(&result))) && result)
        {
            HRESULT status = E_FAIL;
            if (SUCCEEDED(result->GetStatus(&status)) && SUCCEEDED(status) && result->HasOutput(DXC_OUT_DISASSEMBLY))
            {
                ComPtr<IDxcBlobUtf8> disassembly;
                if (SUCCEEDED(result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&disassembly), nullptr)))
                    text = BlobText(disassembly.Get());
            }
        }
    }
    CheckHeaps("IDxcCompiler3::Disassemble");
    if (!text.empty())
        stats.groupsharedBytes = GroupsharedBytes(text);
    return stats;
}
