#include "core/Window.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

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

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse
    rid.hwndTarget = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
}

Window::~Window() {
    setMouseCaptured(false);
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

bool Window::focused() const { return GetForegroundWindow() == hwnd_; }

bool Window::keyDown(int vk) const { return focused() && (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Window::setMouseCaptured(bool captured) {
    if (captured == captured_) return;
    captured_ = captured;
    if (captured) {
        RECT r;
        GetClientRect(hwnd_, &r);
        POINT tl{r.left, r.top}, br{r.right, r.bottom};
        ClientToScreen(hwnd_, &tl);
        ClientToScreen(hwnd_, &br);
        RECT clip{tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {
        }
    }
    mouseDx_ = mouseDy_ = 0.0f;
}

void Window::takeMouseDelta(float& dx, float& dy) {
    dx = mouseDx_;
    dy = mouseDy_;
    mouseDx_ = mouseDy_ = 0.0f;
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
            case WM_LBUTTONDOWN:
                self->setMouseCaptured(true);
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) self->setMouseCaptured(false);
                break;
            case WM_SIZE:
            case WM_MOVE:
                if (self->captured_) {  // re-clip to the new rectangle
                    self->setMouseCaptured(false);
                    self->setMouseCaptured(true);
                }
                break;
            case WM_INPUT: {
                UINT size = 0;
                GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
                std::vector<BYTE> buf(size);
                if (size && GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buf.data(), &size,
                                            sizeof(RAWINPUTHEADER)) == size) {
                    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf.data());
                    if (ri->header.dwType == RIM_TYPEMOUSE && self->captured_ &&
                        !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                        self->mouseDx_ += float(ri->data.mouse.lLastX);
                        self->mouseDy_ += float(ri->data.mouse.lLastY);
                    }
                }
                break;
            }
            case WM_ERASEBKGND:
                return 1;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace bh
