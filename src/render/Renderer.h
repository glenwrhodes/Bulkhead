#pragma once
// Ray-traced renderer (DXR 1.1 inline ray queries in a compute shader).
//
// setKit()   uploads every kit model into one vertex/index pool and builds a bottom-level
//            acceleration structure per part.
// setLevel() uploads the level's lights and the per-cell light grid.
// Each frame: rebuild the top-level structure from the instance list (doors and the gun
// move), trace into an HDR accumulation buffer, then tone-map + HUD into an 8-bit target
// that is either presented (window) or read back (capture).

#include <array>
#include <cstdint>
#include <vector>

#include "core/Image.h"
#include "core/Math.h"
#include "render/Gpu.h"

namespace bh {

class Kit;
struct Level;
struct SceneInstance;

struct FrameSettings {
    Camera camera;
    bool rayTracing = true;
    bool hud = true;
    bool minimap = true;
    float bigMap = 0.0f;       // 0..1 opacity of the full-screen map
    float exposure = 1.0f;
    float exitFlash = 0.0f;
    float time = 0.0f;
    Vec2 playerMapUV;          // player position on the overhead map image (0..1)
    int samples = 1;           // >1: average this many sub-frames of a still camera (capture)
    bool resetHistory = false; // window mode: drop the temporal history (teleport, seed change)
    int maxHistory = 16;       // window mode: frames blended at most
};

class Renderer {
public:
    Renderer(Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    void resize(uint32_t width, uint32_t height);
    void setMapImage(const Image& image);
    void setKit(const Kit& kit);
    void setLevel(const Level& level);

    void renderAndPresent(const std::vector<SceneInstance>& instances, const FrameSettings& settings);
    // Renders without presenting or reading back (benchmarks). gpuIdle() waits for it.
    void render(const std::vector<SceneInstance>& instances, const FrameSettings& settings);
    void gpuIdle() { gpu_.waitIdle(); }
    Image renderToImage(const std::vector<SceneInstance>& instances, const FrameSettings& settings);

    uint64_t trianglesInScene() const { return lastSceneTriangles_; }

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kSwapBuffers = 2;
    static constexpr uint32_t kMaxSamples = 64;
    static constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

    struct FrameContext {
        ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
        ComPtr<ID3D12Resource> constants;   // kMaxSamples x 256 bytes, upload heap
        uint8_t* constantsPtr = nullptr;
        ComPtr<ID3D12Resource> instanceDescs;
        ComPtr<ID3D12Resource> instanceInfo;
        void* instanceDescsPtr = nullptr;
        void* instanceInfoPtr = nullptr;
        ComPtr<ID3D12Resource> tlas;
        ComPtr<ID3D12Resource> tlasScratch;
        uint32_t capacity = 0;
    };

    void createPipelines();
    void createSizeDependent();
    void writeDescriptors();
    void beginFrame();
    void submit();
    void ensureInstanceCapacity(FrameContext& f, uint32_t count);
    void recordScene(const std::vector<SceneInstance>& instances, const FrameSettings& s);
    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to);
    ComPtr<ID3D12Resource> createBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> uploadBuffer(const void* data, uint64_t size);  // upload heap, filled
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(uint32_t i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(uint32_t i) const;

    Gpu& gpu_;
    HWND hwnd_ = nullptr;
    uint32_t width_ = 0, height_ = 0;

    std::array<FrameContext, kFramesInFlight> frames_{};
    uint32_t frameIndex_ = 0;
    ComPtr<ID3D12GraphicsCommandList4> cmd_;

    ComPtr<IDXGISwapChain3> swapChain_;
    std::array<ComPtr<ID3D12Resource>, kSwapBuffers> backBuffers_{};

    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    uint32_t srvStride_ = 0;
    ComPtr<ID3D12Resource> target_;
    D3D12_RESOURCE_STATES targetState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ComPtr<ID3D12Resource> readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint_{};

    // ping-pong HDR accumulation and world position/id (for reprojection)
    std::array<ComPtr<ID3D12Resource>, 2> accum_{}, pos_{};
    std::array<D3D12_RESOURCE_STATES, 2> accumState_{}, posState_{};
    uint32_t last_ = 0;  // buffer written by the most recent dispatch
    bool historyValid_ = false;
    Camera prevCamera_;
    uint64_t frameCounter_ = 0;

    ComPtr<ID3D12RootSignature> traceRS_, presentRS_;
    ComPtr<ID3D12PipelineState> tracePSO_, presentPSO_;

    ComPtr<ID3D12Resource> mapTexture_;
    uint32_t mapWidth_ = 1, mapHeight_ = 1;

    // kit
    const Kit* kit_ = nullptr;
    ComPtr<ID3D12Resource> vertexBuffer_, indexBuffer_, submeshBuffer_, materialBuffer_;
    std::vector<ComPtr<ID3D12Resource>> blas_;
    std::vector<uint64_t> partTriangles_;
    // level
    ComPtr<ID3D12Resource> lightBuffer_, cellBuffer_, cellLightBuffer_;
    float gridW_ = 1, gridH_ = 1, lightCount_ = 0;
    uint64_t lastSceneTriangles_ = 0;
};

}  // namespace bh
