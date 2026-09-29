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
//
// The DXC objects are not thread-safe: a ShaderCompiler must only be used on the thread that created
// it (every method checks this and throws otherwise). Every DXC output is copied out (compiler log,
// disassembly: with its explicit length, never as a C string) and the DXC result objects are
// released before the method returns; only the compiled DXIL blob itself is handed out (one
// reference owned by the returned ComPtr). After every DXC call the process heap (the CRT's and
// DXC's) is validated (HeapValidate, ~1 ms): damaged heap block headers then fail the run with an
// error naming the last shader instead of a later crash somewhere else. (Not every corruption is
// visible to HeapValidate, e.g. in low-fragmentation-heap blocks; what only the heap manager itself
// detects still ends the process with STATUS_HEAP_CORRUPTION, 0xC0000374.)
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
    void CheckThread() const;
    // Throws if the process heap fails HeapValidate after the DXC call 'what' (on the shader m_current).
    void CheckHeaps(const char* what) const;
    ComPtr<IDxcBlob> Compile(const DxcBuffer& source, const std::wstring& name, const std::string& entry,
                             const ShaderDefines& defines, const std::filesystem::path& includeDir, std::string& log);

    ComPtr<IDxcUtils> m_utils;
    ComPtr<IDxcCompiler3> m_compiler;
    ComPtr<IDxcIncludeHandler> m_includeHandler;
    DWORD m_threadId = 0;
    std::string m_current; // the shader of the last Compile (file, entry, defines), for CheckHeaps errors
};
