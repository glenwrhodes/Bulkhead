#include "render/Gpu.h"

#include <format>
#include <stdexcept>

#include "core/Log.h"
#include "core/Window.h"

namespace bh {

void throwIfFailed(HRESULT hr, const char* expression, const char* file, int line) {
    if (SUCCEEDED(hr)) return;
    throw std::runtime_error(std::format("{} failed with HRESULT 0x{:08X} ({}:{})", expression, uint32_t(hr), file, line));
}

std::string GpuInfo::raytracingTierName() const {
    switch (raytracingTier) {
        case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "not supported";
        case D3D12_RAYTRACING_TIER_1_0: return "1.0";
        case D3D12_RAYTRACING_TIER_1_1: return "1.1";
        default: return std::format("1.{}", int(raytracingTier) - int(D3D12_RAYTRACING_TIER_1_0));
    }
}

Gpu::Gpu(bool debugLayer) {
    UINT factoryFlags = 0;
    if (debugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            log::info("D3D12 debug layer enabled");
        } else {
            log::warn("D3D12 debug layer requested but not available (install Graphics Tools)");
        }
    }
    BH_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)));

    // Highest-performance hardware adapter that gives us a DXR-capable device interface.
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
            adapter_ = adapter;
            info_.adapterName = narrow(desc.Description);
            info_.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            break;
        }
    }
    if (!device_) throw std::runtime_error("No DirectX 12 GPU (feature level 12_0) found");

    if (debugLayer) {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(device_.As(&infoQueue))) {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
    fl.NumFeatureLevels = UINT(std::size(levels));
    fl.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        info_.featureLevel = fl.MaxSupportedFeatureLevel;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
        info_.raytracingTier = o5.RaytracingTier;

    log::info("GPU: {} ({} MB VRAM), feature level {}_{}, ray tracing tier {}", info_.adapterName,
              info_.dedicatedVideoMemory / (1024 * 1024), (uint32_t(info_.featureLevel) >> 12) & 0xF,
              (uint32_t(info_.featureLevel) >> 8) & 0xF, info_.raytracingTierName());
    if (info_.raytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        log::warn("This GPU has no DXR support; ray traced passes will be unavailable.");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    BH_CHECK(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)));
    queue_->SetName(L"Bulkhead direct queue");

    BH_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) throw std::runtime_error("CreateEvent failed");
}

Gpu::~Gpu() {
    if (queue_ && fence_) {
        try {
            waitIdle();
        } catch (...) {
        }
    }
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

uint64_t Gpu::signal() {
    const uint64_t value = nextFenceValue_++;
    BH_CHECK(queue_->Signal(fence_.Get(), value));
    return value;
}

void Gpu::waitFor(uint64_t value) {
    if (fence_->GetCompletedValue() >= value) return;
    BH_CHECK(fence_->SetEventOnCompletion(value, fenceEvent_));
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void Gpu::waitIdle() { waitFor(signal()); }

}  // namespace bh
