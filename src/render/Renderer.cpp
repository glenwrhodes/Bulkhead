#include "render/Renderer.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "assets/Kit.h"
#include "core/Log.h"
#include "game/Game.h"
#include "world/Level.h"

// DXIL bytecode generated at build time by dxc (see cmake/Shaders.cmake).
#include "g_PresentPS.h"
#include "g_PresentVS.h"
#include "g_TraceCS.h"

namespace bh {
namespace {

struct TraceConstants {
    float camPos[4], camRight[4], camUp[4], camFwd[4];
    float prevPos[4], prevRight[4], prevUp[4], prevFwd[4];
    float resolution[4], jitter[4], grid[4], options[4];
    float flashPos[4], flashDir[4], flashCone[4], haze[4];
};
static_assert(sizeof(TraceConstants) <= 256, "one constant slot");

struct PresentConstants {
    float p0[4], p1[4], p2[4], p3[4], p4[4], p5[4], p6[4];
};
constexpr uint32_t kMaxBlips = 32;
constexpr uint32_t kMaxDynLights = 128;  // short-lived lights a frame (32 before round 18)
constexpr UINT kPresentConstantCount = sizeof(PresentConstants) / 4;

struct GpuSubmesh {
    uint32_t firstIndex, baseVertex, material, pad;
};
struct GpuMaterial {
    float baseAlpha[4], emissiveRough[4], metalPad[4];
};
struct GpuInstanceInfo {
    uint32_t firstSubmesh, flags, pad0, pad1;
    float emissive[4];
};
static_assert(sizeof(GpuInstanceInfo) == 32, "matches InstInfo in trace.hlsl");
struct GpuCellInfo {
    float decay, haze, mood, pad;
};
struct GpuSteam {
    float originLength[4], dirStrength[4], lightSeed[4];
};
static_assert(sizeof(GpuSteam) == 48, "matches Steam in trace.hlsl");

// descriptor heap slots
constexpr uint32_t kSlotMap = 0;
constexpr uint32_t kSlotTrace = 1;    // + parity * 4: prevAccum SRV, prevPos SRV, accum UAV, pos UAV
constexpr uint32_t kSlotPresent = 9;  // + parity * 2: accum SRV, map SRV
constexpr uint32_t kHeapSize = 16;

constexpr UINT8 kMaskWorld = 0x01, kMaskGun = 0x02, kMaskGlass = 0x04, kMaskFx = 0x08;

D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

D3D12_RESOURCE_BARRIER uavBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    return h;
}

D3D12_RESOURCE_DESC bufferDesc(uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(size, 16);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_DESC textureDesc(uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    return d;
}

float halton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= float(base);
        r += f * float(i % base);
        i /= base;
    }
    return r;
}

void setCamera(float pos[4], float right[4], float up[4], float fwd[4], const Camera& c, float aspect) {
    Vec3 p = c.position, r = c.right(), u = c.up(), f = c.forward();
    float ty = std::tan(c.fovY * 0.5f), tx = ty * aspect;
    pos[0] = p.x, pos[1] = p.y, pos[2] = p.z, pos[3] = tx;
    right[0] = r.x, right[1] = r.y, right[2] = r.z, right[3] = ty;
    up[0] = u.x, up[1] = u.y, up[2] = u.z, up[3] = 0;
    fwd[0] = f.x, fwd[1] = f.y, fwd[2] = f.z, fwd[3] = 0;
}

ComPtr<ID3D12RootSignature> serializeRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc) {
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
        throw std::runtime_error(std::string("Root signature: ") + msg);
    }
    ComPtr<ID3D12RootSignature> rs;
    BH_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    return rs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Renderer::Renderer(Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height)
    : gpu_(gpu), hwnd_(hwnd), width_(std::max(1u, width)), height_(std::max(1u, height)) {
    ID3D12Device5* device = gpu_.device();
    if (gpu_.info().raytracingTier < D3D12_RAYTRACING_TIER_1_1)
        throw std::runtime_error("This GPU does not support DXR 1.1 (inline ray tracing), which Bulkhead needs.");

    for (FrameContext& f : frames_) {
        BH_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)));
        f.constants = createBuffer(256ull * kMaxSamples, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        BH_CHECK(f.constants->Map(0, nullptr, reinterpret_cast<void**>(&f.constantsPtr)));
    }
    BH_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&cmd_)));
    BH_CHECK(cmd_->Close());

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = 1;
    BH_CHECK(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap_)));
    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = kHeapSize;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    BH_CHECK(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srvHeap_)));
    srvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (hwnd_) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width_;
        sd.Height = height_;
        sd.Format = kColorFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kSwapBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> sc;
        BH_CHECK(gpu_.factory()->CreateSwapChainForHwnd(gpu_.queue(), hwnd_, &sd, nullptr, nullptr, &sc));
        BH_CHECK(sc.As(&swapChain_));
        gpu_.factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    }

    createPipelines();
    setMapImage(Image(1, 1, rgb(0x000000)));
    createSizeDependent();
}

