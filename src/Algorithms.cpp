#include "Algorithms.h"

#include "Common.h"

#include <fstream>
#include <sstream>

std::vector<AlgorithmDesc> LoadAlgorithms(const std::filesystem::path& shaderDir)
{
    const std::filesystem::path file = shaderDir / "algorithms.txt";
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());

    std::vector<AlgorithmDesc> algorithms;
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& what) {
        throw std::runtime_error(Format("%s(%d): %s", file.string().c_str(), lineNo, what.c_str()));
    };

    while (std::getline(in, line))
    {
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.resize(hash);
        std::istringstream ss(line);
        std::vector<std::string> tokens;
        for (std::string t; ss >> t;)
            tokens.push_back(t);
        if (tokens.empty())
            continue;

        if (tokens[0] == "algorithm")
        {
            if (tokens.size() != 2)
                fail("expected: algorithm <name>");
            for (const auto& a : algorithms)
            {
                if (a.name == tokens[1])
                    fail("duplicate algorithm '" + tokens[1] + "'");
            }
            algorithms.push_back({tokens[1], {}});
        }
        else if (tokens[0] == "dispatch")
        {
            if (algorithms.empty())
                fail("'dispatch' before any 'algorithm'");
            if (tokens.size() < 4)
                fail("expected: dispatch <file> <entry> <groupSize> [NAME=VALUE ...]");
            DispatchDesc d;
            d.file = tokens[1];
            d.entry = tokens[2];
            try
            {
                d.groupSize = static_cast<uint32_t>(std::stoul(tokens[3]));
            }
            catch (...)
            {
                fail("invalid group size '" + tokens[3] + "'");
            }
            if (d.groupSize == 0 || d.groupSize > 1024)
                fail("group size must be 1..1024");
            for (size_t i = 4; i < tokens.size(); ++i)
            {
                const size_t eq = tokens[i].find('=');
                if (eq == std::string::npos)
                    d.defines.emplace_back(tokens[i], "");
                else
                    d.defines.emplace_back(tokens[i].substr(0, eq), tokens[i].substr(eq + 1));
            }
            algorithms.back().dispatches.push_back(std::move(d));
        }
        else
        {
            fail("unknown keyword '" + tokens[0] + "'");
        }
    }

    if (algorithms.empty())
        throw std::runtime_error(file.string() + ": no algorithms defined");
    for (const auto& a : algorithms)
    {
        if (a.dispatches.empty())
            throw std::runtime_error(file.string() + ": algorithm '" + a.name + "' has no dispatches");
    }
    return algorithms;
}
