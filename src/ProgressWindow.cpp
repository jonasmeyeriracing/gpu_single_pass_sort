#include "ProgressWindow.h"

namespace
{
constexpr UINT WM_PROGRESS_UPDATE = WM_APP + 1;
constexpr UINT WM_PROGRESS_CLOSE = WM_APP + 2;
constexpr wchar_t kClassName[] = L"GpuSortProgressWindow";
constexpr int kWidth = 480;
constexpr int kHeight = 190;
} // namespace

ProgressWindow::~ProgressWindow()
{
    Stop();
}

void ProgressWindow::Start(const std::wstring& title)
{
    if (m_thread.joinable())
        return;
    m_title = title;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = std::thread([this, ready] { ThreadMain(ready); });
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
}

void ProgressWindow::SetText(const std::wstring& text)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_text = text;
    }
    if (HWND hwnd = m_hwnd.load())
        PostMessageW(hwnd, WM_PROGRESS_UPDATE, 0, 0);
}

void ProgressWindow::Stop()
{
    if (!m_thread.joinable())
        return;
    if (HWND hwnd = m_hwnd.load())
        PostMessageW(hwnd, WM_PROGRESS_CLOSE, 0, 0);
    m_thread.join();
}

void ProgressWindow::ThreadMain(HANDLE readyEvent)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ProgressWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    m_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.right - kWidth - 20;
    const int y = work.top + 20;

    // No system menu -> no close button; the window is closed by Stop().
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, m_title.c_str(),
                                WS_POPUP | WS_CAPTION | WS_BORDER, x, y, kWidth, kHeight, nullptr, nullptr, instance,
                                this);
    m_hwnd.store(hwnd);
    if (hwnd)
    {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    SetEvent(readyEvent);

    if (hwnd)
    {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    m_hwnd.store(nullptr);
    if (m_font)
        DeleteObject(m_font);
    m_font = nullptr;
    UnregisterClassW(kClassName, instance);
}

LRESULT CALLBACK ProgressWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_PROGRESS_UPDATE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_PROGRESS_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        return 0; // ignore user close requests while the benchmark runs
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self)
        {
            std::wstring text;
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                text = self->m_text;
            }
            RECT rc;
            GetClientRect(hwnd, &rc);
            InflateRect(&rc, -12, -10);
            HGDIOBJ oldFont = self->m_font ? SelectObject(dc, self->m_font) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, text.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
            if (oldFont)
                SelectObject(dc, oldFont);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
