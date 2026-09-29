#pragma once

#include "Algorithms.h"
#include "Common.h"

#include <dxcapi.h>

#include <filesystem>
#include <string>
#include <vector>

// Occupancy inputs of a compiled compute shader (ShaderCompiler::GetDispatchStats).
struct DispatchStats
{
    uint32_t threads = 0;          // threads per group (numthreads x * y * z; 0 = reflection unavailable)
    int64_t groupsharedBytes = -1; // total groupshared (addrspace(3)) bytes declared by the DXIL; -1 = unknown
};

// "1024t/32768B" ("?" for an unknown value); several dispatches joined with ';'.
std::string FormatDispatchStats(const std::vector<DispatchStats>& stats);

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

    // Threads per group (reflection) and groupshared bytes (the sizes of the addrspace(3) globals in
    // the DXIL disassembly) of a compiled shader.
    DispatchStats GetDispatchStats(IDxcBlob* shader);

private:
    ComPtr<IDxcBlob> Compile(const DxcBuffer& source, const std::wstring& name, const std::string& entry,
                             const ShaderDefines& defines, const std::filesystem::path& includeDir, std::string& log);

    ComPtr<IDxcUtils> m_utils;
    ComPtr<IDxcCompiler3> m_compiler;
    ComPtr<IDxcIncludeHandler> m_includeHandler;
};
