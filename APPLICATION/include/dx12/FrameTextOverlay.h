#pragma once
#include "dx12/ResourceHelpers.h"
#include <windows.h>
#include <functional>
#include <vector>

namespace gmod::dx12 {

// Rasterize existing GDI text into the actual swap-chain frame, before Present.
// The owner waits for each frame slot's fence before draw, and idles the GPU
// before resizing/destroying resources. No overlay HWND is created.
class FrameTextOverlay {
public:
    using Painter=std::function<bool(HDC,int,int)>;
    FrameTextOverlay(ID3D12Device* device,UINT width,UINT height,UINT frame_count);
    ~FrameTextOverlay();
    FrameTextOverlay(const FrameTextOverlay&)=delete;
    FrameTextOverlay& operator=(const FrameTextOverlay&)=delete;
    void resize(UINT width,UINT height);
    bool draw(ID3D12GraphicsCommandList* commands,UINT frame,D3D12_CPU_DESCRIPTOR_HANDLE target,const Painter& painter);

private:
    void create_pipeline();
    void release_surface();
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12DescriptorHeap> descriptors_;
    ComPtr<ID3D12Resource> texture_;
    std::vector<ComPtr<ID3D12Resource>> uploads_;
    std::vector<unsigned char*> mapped_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT width_=0,height_=0,frame_count_=0;
    HDC dc_=nullptr;
    HBITMAP bitmap_=nullptr;
    HGDIOBJ previous_=nullptr;
    void* pixels_=nullptr;
};
}
