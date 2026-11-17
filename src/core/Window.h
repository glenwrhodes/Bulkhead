#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace bh {

class Window {
public:
    Window(const std::string& title, int clientWidth, int clientHeight);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    HWND hwnd() const { return hwnd_; }
    // Processes pending messages. Returns false once the window has been closed.
    bool pump();
    void close() { closed_ = true; }
    void setTitle(const std::string& title);
    void clientSize(uint32_t& width, uint32_t& height) const;

    std::function<void(int virtualKey)> onKeyDown;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    HWND hwnd_ = nullptr;
    bool closed_ = false;
};

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

}  // namespace bh
