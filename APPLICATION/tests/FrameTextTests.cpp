#include "dx12/FrameTextOverlay.h"
#include "ui/NameTags.h"
#include <dxgi1_6.h>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <iostream>

using Microsoft::WRL::ComPtr;
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("Frame text GPU test failed: "+std::to_string(result)); }
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }

int main(int argc,char** argv) try {
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    UINT64 fence_value=0;
    const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    require(event!=nullptr,"Could not create test fence event");
    gmod::dx12::FrameTextOverlay text(device.Get(),320,180,3);
    auto render=[&](UINT width,UINT height,UINT frame,const gmod::dx12::FrameTextOverlay::Painter& painter) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
        desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> target;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
        ComPtr<ID3D12DescriptorHeap> rtv;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));
        const auto handle=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,handle);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto readback=gmod::dx12::create_buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        const float clear[]{17/255.0f,34/255.0f,51/255.0f,1};list->ClearRenderTargetView(handle,clear,0,nullptr);
        text.draw(list.Get(),frame,handle,painter);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=target.Get();barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);check(list->Close());
        ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++fence_value));check(fence->SetEventOnCompletion(fence_value,event));
        require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"Frame text GPU timed out");
        void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};check(readback->Map(0,&range,&mapped));
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width)*height);
        for (UINT row=0;row<height;++row) std::memcpy(pixels.data()+static_cast<std::size_t>(row)*width,
            static_cast<const unsigned char*>(mapped)+footprint.Offset+static_cast<std::size_t>(row)*footprint.Footprint.RowPitch,width*4);
        const D3D12_RANGE empty{};readback->Unmap(0,&empty);
        return pixels;
    };
    auto names=[](HDC dc,int width,int height) {
        gmod::ui::NameTag tag;
        tag.x=width/2.0f;tag.y=height-24.0f;tag.color=RGB(255,0,255);
        tag.name=L"Captured Player";tag.steam=L"Steam name";tag.job=L"Officer";tag.status=L"AIMING";
        gmod::ui::NameTags::paint(dc,width,height,{tag});return true;
    };
    const auto pixels=render(320,180,0,names);
    require(std::count(pixels.begin(),pixels.end(),0xffff00ffu)>20,"Player-name colour missing from raw GPU render target");
    require(std::count(pixels.begin(),pixels.end(),0xff000000u)>20,"Black text outline missing from raw GPU render target");
    require(pixels.front()==0xff332211u,"Colour-key background covered the scene");
    if (argc==2) {
        auto bgr=pixels;
        for (auto& pixel:bgr) pixel=(pixel&0xff00ff00u)|((pixel&0xffu)<<16)|((pixel>>16)&0xffu);
        BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
        file.bfSize=file.bfOffBits+static_cast<DWORD>(bgr.size()*4);
        BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=320;info.biHeight=-180;info.biPlanes=1;info.biBitCount=32;
        std::ofstream output(argv[1],std::ios::binary);output.write(reinterpret_cast<const char*>(&file),sizeof(file));
        output.write(reinterpret_cast<const char*>(&info),sizeof(info));output.write(reinterpret_cast<const char*>(bgr.data()),bgr.size()*4);
    }
    for (const auto& blank : {render(320,180,1,[](HDC,int,int){return false;}),render(320,180,2,[](HDC,int,int){return true;})})
        require(std::all_of(blank.begin(),blank.end(),[](auto pixel){return pixel==0xff332211u;}),"Disabled/empty ESP left stale labels in the frame");
    text.resize(321,187);
    const auto resized=render(321,187,0,names);
    require(std::count(resized.begin(),resized.end(),0xffff00ffu)>20,"Odd-size resize lost captured labels");
    text.resize(320,180);
    require(render(320,180,1,names)==pixels,"Frame slot/resize restoration changed label pixels");
    CloseHandle(event);
    std::cout << "PASS: ESP labels/black outlines in raw GPU frame, transparent background, disable/clear, all frame slots and odd-size resize\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