Renderer::~Renderer() { gpu_.waitIdle(); }

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::cpuSlot(uint32_t i) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * srvStride_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::gpuSlot(uint32_t i) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += uint64_t(i) * srvStride_;
    return h;
}

ComPtr<ID3D12Resource> Renderer::createBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                              D3D12_RESOURCE_FLAGS flags) {
    const D3D12_HEAP_PROPERTIES hp = heapProps(heap);
    const D3D12_RESOURCE_DESC d = bufferDesc(size, flags);
    ComPtr<ID3D12Resource> r;
    BH_CHECK(gpu_.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

ComPtr<ID3D12Resource> Renderer::uploadBuffer(const void* data, uint64_t size) {
    ComPtr<ID3D12Resource> r = createBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr;
    BH_CHECK(r->Map(0, nullptr, &p));
    if (size) std::memcpy(p, data, size_t(size));
    r->Unmap(0, nullptr);
    return r;
}

void Renderer::createPipelines() {
    ID3D12Device5* device = gpu_.device();

    // --- trace (compute) ---
    D3D12_ROOT_PARAMETER tp[14]{};
    tp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    tp[0].Descriptor.ShaderRegister = 0;
    for (UINT i = 1; i <= 9; ++i) {
        tp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        tp[i].Descriptor.ShaderRegister = i - 1;  // t0..t8
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 9;  // t9, t10
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;  // u0, u1
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    tp[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tp[10].DescriptorTable.NumDescriptorRanges = 2;
    tp[10].DescriptorTable.pDescriptorRanges = ranges;
    tp[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    tp[11].Descriptor.ShaderRegister = 11;  // t11: per-cell wear, haze, mood
    tp[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    tp[12].Descriptor.ShaderRegister = 12;  // t12: short-lived lights
    tp[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    tp[13].Descriptor.ShaderRegister = 13;  // t13: steam plumes
    for (auto& p : tp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC trs{};
    trs.NumParameters = 14;
    trs.pParameters = tp;
    traceRS_ = serializeRootSignature(device, trs);

    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = traceRS_.Get();
    cp.CS = {g_TraceCS, sizeof(g_TraceCS)};
    BH_CHECK(device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&tracePSO_)));

    // --- present (graphics) ---
    D3D12_DESCRIPTOR_RANGE prange{};
    prange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    prange.NumDescriptors = 2;
    prange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER pp[4]{};
    pp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    pp[0].Constants.Num32BitValues = kPresentConstantCount;
    pp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    pp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pp[1].DescriptorTable.NumDescriptorRanges = 1;
    pp[1].DescriptorTable.pDescriptorRanges = &prange;
    pp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    pp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    pp[2].Descriptor.ShaderRegister = 2;  // t2: explored cells
    pp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    pp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    pp[3].Descriptor.ShaderRegister = 3;  // t3: scanner blips
    pp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC prs{};
    prs.NumParameters = 4;
    prs.pParameters = pp;
    prs.NumStaticSamplers = 1;
    prs.pStaticSamplers = &sampler;
    presentRS_ = serializeRootSignature(device, prs);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = presentRS_.Get();
    pso.VS = {g_PresentVS, sizeof(g_PresentVS)};
    pso.PS = {g_PresentPS, sizeof(g_PresentPS)};
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.FrontFace = keep;
    pso.DepthStencilState.BackFace = keep;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kColorFormat;
    pso.SampleDesc.Count = 1;
    BH_CHECK(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&presentPSO_)));
}

void Renderer::createSizeDependent() {
    ID3D12Device5* device = gpu_.device();
    if (swapChain_) {
        for (auto& b : backBuffers_) b.Reset();
        BH_CHECK(swapChain_->ResizeBuffers(kSwapBuffers, width_, height_, kColorFormat, 0));
        for (UINT i = 0; i < kSwapBuffers; ++i) BH_CHECK(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])));
    }
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);

    target_.Reset();
    const D3D12_RESOURCE_DESC td = textureDesc(width_, height_, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kColorFormat;
    BH_CHECK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target_)));
    target_->SetName(L"LDR target");
    targetState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    device->CreateRenderTargetView(target_.Get(), nullptr, rtvHeap_->GetCPUDescriptorHandleForHeapStart());

    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &readbackFootprint_, nullptr, nullptr, &total);
    readback_ = createBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    const D3D12_RESOURCE_DESC hd =
        textureDesc(width_, height_, kHdrFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (int i = 0; i < 2; ++i) {
        accum_[size_t(i)].Reset();
        pos_[size_t(i)].Reset();
        BH_CHECK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &hd,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&accum_[size_t(i)])));
        BH_CHECK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &hd,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&pos_[size_t(i)])));
        accumState_[size_t(i)] = posState_[size_t(i)] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    historyValid_ = false;
    writeDescriptors();
}

