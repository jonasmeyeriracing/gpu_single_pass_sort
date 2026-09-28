#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

[[noreturn]] void ThrowHr(HRESULT hr, const char* expr, const char* file, int line);

#define CHECK_HR(expr)                               \
    do                                               \
    {                                                \
        const HRESULT hr_ = (expr);                  \
        if (FAILED(hr_))                             \
            ThrowHr(hr_, #expr, __FILE__, __LINE__); \
    } while (false)

std::string WideToUtf8(const std::wstring& s);
std::wstring Utf8ToWide(const std::string& s);

// printf-style formatting into a std::string.
std::string Format(const char* fmt, ...);

// Prints to stdout and flushes (so progress is visible when stdout is redirected). Also appends to
// the log file, if one was opened with OpenLogFile.
void Log(const char* fmt, ...);

// Like Log, but prints to stderr (and to the log file).
void LogError(const char* fmt, ...);

// --log <file>: from now on Log / LogError also write to this file (truncated). Returns false if it
// cannot be opened.
bool OpenLogFile(const std::wstring& path);
