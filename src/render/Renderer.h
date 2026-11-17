#pragma once
// Frame renderer. Everything is drawn into an offscreen colour target first; that
// target is then either copied to the swap chain (window) or read back to the CPU
// (capture). Window and headless capture therefore share one rendering path.
//
// Round 1 draws a single full-screen pass: an animated "tactical display" background
// with the overhead maze map and the scripted-route marker on top.

#include <array>
#include <cstdint>

#include "core/Image.h"
#include "render/Gpu.h"

namespace bh {

struct FrameParams {
    float time = 0.0f;           // seconds
    float mapOpacity = 1.0f;     // 0 hides the map panel
    float markerU = 0.0f;        // route marker position in map UV (0..1)
    float markerV = 0.0f;
    float markerVisible = 0.0f;  // 0..1
};

class Renderer {
public:
    // hwnd may be null for headless rendering.
    Renderer(Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    void resize(uint32_t width, uint32_t height);
    void setMapImage(const Image& image);

    // Window mode: render and present.
    void renderAndPresent(const FrameParams& params);
    // Render one frame and read it back (works with or without a window).
    Image renderToImage(const FrameParams& params);

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kSwapBuffers = 2;
    static constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    struct FrameContext {
        ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
    };

    void createPipeline();
    void createSizeDependent();
    void beginFrame();
    void recordScene(const FrameParams& params);
    void submit();
    void transitionTarget(D3D12_RESOURCE_STATES to);

    Gpu& gpu_;
    HWND hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::array<FrameContext, kFramesInFlight> frames_{};
    uint32_t frameIndex_ = 0;
    ComPtr<ID3D12GraphicsCommandList4> cmd_;

    ComPtr<IDXGISwapChain3> swapChain_;
    std::array<ComPtr<ID3D12Resource>, kSwapBuffers> backBuffers_{};

    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12Resource> target_;
    D3D12_RESOURCE_STATES targetState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;

    ComPtr<ID3D12Resource> readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint_{};

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipeline_;

    ComPtr<ID3D12Resource> mapTexture_;
    uint32_t mapWidth_ = 1;
    uint32_t mapHeight_ = 1;
};

}  // namespace bh
