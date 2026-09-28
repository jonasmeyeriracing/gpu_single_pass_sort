#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using ShaderDefines = std::vector<std::pair<std::string, std::string>>;

// One ExecuteIndirect(DISPATCH {numSorts, 1, 1}) of one shader. Every group handles one sort
// (SV_GroupID.x = sort index) and early-outs if that sort is not in its tier.
struct DispatchDesc
{
    std::string file;       // relative to the shader directory
    std::string entry;
    uint32_t groupSize = 0; // passed to the shader as GROUP_SIZE
    ShaderDefines defines;  // extra defines, e.g. MIN_COUNT / MAX_COUNT
};

// An algorithm is a list of dispatches issued back-to-back without barriers.
struct AlgorithmDesc
{
    std::string name;
    std::vector<DispatchDesc> dispatches;
};

// Algorithms are registered by the shader directory itself, in <shaderDir>/algorithms.txt:
//
//   # comment
//   algorithm <name>
//   dispatch <file.hlsl> <entryPoint> <groupSize> [NAME=VALUE ...]
//
// so every _test/passN snapshot carries its own algorithm list. Throws on parse errors.
std::vector<AlgorithmDesc> LoadAlgorithms(const std::filesystem::path& shaderDir);
