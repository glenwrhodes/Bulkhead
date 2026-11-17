#pragma once
// D3D12 device, direct queue and a fence. Picks the high-performance adapter
// (the RTX GPU on a laptop) and records its ray tracing support.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace bh {

using Microsoft::WRL::ComPtr;

// Throws std::runtime_error with the HRESULT and the failing expression.
void throwIfFailed(HRESULT hr, const char* expression, const char* file, int line);
#define BH_CHECK(expr) ::bh::throwIfFailed((expr), #expr, __FILE__, __LINE__)

struct GpuInfo {
    std::string adapterName;
    uint64_t dedicatedVideoMemory = 0;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_0;
    D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    std::string raytracingTierName() const;
};

class Gpu {
public:
    explicit Gpu(bool debugLayer);
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    ID3D12Device5* device() const { return device_.Get(); }
    IDXGIFactory6* factory() const { return factory_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    const GpuInfo& info() const { return info_; }

    uint64_t signal();                 // returns the fence value just signalled
    void waitFor(uint64_t fenceValue); // blocks the CPU until the GPU reaches it
    void waitIdle();

private:
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device5> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t nextFenceValue_ = 1;
    GpuInfo info_;
};

}  // namespace bh
