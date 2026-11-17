#include "render/Renderer.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

#include "core/Log.h"

// DXIL bytecode generated at build time by dxc (see cmake/Shaders.cmake).
#include "g_FullscreenPS.h"
#include "g_FullscreenVS.h"

namespace bh {
namespace {

// Must match the cbuffer in shaders/fullscreen.hlsl (root constants, register b0).
struct ShaderConstants {
    float resolution[2];
    float time;
    float mapOpacity;
    float mapRect[4];  // x0, y0, x1, y1 in pixels
    float marker[2];   // map UV
    float markerVisible;
    float markerRadius;  // pixels
};
static_assert(sizeof(ShaderConstants) == 48, "root constant layout");
constexpr UINT kConstantCount = sizeof(ShaderConstants) / 4;

D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    return h;
}

D3D12_RESOURCE_DESC bufferDesc(uint64_t size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
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

}  // namespace

Renderer::Renderer(Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height)
    : gpu_(gpu), hwnd_(hwnd), width_(std::max(1u, width)), height_(std::max(1u, height)) {
    ID3D12Device5* device = gpu_.device();

    for (FrameContext& f : frames_)
        BH_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)));
    BH_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&cmd_)));
    BH_CHECK(cmd_->Close());

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = 1;
    BH_CHECK(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap_)));

    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = 1;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    BH_CHECK(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srvHeap_)));

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

    createPipeline();
    createSizeDependent();
    setMapImage(Image(1, 1, rgb(0x000000)));
}

Renderer::~Renderer() { gpu_.waitIdle(); }

void Renderer::createPipeline() {
    ID3D12Device5* device = gpu_.device();

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kConstantCount;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
        throw std::runtime_error(std::string("Root signature: ") + msg);
    }
    BH_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&rootSignature_)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_.Get();
    pso.VS = {g_FullscreenVS, sizeof(g_FullscreenVS)};
    pso.PS = {g_FullscreenPS, sizeof(g_FullscreenPS)};
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
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.FrontFace = keep;
    pso.DepthStencilState.BackFace = keep;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kColorFormat;
    pso.SampleDesc.Count = 1;
    BH_CHECK(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_)));
}

void Renderer::createSizeDependent() {
    ID3D12Device5* device = gpu_.device();

    if (swapChain_) {
        for (auto& b : backBuffers_) b.Reset();
        BH_CHECK(swapChain_->ResizeBuffers(kSwapBuffers, width_, height_, kColorFormat, 0));
        for (UINT i = 0; i < kSwapBuffers; ++i) BH_CHECK(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])));
    }

    // Offscreen colour target.
    target_.Reset();
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC td =
        textureDesc(width_, height_, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kColorFormat;
    BH_CHECK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target_)));
    target_->SetName(L"Offscreen colour");
    targetState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    device->CreateRenderTargetView(target_.Get(), nullptr, rtvHeap_->GetCPUDescriptorHandleForHeapStart());

    // Readback buffer laid out like the target (rows padded to 256 bytes).
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &readbackFootprint_, nullptr, nullptr, &total);
    readback_.Reset();
    const D3D12_HEAP_PROPERTIES readbackHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC bd = bufferDesc(total);
    BH_CHECK(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)));
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

void Renderer::setMapImage(const Image& image) {
    if (image.empty()) return;
    ID3D12Device5* device = gpu_.device();
    gpu_.waitIdle();

    const D3D12_RESOURCE_DESC td =
        textureDesc(uint32_t(image.width()), uint32_t(image.height()), kColorFormat, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    BH_CHECK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&texture)));
    texture->SetName(L"Overhead map");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    const D3D12_HEAP_PROPERTIES uploadHeap = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bd = bufferDesc(total);
    ComPtr<ID3D12Resource> upload;
    BH_CHECK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
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
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    cmd_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = transitionBarrier(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd_->ResourceBarrier(1, &b);
    submit();
    gpu_.waitIdle();  // the upload buffer is released when this function returns

    mapTexture_ = texture;
    mapWidth_ = uint32_t(image.width());
    mapHeight_ = uint32_t(image.height());

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = kColorFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mapTexture_.Get(), &sv, srvHeap_->GetCPUDescriptorHandleForHeapStart());
}

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

void Renderer::transitionTarget(D3D12_RESOURCE_STATES to) {
    if (targetState_ == to) return;
    const D3D12_RESOURCE_BARRIER b = transitionBarrier(target_.Get(), targetState_, to);
    cmd_->ResourceBarrier(1, &b);
    targetState_ = to;
}

void Renderer::recordScene(const FrameParams& p) {
    transitionTarget(D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(width_), LONG(height_)};
    cmd_->RSSetViewports(1, &vp);
    cmd_->RSSetScissorRects(1, &scissor);

    // Fit the map panel inside the frame, centred.
    const float scale = std::min(width_ * 0.92f / float(mapWidth_), height_ * 0.86f / float(mapHeight_));
    const float mw = mapWidth_ * scale, mh = mapHeight_ * scale;
    const float mx = (width_ - mw) * 0.5f, my = (height_ - mh) * 0.5f;

    ShaderConstants c{};
    c.resolution[0] = float(width_);
    c.resolution[1] = float(height_);
    c.time = p.time;
    c.mapOpacity = p.mapOpacity;
    c.mapRect[0] = mx;
    c.mapRect[1] = my;
    c.mapRect[2] = mx + mw;
    c.mapRect[3] = my + mh;
    c.marker[0] = p.markerU;
    c.marker[1] = p.markerV;
    c.markerVisible = p.markerVisible;
    c.markerRadius = std::max(5.0f, height_ * 0.011f);

    cmd_->SetPipelineState(pipeline_.Get());
    cmd_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    cmd_->SetGraphicsRoot32BitConstants(0, kConstantCount, &c, 0);
    cmd_->SetGraphicsRootDescriptorTable(1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
    cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_->DrawInstanced(3, 1, 0, 0);
}

void Renderer::renderAndPresent(const FrameParams& params) {
    if (!swapChain_) throw std::logic_error("renderAndPresent needs a window");
    beginFrame();
    recordScene(params);
    transitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);

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

Image Renderer::renderToImage(const FrameParams& params) {
    beginFrame();
    recordScene(params);
    transitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
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
    const D3D12_RANGE readRange{0, size_t(readbackFootprint_.Footprint.RowPitch) * height_};
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
