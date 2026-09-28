#include "Common.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

void ThrowHr(HRESULT hr, const char* expr, const char* file, int line)
{
    throw std::runtime_error(Format("%s failed with HRESULT 0x%08X (%s:%d)", expr, static_cast<unsigned>(hr), file, line));
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}

static std::string FormatV(const char* fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0)
        return {};
    std::vector<char> buf(static_cast<size_t>(n) + 1);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    return std::string(buf.data(), static_cast<size_t>(n));
}

std::string Format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string r = FormatV(fmt, args);
    va_end(args);
    return r;
}

static FILE* g_logFile = nullptr;

bool OpenLogFile(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return false;
    if (g_logFile)
        fclose(g_logFile);
    g_logFile = f;
    return true;
}

static void WriteLog(FILE* console, const std::string& s)
{
    fputs(s.c_str(), console);
    fflush(console);
    if (g_logFile)
    {
        fputs(s.c_str(), g_logFile);
        fflush(g_logFile);
    }
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const std::string s = FormatV(fmt, args);
    va_end(args);
    WriteLog(stdout, s);
}

void LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const std::string s = FormatV(fmt, args);
    va_end(args);
    WriteLog(stderr, s);
}