void Renderer::writeDescriptors() {
    ID3D12Device5* device = gpu_.device();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kHdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = kHdrFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (uint32_t p = 0; p < 2; ++p) {
        uint32_t base = kSlotTrace + p * 4;
        device->CreateShaderResourceView(accum_[1 - p].Get(), &srv, cpuSlot(base + 0));
        device->CreateShaderResourceView(pos_[1 - p].Get(), &srv, cpuSlot(base + 1));
        device->CreateUnorderedAccessView(accum_[p].Get(), nullptr, &uav, cpuSlot(base + 2));
        device->CreateUnorderedAccessView(pos_[p].Get(), nullptr, &uav, cpuSlot(base + 3));
        device->CreateShaderResourceView(accum_[p].Get(), &srv, cpuSlot(kSlotPresent + p * 2));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC map{};
    map.Format = kColorFormat;
    map.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    map.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    map.Texture2D.MipLevels = 1;
    for (uint32_t slot : {kSlotMap, kSlotPresent + 1, kSlotPresent + 3})
        device->CreateShaderResourceView(mapTexture_.Get(), &map, cpuSlot(slot));
}

void Renderer::resize(uint32_t width, uint32_t height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (width == width_ && height == height_) return;
    gpu_.waitIdle();
    width_ = width;
    height_ = height;
    createSizeDependent();
}

// ---------------------------------------------------------------------------
// Uploads
// ---------------------------------------------------------------------------

void Renderer::setMapImage(const Image& image, int cellPx, int marginPx) {
    if (image.empty()) return;
    mapCellPx_ = cellPx;
    mapMarginPx_ = marginPx;
    ID3D12Device5* device = gpu_.device();
    gpu_.waitIdle();
    const D3D12_RESOURCE_DESC td =
        textureDesc(uint32_t(image.width()), uint32_t(image.height()), kColorFormat, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    BH_CHECK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&texture)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> upload = createBuffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    BH_CHECK(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, image.data() + size_t(y) * image.rowPitch(),
                    image.rowPitch());
    upload->Unmap(0, nullptr);

    beginFrame();
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    cmd_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = transitionBarrier(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd_->ResourceBarrier(1, &b);
    submit();
    gpu_.waitIdle();

    mapTexture_ = texture;
    mapWidth_ = uint32_t(image.width());
    mapHeight_ = uint32_t(image.height());
    if (accum_[0]) writeDescriptors();
}

void Renderer::setKit(const Kit& kit) {
    ID3D12Device5* device = gpu_.device();
    gpu_.waitIdle();
    kit_ = &kit;

    const uint64_t vbSize = kit.vertices().size() * sizeof(KitVertex);
    const uint64_t ibSize = kit.indices().size() * sizeof(uint32_t);
    ComPtr<ID3D12Resource> vbUpload = uploadBuffer(kit.vertices().data(), vbSize);
    ComPtr<ID3D12Resource> ibUpload = uploadBuffer(kit.indices().data(), ibSize);
    vertexBuffer_ = createBuffer(vbSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    indexBuffer_ = createBuffer(ibSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);

    std::vector<GpuSubmesh> subs;
    for (const KitSubmesh& s : kit.submeshes()) subs.push_back({s.firstIndex, s.baseVertex, s.material, 0});
    submeshBuffer_ = uploadBuffer(subs.data(), subs.size() * sizeof(GpuSubmesh));
    std::vector<GpuMaterial> mats;
    for (const KitMaterial& m : kit.materials()) {
        GpuMaterial g{};
        g.baseAlpha[0] = m.baseColor[0], g.baseAlpha[1] = m.baseColor[1], g.baseAlpha[2] = m.baseColor[2];
        g.baseAlpha[3] = m.alpha;
        g.emissiveRough[0] = m.emissive[0], g.emissiveRough[1] = m.emissive[1], g.emissiveRough[2] = m.emissive[2];
        g.emissiveRough[3] = m.roughness;
        g.metalPad[0] = m.metallic;
        mats.push_back(g);
    }
    materialBuffer_ = uploadBuffer(mats.data(), mats.size() * sizeof(GpuMaterial));

    beginFrame();
    cmd_->CopyBufferRegion(vertexBuffer_.Get(), 0, vbUpload.Get(), 0, vbSize);
    cmd_->CopyBufferRegion(indexBuffer_.Get(), 0, ibUpload.Get(), 0, ibSize);
    D3D12_RESOURCE_BARRIER bs[2] = {
        transitionBarrier(vertexBuffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transitionBarrier(indexBuffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    cmd_->ResourceBarrier(2, bs);

    // one bottom-level acceleration structure per part
    blas_.clear();
    partTriangles_.clear();
    std::vector<ComPtr<ID3D12Resource>> scratch;
    uint64_t totalBytes = 0;
    for (const KitPart& part : kit.parts()) {
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms;
        uint64_t tris = 0;
        for (uint32_t i = 0; i < part.submeshCount; ++i) {
            const KitSubmesh& s = kit.submeshes()[part.firstSubmesh + i];
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexBuffer.StartAddress =
                vertexBuffer_->GetGPUVirtualAddress() + uint64_t(s.baseVertex) * sizeof(KitVertex);
            g.Triangles.VertexBuffer.StrideInBytes = sizeof(KitVertex);
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount = s.vertexCount;
            g.Triangles.IndexBuffer = indexBuffer_->GetGPUVirtualAddress() + uint64_t(s.firstIndex) * 4;
            g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount = s.indexCount;
            geoms.push_back(g);
            tris += s.indexCount / 3;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = UINT(geoms.size());
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = geoms.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        ComPtr<ID3D12Resource> result =
            createBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> s = createBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
        b.Inputs = in;
        b.DestAccelerationStructureData = result->GetGPUVirtualAddress();
        b.ScratchAccelerationStructureData = s->GetGPUVirtualAddress();
        cmd_->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
        totalBytes += info.ResultDataMaxSizeInBytes;
        blas_.push_back(result);
        scratch.push_back(s);
        partTriangles_.push_back(tris);
    }
    D3D12_RESOURCE_BARRIER all = uavBarrier(nullptr);
    cmd_->ResourceBarrier(1, &all);
    submit();
    gpu_.waitIdle();
    log::info("Kit on GPU: {} vertices, {} triangles, {} parts -> {} BLAS ({:.1f} MB)", kit.vertices().size(),
              kit.indices().size() / 3, kit.parts().size(), blas_.size(), double(totalBytes) / (1024.0 * 1024.0));
}

void Renderer::setLevel(const Level& level) {
    gpu_.waitIdle();
    lights_.clear();
    for (const WorldLight& l : level.lights)
        lights_.push_back({{l.position.x, l.position.y, l.position.z, l.radius},
                           {l.radiance.x, l.radiance.y, l.radiance.z, l.range}});
    std::vector<GpuCellInfo> info;
    const Sections& sec = level.sections;
    for (int i = 0; i < level.gridW * level.gridH; ++i) {
        GpuCellInfo c{};
        if (size_t(i) < sec.decay.size()) {
            c.decay = sec.decay[size_t(i)];
            c.haze = sec.haze[size_t(i)];
            c.mood = float(int(sec.moodAt(i % level.gridW, i / level.gridW)));
        }
        info.push_back(c);
    }
    if (info.empty()) info.push_back({});
    cellInfoBuffer_ = uploadBuffer(info.data(), info.size() * sizeof(GpuCellInfo));
    std::vector<uint32_t> cells;
    for (size_t i = 0; i < level.cellStart.size(); ++i) {
        cells.push_back(level.cellStart[i]);
        cells.push_back(level.cellCount[i]);
    }
    if (cells.empty()) cells = {0, 0};
    cellBuffer_ = uploadBuffer(cells.data(), cells.size() * sizeof(uint32_t));
    std::vector<uint32_t> idx = level.cellLights;
    if (idx.empty()) idx.push_back(0);
    cellLightBuffer_ = uploadBuffer(idx.data(), idx.size() * sizeof(uint32_t));
    gridW_ = float(level.gridW);
    gridH_ = float(level.gridH);
    lightCount_ = float(level.lights.size());
    levelSet_ = true;
    historyValid_ = false;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

void Renderer::beginFrame() {
    FrameContext& f = frames_[frameIndex_];
    gpu_.waitFor(f.fenceValue);
    BH_CHECK(f.allocator->Reset());
    BH_CHECK(cmd_->Reset(f.allocator.Get(), nullptr));
}

void Renderer::submit() {
    BH_CHECK(cmd_->Close());
    ID3D12CommandList* lists[] = {cmd_.Get()};
    gpu_.queue()->ExecuteCommandLists(1, lists);
    frames_[frameIndex_].fenceValue = gpu_.signal();
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void Renderer::transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
    if (state == to) return;
    const D3D12_RESOURCE_BARRIER b = transitionBarrier(r, state, to);
    cmd_->ResourceBarrier(1, &b);
    state = to;
}

void Renderer::ensureInstanceCapacity(FrameContext& f, uint32_t count) {
    if (count <= f.capacity) return;
    uint32_t cap = std::max(count + count / 2, 256u);
    if (f.instanceDescs) {
        // other frame may still use its own buffers; ours are idle after beginFrame's wait
        f.instanceDescs->Unmap(0, nullptr);
        f.instanceInfo->Unmap(0, nullptr);
    }
    f.instanceDescs = createBuffer(uint64_t(cap) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    f.instanceInfo = createBuffer(uint64_t(cap) * sizeof(GpuInstanceInfo), D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    BH_CHECK(f.instanceDescs->Map(0, nullptr, &f.instanceDescsPtr));
    BH_CHECK(f.instanceInfo->Map(0, nullptr, &f.instanceInfoPtr));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.NumDescs = cap;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    gpu_.device()->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    f.tlas = createBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    f.tlasScratch = createBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    f.capacity = cap;
}

void Renderer::recordScene(const std::vector<SceneInstance>& instances, const FrameSettings& s) {
    if (!kit_ || !levelSet_) throw std::logic_error("Renderer: setKit and setLevel before rendering");
    FrameContext& f = frames_[frameIndex_];
    const uint32_t n = uint32_t(instances.size());
    ensureInstanceCapacity(f, std::max(n, 1u));

    // --- per-frame lights (current radiance) and explored cells ---
    // This frame context's buffers are idle: beginFrame waited for its fence.
    auto ensure = [&](ComPtr<ID3D12Resource>& buf, void*& ptr, uint32_t& cap, uint32_t count, size_t stride) {
        if (buf && count <= cap) return;
        if (buf) buf->Unmap(0, nullptr);
        cap = std::max(count + count / 4, 64u);
        buf = createBuffer(uint64_t(cap) * stride, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        BH_CHECK(buf->Map(0, nullptr, &ptr));
    };
    {
        const uint32_t lc = uint32_t(std::max<size_t>(lights_.size(), 1));
        ensure(f.lights, f.lightsPtr, f.lightsCapacity, lc, sizeof(GpuLight));
        auto* dst = static_cast<GpuLight*>(f.lightsPtr);
        if (lights_.empty()) dst[0] = {};
        const bool live = s.lightRadiance && s.lightRadiance->size() == lights_.size();
        for (size_t i = 0; i < lights_.size(); ++i) {
            dst[i] = lights_[i];
            if (live) {
                const Vec3& r = (*s.lightRadiance)[i];
                dst[i].radianceRange[0] = r.x, dst[i].radianceRange[1] = r.y, dst[i].radianceRange[2] = r.z;
            }
        }
        const uint32_t cells = uint32_t(std::max(1.0f, gridW_ * gridH_));
        ensure(f.explored, f.exploredPtr, f.exploredCapacity, cells, sizeof(uint32_t));
        auto* ex = static_cast<uint32_t*>(f.exploredPtr);
        const bool fog = s.explored && s.explored->size() == cells;
        for (uint32_t i = 0; i < cells; ++i) ex[i] = fog ? ((*s.explored)[i] ? 1u : 0u) : 1u;

        const size_t dn = s.dynamicLights ? std::min<size_t>(s.dynamicLights->size(), kMaxDynLights) : 0;
        ensure(f.dynLights, f.dynLightsPtr, f.dynLightsCapacity, kMaxDynLights, sizeof(GpuLight));
        auto* dl = static_cast<GpuLight*>(f.dynLightsPtr);
        for (size_t i = 0; i < dn; ++i) {
            const DynamicLight& l = (*s.dynamicLights)[i];
            dl[i] = {{l.position.x, l.position.y, l.position.z, l.radius}, {l.radiance.x, l.radiance.y, l.radiance.z, l.range}};
        }
        dynCount_ = float(dn);
        const size_t sn = s.steam ? std::min<size_t>(s.steam->size(), 16) : 0;
        ensure(f.steam, f.steamPtr, f.steamCapacity, 16, sizeof(GpuSteam));
        auto* st = static_cast<GpuSteam*>(f.steamPtr);
        for (size_t i = 0; i < sn; ++i) {
            const SteamPlume& p = (*s.steam)[i];
            st[i] = {{p.origin.x, p.origin.y, p.origin.z, p.length},
                     {p.direction.x, p.direction.y, p.direction.z, p.strength},
                     {p.light.x, p.light.y, p.light.z, p.seed}};
        }
        steamCount_ = float(sn);
        const size_t bn = s.scan.blips ? std::min<size_t>(s.scan.blips->size(), kMaxBlips) : 0;
        ensure(f.blips, f.blipsPtr, f.blipsCapacity, kMaxBlips, sizeof(float) * 4);
        auto* bl = static_cast<float*>(f.blipsPtr);
        for (size_t i = 0; i < bn; ++i) {
            const ScanBlip& b = (*s.scan.blips)[i];
            bl[i * 4 + 0] = b.x, bl[i * 4 + 1] = b.z, bl[i * 4 + 2] = b.radius, bl[i * 4 + 3] = b.alpha;
        }
        blipCount_ = float(bn);
    }

    // --- top-level acceleration structure ---
    auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(f.instanceDescsPtr);
    auto* infos = static_cast<GpuInstanceInfo*>(f.instanceInfoPtr);
    uint64_t tris = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const SceneInstance& si = instances[i];
        const KitPart& part = kit_->parts()[si.part];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) d.Transform[r][c] = si.transform.m[r][c];
        d.InstanceID = i;
        d.InstanceMask = (si.flags & kInstanceGun)   ? kMaskGun
                         : (si.flags & kInstanceFx) ? kMaskFx
                         : part.transparent         ? kMaskGlass
                                                    : kMaskWorld;
        d.InstanceContributionToHitGroupIndex = 0;
        // glTF fronts are counter-clockwise in a right-handed space; DXR's default winding
        // test (clockwise, left-handed convention) already matches them.
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure = blas_[si.part]->GetGPUVirtualAddress();
        descs[i] = d;
        infos[i] = {part.firstSubmesh, si.flags, 0, 0, {si.emissive.x, si.emissive.y, si.emissive.z, 0.0f}};
        tris += partTriangles_[si.part];
    }
    lastSceneTriangles_ = tris;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
    b.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    b.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    b.Inputs.NumDescs = n;
    b.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    b.Inputs.InstanceDescs = f.instanceDescs->GetGPUVirtualAddress();
    b.DestAccelerationStructureData = f.tlas->GetGPUVirtualAddress();
    b.ScratchAccelerationStructureData = f.tlasScratch->GetGPUVirtualAddress();
    cmd_->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    D3D12_RESOURCE_BARRIER tb = uavBarrier(f.tlas.Get());
    cmd_->ResourceBarrier(1, &tb);

    // --- trace ---
    const float aspect = float(width_) / float(height_);
    const int samples = std::clamp(s.samples, 1, int(kMaxSamples));
    const bool still = samples > 1;
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    for (int k = 0; k < samples; ++k) {
        const uint32_t cur = last_ ^ 1u;
        TraceConstants c{};
        setCamera(c.camPos, c.camRight, c.camUp, c.camFwd, s.camera, aspect);
        const Camera& prev = historyValid_ ? prevCamera_ : s.camera;
        setCamera(c.prevPos, c.prevRight, c.prevUp, c.prevFwd, prev, aspect);
        c.resolution[0] = float(width_);
        c.resolution[1] = float(height_);
        c.resolution[2] = 1.0f / float(width_);
        c.resolution[3] = 1.0f / float(height_);
        const uint32_t seq = uint32_t(frameCounter_ % 1024u) + 1u;
        // a slightly wider-than-pixel filter tames moire on the fine floor gratings
        c.jitter[0] = (halton(seq, 2) - 0.5f) * 1.5f;
        c.jitter[1] = (halton(seq, 3) - 0.5f) * 1.5f;
        c.jitter[2] = float(frameCounter_ % 100000u);
        float mode;
        if (still) mode = (k == 0) ? 1.0f : 1.0f / float(k + 1);
        else mode = (historyValid_ && !s.resetHistory) ? -1.0f : 1.0f;
        c.jitter[3] = mode;
        c.grid[0] = gridW_;
        c.grid[1] = gridH_;
        c.grid[2] = kCell;
        c.grid[3] = lightCount_;
        c.options[0] = s.rayTracing ? 1.0f : 0.0f;
        c.options[1] = 0.012f;
        c.options[2] = float(std::max(1, s.maxHistory));
        c.options[3] = dynCount_;
        c.flashPos[0] = s.flashPos.x, c.flashPos[1] = s.flashPos.y, c.flashPos[2] = s.flashPos.z;
        c.flashPos[3] = s.flashlight ? s.flashIntensity : 0.0f;
        c.flashDir[0] = s.flashDir.x, c.flashDir[1] = s.flashDir.y, c.flashDir[2] = s.flashDir.z;
        c.flashDir[3] = s.flashCosOuter;
        c.flashCone[0] = s.flashCosInner;
        c.flashCone[1] = std::cos(std::acos(std::clamp(s.flashCosOuter, -1.0f, 1.0f)) * 1.9f);  // spill edge
        c.flashCone[2] = 30.0f;  // range, m
        c.flashCone[3] = steamCount_;
        c.haze[0] = s.haze;
        c.haze[1] = 0.2f;        // Henyey-Greenstein g: a little forward scattering
        c.haze[2] = s.time;
        // lights picked per pixel (0: all of them), plus 8 for the plain estimator
        c.haze[3] = float(std::clamp(s.lightPicks, 0, 4) + (s.lightPicks > 0 && s.plainEstimator ? 8 : 0));
        std::memcpy(f.constantsPtr + size_t(k) * 256, &c, sizeof(c));

        transition(accum_[cur].Get(), accumState_[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(pos_[cur].Get(), posState_[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(accum_[last_].Get(), accumState_[last_], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(pos_[last_].Get(), posState_[last_], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmd_->SetComputeRootSignature(traceRS_.Get());
        cmd_->SetPipelineState(tracePSO_.Get());
        cmd_->SetComputeRootConstantBufferView(0, f.constants->GetGPUVirtualAddress() + uint64_t(k) * 256);
        cmd_->SetComputeRootShaderResourceView(1, f.tlas->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(2, vertexBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(3, indexBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(4, submeshBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(5, f.instanceInfo->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(6, materialBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(7, f.lights->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(8, cellBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(9, cellLightBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootDescriptorTable(10, gpuSlot(kSlotTrace + cur * 4));
        cmd_->SetComputeRootShaderResourceView(11, cellInfoBuffer_->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(12, f.dynLights->GetGPUVirtualAddress());
        cmd_->SetComputeRootShaderResourceView(13, f.steam->GetGPUVirtualAddress());
        cmd_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
        last_ = cur;
        ++frameCounter_;
    }
    historyValid_ = true;
    prevCamera_ = s.camera;

    // --- present: despeckle, tone map + HUD into the 8-bit target ---
    transition(accum_[last_].Get(), accumState_[last_], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(target_.Get(), targetState_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(width_), LONG(height_)};
    cmd_->RSSetViewports(1, &vp);
    cmd_->RSSetScissorRects(1, &scissor);
    PresentConstants pc{};
    pc.p0[0] = s.exposure;
    pc.p0[1] = s.hud ? 1.0f : 0.0f;
    pc.p0[2] = s.time;
    pc.p0[3] = s.exitFlash;
    pc.p1[0] = float(width_);
    pc.p1[1] = float(height_);
    pc.p1[2] = float(mapWidth_) / float(mapHeight_);
    pc.p1[3] = s.bigMap;
    pc.p2[0] = s.playerMapUV.x;
    pc.p2[1] = s.playerMapUV.y;
    pc.p2[2] = s.camera.yaw;
    pc.p2[3] = s.minimap ? 1.0f : 0.0f;
    // stronger in the window (1 sample per frame), gentle on converged captures
    pc.p3[0] = samples > 1 ? 2.5f : 1.6f;
    pc.p3[1] = s.redout;
    pc.p3[2] = s.blackout;
    // map grid: margin and cell size as fractions of the map image
    const bool grid = mapCellPx_ > 0;
    pc.p4[0] = grid ? float(mapMarginPx_) / float(mapWidth_) : 0.0f;
    pc.p4[1] = grid ? float(mapMarginPx_) / float(mapHeight_) : 0.0f;
    pc.p4[2] = grid ? float(mapCellPx_) / float(mapWidth_) : 1.0f;
    pc.p4[3] = grid ? float(mapCellPx_) / float(mapHeight_) : 1.0f;
    pc.p5[0] = gridW_;
    pc.p5[1] = gridH_;
    pc.p5[2] = (grid && s.explored) ? 1.0f : 0.0f;
    pc.p5[3] = grid ? s.minimapCells : 0.0f;
    // scanner (needs the map grid to place things in metres)
    pc.p6[0] = grid ? blipCount_ : 0.0f;
    pc.p6[1] = grid ? s.scan.front : -1.0f;
    pc.p6[2] = s.scan.frontAlpha;
    pc.p6[3] = (grid && s.scan.blips) ? s.scan.range : 0.0f;  // (no blip list: the scanner is off)
    cmd_->SetGraphicsRootSignature(presentRS_.Get());
    cmd_->SetPipelineState(presentPSO_.Get());
    cmd_->SetGraphicsRoot32BitConstants(0, kPresentConstantCount, &pc, 0);
    cmd_->SetGraphicsRootDescriptorTable(1, gpuSlot(kSlotPresent + last_ * 2));
    cmd_->SetGraphicsRootShaderResourceView(2, f.explored->GetGPUVirtualAddress());
    cmd_->SetGraphicsRootShaderResourceView(3, f.blips->GetGPUVirtualAddress());
    cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_->DrawInstanced(3, 1, 0, 0);
    transition(target_.Get(), targetState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void Renderer::renderAndPresent(const std::vector<SceneInstance>& instances, const FrameSettings& settings) {
    if (!swapChain_) throw std::logic_error("renderAndPresent needs a window");
    beginFrame();
    recordScene(instances, settings);
    ID3D12Resource* back = backBuffers_[swapChain_->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER b = transitionBarrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd_->ResourceBarrier(1, &b);
    cmd_->CopyResource(back, target_.Get());
    b = transitionBarrier(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    cmd_->ResourceBarrier(1, &b);
    BH_CHECK(cmd_->Close());
    ID3D12CommandList* lists[] = {cmd_.Get()};
    gpu_.queue()->ExecuteCommandLists(1, lists);
    BH_CHECK(swapChain_->Present(1, 0));
    frames_[frameIndex_].fenceValue = gpu_.signal();
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void Renderer::render(const std::vector<SceneInstance>& instances, const FrameSettings& settings) {
    beginFrame();
    recordScene(instances, settings);
    submit();
}

Image Renderer::renderToImage(const std::vector<SceneInstance>& instances, const FrameSettings& settings) {
    beginFrame();
    recordScene(instances, settings);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = readbackFootprint_;
    cmd_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const uint32_t submitted = frameIndex_;
    submit();
    gpu_.waitFor(frames_[submitted].fenceValue);

    Image img{int(width_), int(height_)};
    void* mappedVoid = nullptr;
    // the last row is not padded to the row pitch
    const D3D12_RANGE readRange{0, size_t(readbackFootprint_.Offset) +
                                       size_t(readbackFootprint_.Footprint.RowPitch) * (height_ - 1) +
                                       size_t(width_) * 4};
    BH_CHECK(readback_->Map(0, &readRange, &mappedVoid));
    const uint8_t* mapped = static_cast<const uint8_t*>(mappedVoid);
    for (uint32_t y = 0; y < height_; ++y)
        std::memcpy(img.data() + size_t(y) * img.rowPitch(),
                    mapped + readbackFootprint_.Offset + size_t(y) * readbackFootprint_.Footprint.RowPitch,
                    img.rowPitch());
    const D3D12_RANGE noWrite{0, 0};
    readback_->Unmap(0, &noWrite);
    return img;
}

}  // namespace bh
