#pragma once

#include "Common.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Small always-on-top, non-activating status window with its own thread and message pump.
class ProgressWindow
{
public:
    ProgressWindow() = default;
    ~ProgressWindow();
    ProgressWindow(const ProgressWindow&) = delete;
    ProgressWindow& operator=(const ProgressWindow&) = delete;

    void Start(const std::wstring& title);
    void SetText(const std::wstring& text);
    void Stop();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void ThreadMain(HANDLE readyEvent);

    std::thread m_thread;
    std::atomic<HWND> m_hwnd{nullptr};
    std::mutex m_mutex;
    std::wstring m_text;
    std::wstring m_title;
    HFONT m_font = nullptr;
};
