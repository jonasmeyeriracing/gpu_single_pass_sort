#pragma once

#include "Algorithms.h"
#include "Common.h"

#include <dxcapi.h>

#include <filesystem>
#include <string>

// Runtime HLSL compilation via IDxcCompiler3 (target cs_6_6, -O3).
class ShaderCompiler
{
public:
    ShaderCompiler();

    // Returns null on failure. 'log' receives compiler errors/warnings (may be non-empty on success).
    ComPtr<IDxcBlob> CompileFile(const std::filesystem::path& file, const std::string& entry,
                                 const ShaderDefines& defines, std::string& log);
    ComPtr<IDxcBlob> CompileSource(const std::string& source, const std::wstring& name, const std::string& entry,
                                   const ShaderDefines& defines, std::string& log);

    // True if the compiled shader uses wave intrinsics (DXIL shader flag "wave ops", from the
    // container's reflection). Also true if the reflection cannot be read (conservative).
    bool UsesWaveOps(IDxcBlob* shader);

private:
    ComPtr<IDxcBlob> Compile(const DxcBuffer& source, const std::wstring& name, const std::string& entry,
                             const ShaderDefines& defines, const std::filesystem::path& includeDir, std::string& log);

    ComPtr<IDxcUtils> m_utils;
    ComPtr<IDxcCompiler3> m_compiler;
    ComPtr<IDxcIncludeHandler> m_includeHandler;
};
