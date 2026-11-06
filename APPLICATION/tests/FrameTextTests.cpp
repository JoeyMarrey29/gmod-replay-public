#include "dx12/FrameTextOverlay.h"
#include "ui/NameTags.h"
#include "ui/Hud.h"
#include "ui/PlayerListPanel.h"
#include "ui/PlayerCard.h"
#include "ui/TimelineBar.h"
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

    text.resize(960,540);
    const auto blank = render(960,540,0,[](HDC,int,int){return true;});
    gmod::ui::HudState hud;
    hud.spectating = L"Steam Player - Character Name";
    const auto spectator = [&](HDC dc,int width,int height) {
        gmod::ui::Hud::paint(dc,width,height,hud);return true;
    };
    const auto watched = render(960,540,1,spectator);
    require(std::count(watched.begin(),watched.begin()+960*36,0xfff4eeecu)>20,
            "Spectator text missing from top-left raw GPU frame without ESP");
    require(watched.back()==blank.back(),"Spectator label covered the rest of the scene");
    const auto before = hud;
    hud.spectating = L"Other Steam Player - Other Character";
    require(!(before==hud),"Changing followed player did not invalidate HUD state");
    require(render(960,540,2,spectator)!=watched,"Switching POV left the previous spectator label");
    hud.spectating.clear();
    require(render(960,540,0,spectator)==blank,"Free view left a stale spectator label");
    hud.spectating = std::wstring(500,L'W');
    const auto long_name = render(960,540,1,spectator);
    require(std::equal(long_name.begin()+960*36,long_name.end(),blank.begin()+960*36),
            "Long spectator name wrapped or escaped its small header area");

    const HWND owner = CreateWindowExW(0,L"STATIC",L"Frame text test",WS_POPUP,
                                      0,0,960,540,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(owner!=nullptr,"Could not create frame text test owner");
    {
        int esp_mode=0;
        gmod::ui::TimelineActions actions;
        actions.get_esp=[&]{return esp_mode;};
        actions.set_esp=[&](int mode){esp_mode=mode;};
        gmod::ui::TimelineBar timeline(owner,{L"1x",L"2x"},actions);
        timeline.update({30,120,true,false,0,L"Steam Player",3600});
        const auto bar = [&](HDC dc,int width,int height) {
            if (!timeline.visible()) return false;
            timeline.paint_preview(dc,width,height);return true;
        };
        require(render(960,540,0,bar)==blank,"Closed Tab controls appeared in capture");
        timeline.set_visible(true);
        const auto controls = render(960,540,1,bar);
        require(std::count(controls.begin()+960*(540-88),controls.end(),0xff181210u)>1000,
                "Tab playback bar missing from raw GPU frame");
        ShowWindow(owner,SW_SHOWNOACTIVATE);
        timeline.update({90,120,false,false,1,L"Other Steam Player",3600});
        const HWND bar_window=FindWindowW(L"GmodReplayTimelineBar",nullptr);
        require(bar_window!=nullptr,"Playback bar window missing");
        SendMessageW(bar_window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(576,65));
        SendMessageW(bar_window,WM_LBUTTONUP,0,MAKELPARAM(576,65));
        require(esp_mode==1,"Playback bar ESP selector did not cycle to Outline");
        timeline.update({90,120,false,false,1,L"Other Steam Player",3600});
        const auto outline = render(960,540,2,bar);
        require(outline!=controls,"Playback bar ESP selection did not repaint");
        esp_mode=2;
        timeline.update({90,120,false,false,1,L"Other Steam Player",3600});
        require(render(960,540,0,bar)!=outline,"Through-walls ESP selection did not repaint");
        timeline.set_visible(false);
        ShowWindow(owner,SW_HIDE);
        require(render(960,540,0,bar)==blank,"Closing Tab left stale captured controls");

        gmod::ui::PlayerListPanel list(owner,{});
        std::vector<gmod::ui::PlayerListRow> rows(20);
        for (int i=0;i<20;++i) {
            rows[i].id=i;rows[i].name=L"Player "+std::to_wstring(i);
            rows[i].detail=L"Steam ID | HP 100";rows[i].status=L"In world";
            rows[i].present=true;rows[i].followed=i==0;
        }
        list.set_rows(rows,L"Players");
        const auto players = [&](HDC dc,int width,int height) {return list.paint_frame(dc,width,height);};
        require(render(960,540,0,players)==blank,"Closed P player list appeared in capture");
        list.set_visible(true);
        const auto scoreboard = render(960,540,1,players);
        require(std::count(scoreboard.begin(),scoreboard.end(),0xff262278u)>1000,
                "Followed player row missing from raw GPU frame");
        require(scoreboard.front()==blank.front() && scoreboard.back()==blank.back(),
                "Player list escaped its panel bounds");
        list.scroll_by(-WHEEL_DELTA);
        require(render(960,540,2,players)!=scoreboard,"Player list scroll not reflected in capture");
        rows[0].followed=false;rows[4].followed=true;
        list.set_rows(rows,L"Players - Other POV");
        require(render(960,540,0,players)!=scoreboard,"Player list selection not reflected in capture");
        list.set_visible(false);
        require(render(960,540,1,players)==blank,"Closing P left stale captured player information");

        int follows=0, closes=0;
        gmod::ui::PlayerCard card(owner,[&]{++follows;},[]{},[]{},[&]{++closes;});
        const auto information = [&](HDC dc,int width,int height) {return card.paint_frame(dc,width,height);};
        require(render(960,540,0,information)==blank,"Closed information card appeared in capture");
        gmod::ui::PlayerCardState state;
        state.visible=true;state.title=L"Character Name";state.rows={{L"Steam",L"Steam Player"},{L"Health",L"100"}};
        card.update(state);
        const auto details = render(960,540,1,information);
        require(std::count(details.begin(),details.end(),0xff00ff00u)>20,
                "Player info card title missing from raw GPU frame");
        require(details.front()==blank.front() && details.back()==blank.back(),
                "Player information card escaped its panel bounds");
        HWND card_window = nullptr;
        while ((card_window = FindWindowExW(nullptr,card_window,L"GModReplayPlayerCard",nullptr))) {
            if (GetWindow(card_window,GW_OWNER)==owner) break;
        }
        require(card_window && GetWindow(card_window,GW_OWNER)==owner,"Could not find owned playerinfo card");
        const auto click_card = [&](int x,int y) {
            SendMessageW(card_window,WM_LBUTTONDOWN,0,MAKELPARAM(x,y));
            SendMessageW(card_window,WM_LBUTTONUP,0,MAKELPARAM(x,y));
        };
        click_card(150,10);
        RECT collapsed_client{};
        GetClientRect(card_window,&collapsed_client);
        require(collapsed_client.bottom==42,"Minus did not shrink playerinfo to its header");
        const auto collapsed = render(960,540,2,information);
        require(collapsed!=blank,"Collapsed playerinfo header disappeared from capture");
        require(std::equal(collapsed.begin()+960*42,collapsed.end(),blank.begin()+960*42),
                "Collapsed playerinfo left stale details or action buttons");
        click_card(30,collapsed_client.bottom-15);
        require(follows==0 && closes==0,"Collapsed header triggered a hidden action button");
        require(render(960,540,1,information)==details,"Clicking collapsed header did not expand the card");
        click_card(355,5);
        card.update(state);
        require(render(960,540,0,information)==collapsed,"Card refresh expanded collapsed playerinfo");
        click_card(310,20);
        require(render(960,540,1,information)==details,"Plus did not restore playerinfo details");
        state.title=L"Other Character";state.rows={{L"Steam",L"Other Steam Player"}};
        card.update(state);
        require(render(960,540,2,information)!=details,"Player information changes not reflected in capture");
        state.visible=false;card.update(state);
        require(render(960,540,0,information)==blank,"Closing player card left stale captured information");
        state.visible=true;card.update(state);
        click_card(310,20);
        click_card(339,20);
        require(closes==0,"Removed playerinfo X still triggered close");
        state.visible=false;card.update(state);
        state.visible=true;card.update(state);
        GetClientRect(card_window,&collapsed_client);
        require(collapsed_client.bottom>42,"Reopening playerinfo did not restore expanded details");
        state.vehicle=true;
        card.update(state);
        click_card(100,10);
        GetClientRect(card_window,&collapsed_client);
        require(collapsed_client.bottom==42,"Vehicle header did not collapse");
        SendMessageW(card_window,WM_LBUTTONDBLCLK,0,MAKELPARAM(100,10));
        GetClientRect(card_window,&collapsed_client);
        require(collapsed_client.bottom==42,"Header double-click immediately undid collapse");
        click_card(339,20);
        require(closes==1,"Vehicle X did not retain its separate close action");
        GetClientRect(card_window,&collapsed_client);
        require(collapsed_client.bottom==42,"Vehicle X toggled collapse instead of closing");
        click_card(20,10);
        GetClientRect(card_window,&collapsed_client);
        require(collapsed_client.bottom>42,"Vehicle header did not expand");
        state.vehicle=false;card.update(state);
        closes=0;

        state.spectator=L"Steam Player - Other Character";
        card.update(state);
        POINT card_origin{};
        ClientToScreen(card_window,&card_origin);
        POINT owner_origin{};
        ClientToScreen(owner,&owner_origin);
        require(card_origin.x==owner_origin.x && card_origin.y==owner_origin.y,
                "Spectator card is not at the very top-left client pixel");
        const auto spectating_card=render(960,540,0,information);
        require(spectating_card.front()!=blank.front(),"Spectator card missing from top-left GPU pixel");
        click_card(150,10);
        GetClientRect(card_window,&collapsed_client);
        const auto collapsed_spectator=render(960,540,1,information);
        require(std::equal(collapsed_spectator.begin()+960*collapsed_client.bottom,collapsed_spectator.end(),
                           blank.begin()+960*collapsed_client.bottom),
                "Collapsed spectator card left stale player details");
        state.spectator=L"Other Steam Player - New Character";
        card.update(state);
        require(render(960,540,2,information)!=collapsed_spectator,
                "Switching spectator did not refresh collapsed card identity");
        click_card(339,20);
        require(closes==0,"Spectator card exposed a close action");
        click_card(150,10);
        click_card(310,20);
        state.spectator.clear();card.update(state);
        card_origin={};ClientToScreen(card_window,&card_origin);
        require(card_origin.x==owner_origin.x+960-360 && card_origin.y==owner_origin.y,
                "Clicked information card did not return to top-right");
        {
            gmod::ui::PlayerCard spectator_card(owner,[]{},[]{},[]{},[]{});
            auto followed=state;
            followed.spectator=L"Spectated Steam Player - Spectated Character";
            spectator_card.update(followed);
            const auto both=render(960,540,0,[&](HDC dc,int width,int height) {
                spectator_card.paint_frame(dc,width,height);
                return card.paint_frame(dc,width,height);
            });
            require(both.front()!=blank.front() && both[960-360]!=blank.front(),
                    "Spectator and clicked cards did not coexist in opposite corners");
            require(std::count(both.begin(),both.begin()+960*42,0xff00ff00u)>20,
                    "Clicked player name did not replace the old playerinfo header");
            const auto spectator_only = [&](HDC dc,int width,int height) {
                return spectator_card.paint_frame(dc,width,height);
            };
            const auto default_spectator=render(960,540,2,spectator_only);
            require(std::count(default_spectator.begin(),default_spectator.end(),0xff00ff00u)==0,
                    "Spectator card did not default collapsed");
            followed.title=L"New Character";
            followed.spectator=L"Steam Player - New Character";
            spectator_card.update(followed);
            const auto refreshed_spectator=render(960,540,0,spectator_only);
            require(std::count(refreshed_spectator.begin(),refreshed_spectator.end(),0xff00ff00u)==0,
                    "Switching player expanded the spectator card");
            followed.visible=false;spectator_card.update(followed);
            followed.visible=true;spectator_card.update(followed);
            require(render(960,540,1,spectator_only)==refreshed_spectator,
                    "Reopening spectator card did not default collapsed");
            state.visible=false;card.update(state);
            require(render(960,540,1,[&](HDC dc,int width,int height) {
                spectator_card.paint_frame(dc,width,height);
                card.paint_frame(dc,width,height);return true;
            })!=blank,"Closing clicked card also removed spectator card");
            state.visible=true;card.update(state);
        }

        hud.spectating=L"Steam Player - Character Name";
        timeline.set_visible(true);
        list.set_visible(true);
        state.visible=true;card.update(state);
        const auto combined = [&](HDC dc,int width,int height) {
            card.paint_frame(dc,width,height);
            list.paint_frame(dc,width,height);
            timeline.paint_preview(dc,width,height);
            gmod::ui::Hud::paint(dc,width,height,hud);
            return true;
        };
        const auto all = render(960,540,0,combined);
        require(std::count(all.begin(),all.begin()+960*36,0xfff4eeecu)>20,
                "Panel clipping leaked into the spectator label");
        require(std::count(all.begin()+960*(540-88),all.end(),0xff181210u)>1000,
                "Panel viewport leaked into the playback bar");
        text.resize(641,397);
        const auto resized_overlays = render(641,397,1,combined);
        require(std::count(resized_overlays.begin(),resized_overlays.begin()+641*36,0xfff4eeecu)>20,
                "Odd-size resize lost the spectator label");
        require(std::count(resized_overlays.begin()+641*(397-88),resized_overlays.end(),0xff181210u)>1000,
                "Odd-size resize lost the playback bar");
        text.resize(960,540);
        require(render(960,540,2,combined)==all,"Resize/frame-slot restoration changed combined overlay pixels");
    }
    DestroyWindow(owner);
    CloseHandle(event);
    std::cout << "PASS: ESP, spectator label, Tab controls, P list and info card in raw GPU frame; switching, scroll, hide/clear and resize\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
