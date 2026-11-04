#include "dx12/FrameTextOverlay.h"
#include <d3dcompiler.h>
#include <cstring>
#include <limits>
#include <utility>

namespace gmod::dx12 {
namespace {
void check(HRESULT hr,const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(static_cast<unsigned long>(hr)));
}
ComPtr<ID3DBlob> shader(const char* source,const char* target) {
    ComPtr<ID3DBlob> code,error;
    const auto result=D3DCompile(source,std::strlen(source),"frame_text",nullptr,nullptr,"main",target,
        D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if (FAILED(result)) throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "Frame text shader compilation failed");
    return code;
}
}

FrameTextOverlay::FrameTextOverlay(ID3D12Device* device,UINT width,UINT height,UINT frame_count)
    :device_(device),frame_count_(frame_count) {
    if (!device || !frame_count) throw std::runtime_error("Invalid frame text device/slot count");
    try { create_pipeline();resize(width,height); }
    catch (...) { release_surface();throw; }
}
FrameTextOverlay::~FrameTextOverlay() { release_surface(); }
void FrameTextOverlay::release_surface() {
    if (dc_ && previous_) SelectObject(dc_,previous_);
    if (bitmap_) DeleteObject(bitmap_);
    if (dc_) DeleteDC(dc_);
    dc_=nullptr;bitmap_=nullptr;previous_=nullptr;pixels_=nullptr;
    for (std::size_t i=0;i<uploads_.size();++i)
        if (uploads_[i] && i<mapped_.size() && mapped_[i]) uploads_[i]->Unmap(0,nullptr);
    uploads_.clear();mapped_.clear();texture_.Reset();
}
void FrameTextOverlay::create_pipeline() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable={1,&range};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters=1;root.pParameters=&parameter;
    ComPtr<ID3DBlob> data,error;
    check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&data,&error),"Serialize frame text root");
    check(device_->CreateRootSignature(0,data->GetBufferPointer(),data->GetBufferSize(),IID_PPV_ARGS(&root_)),"Create frame text root");
    const auto vertex=shader(R"(
        float4 main(uint id:SV_VertexID):SV_POSITION {
            float2 p=float2((id<<1)&2,id&2);
            return float4(p.x*2-1,1-p.y*2,0,1);
        })","vs_5_0");
    const auto pixel=shader(R"(
        Texture2D text:register(t0);
        float4 main(float4 position:SV_POSITION):SV_TARGET {
            float3 color=text.Load(int3(int2(position.xy),0)).rgb;
            // Match the existing colour-keyed label renderer. Black outlines
            // remain opaque; only the unused background colour is transparent.
            if (all(abs(color-float3(2,1,3)/255.0)<0.5/255.0)) discard;
            return float4(color,1);
        })","ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature=root_.Get();
    pso.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
    pso.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    auto& blend=pso.BlendState.RenderTarget[0];
    blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
    blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    pso.SampleMask=std::numeric_limits<UINT>::max();
    pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable=TRUE;
    pso.DepthStencilState.DepthEnable=FALSE;pso.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    auto& stencil=pso.DepthStencilState.FrontFace;
    stencil.StencilFailOp=stencil.StencilDepthFailOp=stencil.StencilPassOp=D3D12_STENCIL_OP_KEEP;
    stencil.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;pso.DepthStencilState.BackFace=stencil;
    pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets=1;pso.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pso.SampleDesc.Count=1;
    check(device_->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipeline_)),"Create frame text pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=1;
    heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors_)),"Create frame text descriptors");
}
void FrameTextOverlay::resize(UINT width,UINT height) {
    if (!width || !height) throw std::runtime_error("Invalid frame text dimensions");
    release_surface();width_=width;height_=height;
    dc_=CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=static_cast<LONG>(width);info.bmiHeader.biHeight=-static_cast<LONG>(height);
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,&pixels_,nullptr,0);
    if (!dc_ || !bitmap_ || !pixels_) throw std::runtime_error("Create frame text surface failed");
    previous_=SelectObject(dc_,bitmap_);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    check(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&texture_)),"Create frame text texture");
    UINT64 bytes=0;
    device_->GetCopyableFootprints(&desc,0,1,0,&footprint_,nullptr,nullptr,&bytes);
    uploads_.resize(frame_count_);mapped_.resize(frame_count_,nullptr);
    for (UINT frame=0;frame<frame_count_;++frame) {
        uploads_[frame]=create_buffer(device_.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RANGE empty{};
        check(uploads_[frame]->Map(0,&empty,reinterpret_cast<void**>(&mapped_[frame])),"Map frame text upload");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(texture_.Get(),&view,descriptors_->GetCPUDescriptorHandleForHeapStart());
}
bool FrameTextOverlay::draw(ID3D12GraphicsCommandList* commands,UINT frame,D3D12_CPU_DESCRIPTOR_HANDLE target,const Painter& painter) {
    if (!painter) return false;
    const int saved=SaveDC(dc_);
    const RECT rect{0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};
    const HBRUSH clear=CreateSolidBrush(RGB(2,1,3));
    FillRect(dc_,&rect,clear);DeleteObject(clear);
    bool painted=false;
    try { painted=painter(dc_,static_cast<int>(width_),static_cast<int>(height_)); }
    catch (...) { RestoreDC(dc_,saved);throw; }
    RestoreDC(dc_,saved);
    if (!painted) return false;
    GdiFlush();
    auto* destination=mapped_.at(frame)+footprint_.Offset;
    const auto* source=static_cast<const unsigned char*>(pixels_);
    for (UINT row=0;row<height_;++row)
        std::memcpy(destination+static_cast<std::size_t>(row)*footprint_.Footprint.RowPitch,
            source+static_cast<std::size_t>(row)*width_*4,static_cast<std::size_t>(width_)*4);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=texture_.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    commands->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};
    from.pResource=uploads_[frame].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint_;
    to.pResource=texture_.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);
    commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
    commands->SetGraphicsRootSignature(root_.Get());commands->SetPipelineState(pipeline_.Get());
    ID3D12DescriptorHeap* heaps[]{descriptors_.Get()};commands->SetDescriptorHeaps(1,heaps);
    commands->SetGraphicsRootDescriptorTable(0,descriptors_->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};
    const D3D12_RECT scissor{0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};
    commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3,1,0,0);
    return true;
}
}
