#include "Algorithms.h"

#include "Common.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace
{
constexpr std::pair<FlushKind, const char*> kKindNames[] = {
    {FlushKind::Full, "full"}, {FlushKind::FullRo, "full_ro"}, {FlushKind::Code, "code"},
    {FlushKind::Data, "data"}, {FlushKind::None, "none"},
};

// The drain of the pass5 mode of that name (no suffix).
DrainKind DefaultDrain(FlushKind kind)
{
    return kind == FlushKind::FullRo ? DrainKind::None : DrainKind::Group;
}
} // namespace

std::string FlushModeName(const FlushMode& mode)
{
    if (mode.kind == FlushKind::Full && mode.drain == DrainKind::None)
        return "full_legacy";
    std::string name = "?";
    for (const auto& [kind, kindName] : kKindNames)
    {
        if (kind == mode.kind)
            name = kindName;
    }
    if (mode.drain == DefaultDrain(mode.kind))
        return name;
    switch (mode.drain)
    {
    case DrainKind::None: return name + "_d0";
    case DrainKind::Group: return name + "_dg";
    case DrainKind::Spin: return name + "_d" + std::to_string(mode.drainUs);
    }
    return name;
}

bool ParseFlushMode(const std::string& name, FlushMode& mode)
{
    if (name == "full_legacy")
    {
        mode = {FlushKind::Full, DrainKind::None, 0};
        return true;
    }
    for (const auto& [kind, kindName] : kKindNames)
    {
        const std::string base = kindName;
        if (name.compare(0, base.size(), base) != 0)
            continue;
        const std::string suffix = name.substr(base.size());
        if (suffix.empty())
        {
            mode = {kind, DefaultDrain(kind), 0};
            return true;
        }
        if (suffix == "_dg")
        {
            mode = {kind, DrainKind::Group, 0};
            return true;
        }
        // _d<N>: decimal, no sign / leading zeros (except "0"), 0..kMaxDrainUs
        if (suffix.size() < 3 || suffix.compare(0, 2, "_d") != 0)
            continue;
        const std::string digits = suffix.substr(2);
        if (digits.size() > 4 || (digits.size() > 1 && digits[0] == '0') ||
            digits.find_first_not_of("0123456789") != std::string::npos)
            continue;
        const uint32_t us = static_cast<uint32_t>(std::stoul(digits));
        if (us > kMaxDrainUs)
            continue;
        mode = us == 0 ? FlushMode{kind, DrainKind::None, 0} : FlushMode{kind, DrainKind::Spin, us};
        return true;
    }
    return false;
}

std::vector<AlgorithmDesc> LoadAlgorithms(const std::filesystem::path& shaderDir, const std::filesystem::path& fileName)
{
    const std::filesystem::path file = fileName.is_absolute() ? fileName : shaderDir / fileName;
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
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
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
            AlgorithmDesc a;
            a.name = tokens[1];
            algorithms.push_back(std::move(a));
        }
        else if (tokens[0] == "flush")
        {
            if (algorithms.empty())
                fail("'flush' before any 'algorithm'");
            if (tokens.size() != 2 || !ParseFlushMode(tokens[1], algorithms.back().flush))
                fail("expected: flush <full|full_legacy|full_ro|code|data|none>[_dg|_d<us>] (see --help)");
            if (algorithms.back().flushGiven)
                fail("duplicate 'flush' for algorithm '" + algorithms.back().name + "'");
            algorithms.back().flushGiven = true;
        }
        else if (tokens[0] == "pass")
        {
            if (algorithms.empty())
                fail("'pass' before any 'algorithm'");
            AlgorithmDesc& a = algorithms.back();
            if (a.pass >= 0)
                fail("duplicate 'pass' for algorithm '" + a.name + "'");
            if (tokens.size() != 2 || tokens[1].size() > 2 ||
                tokens[1].find_first_not_of("0123456789") != std::string::npos)
                fail("expected: pass <0..99>");
            a.pass = std::stoi(tokens[1]);
        }
        else if (tokens[0] == "desc")
        {
            if (algorithms.empty())
                fail("'desc' before any 'algorithm'");
            AlgorithmDesc& a = algorithms.back();
            if (!a.description.empty())
                fail("duplicate 'desc' for algorithm '" + a.name + "'");
            // The rest of the line after the keyword (the comment is already cut off).
            std::string text = line.substr(line.find("desc") + 4);
            const size_t first = text.find_first_not_of(" \t");
            const size_t last = text.find_last_not_of(" \t");
            a.description = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
            if (a.description.empty())
                fail("expected: desc <text>");
        }
        else if (tokens[0] == "tag")
        {
            if (algorithms.empty())
                fail("'tag' before any 'algorithm'");
            if (tokens.size() < 2)
                fail("expected: tag <word> [<word> ...]");
            AlgorithmDesc& a = algorithms.back();
            for (size_t i = 1; i < tokens.size(); ++i)
            {
                if (tokens[i].find(';') != std::string::npos)
                    fail("a tag must not contain ';'");
                if (std::find(a.tags.begin(), a.tags.end(), tokens[i]) == a.tags.end())
                    a.tags.push_back(tokens[i]);
            }
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
