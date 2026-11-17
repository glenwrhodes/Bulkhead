#include "core/Window.h"

#include <algorithm>
#include <stdexcept>

namespace bh {
namespace {
constexpr wchar_t kClassName[] = L"BulkheadWindow";
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

Window::Window(const std::string& title, int clientWidth, int clientHeight) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, clientWidth, clientHeight};
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - w) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2);

    hwnd_ = CreateWindowExW(0, kClassName, widen(title).c_str(), style, x, y, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) throw std::runtime_error("CreateWindowExW failed");
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
}

Window::~Window() {
    if (hwnd_) DestroyWindow(hwnd_);
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool Window::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

void Window::setTitle(const std::string& title) { SetWindowTextW(hwnd_, widen(title).c_str()); }

void Window::clientSize(uint32_t& width, uint32_t& height) const {
    RECT r{};
    GetClientRect(hwnd_, &r);
    width = uint32_t(std::max<LONG>(0, r.right - r.left));
    height = uint32_t(std::max<LONG>(0, r.bottom - r.top));
}

LRESULT CALLBACK Window::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
            case WM_CLOSE:
                self->closed_ = true;
                return 0;
            case WM_KEYDOWN:
                if ((lParam & (1 << 30)) == 0 && self->onKeyDown) self->onKeyDown(int(wParam));
                return 0;
            case WM_ERASEBKGND:
                return 1;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace bh
