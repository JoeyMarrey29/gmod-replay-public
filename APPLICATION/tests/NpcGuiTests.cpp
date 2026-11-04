#include "demo/DemoData.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool condition,const char* text) { if (!condition) throw std::runtime_error(text); }
int wmain(int argc,wchar_t** argv) {
    using namespace gmod::demo;
    try {
        DemoData data;
        data.npc_gui={{1,true,true,"NPC","Hello",{"Cars","Leave"}},
                      {2,false,false,"","",{}},
                      {2,true,true,"Garage","Select a vehicle",{"Vehicles","Close"}},
                      {3,false,false,"","",{}}};
        require(!npc_gui_at(data,0),"GUI appeared before interaction");
        require(npc_gui_at(data,1.5f)->text=="Hello","Initial dialogue missing");
        require(npc_gui_at(data,2)->title=="Garage" && npc_gui_at(data,2)->visible,
                "Same-tick replacement was closed with its previous page");
        require(!npc_gui_at(data,4)->visible,"Closed page remained visible");
        require(npc_gui_at(data,2.5f)->title=="Garage","Backward seek failed");
        require(npc_gui_at(data,1)->text=="Hello","Rewind failed");
        if (argc==3) {
            const std::atomic<bool> cancel{false};
            data=load_demo(argv[1],argv[2],[](float,const std::string&){},cancel);
            require(!data.npc_gui.empty(),"Parser did not supply GUI pages");
            require(std::is_sorted(data.npc_gui.begin(),data.npc_gui.end(),
                [](const auto& a,const auto& b){return a.time<b.time;}),"GUI timeline is out of order");
            require(!data.npc_uses.empty() && std::fabs(data.npc_uses[0].time-data.npc_gui[0].time)<0.01f,
                "GUI pages were not rescaled to the real demo tick rate");
            for (const auto* title : {"Garage","Storage","ATM","Druggy"})
                require(std::any_of(data.npc_gui.begin(),data.npc_gui.end(),[&](const auto& page) {
                    return page.visible && page.title==title && (!page.options.empty() || !page.text.empty());
                }),"Expected reconstructed interface missing from captured demo");
            std::cout << "PASS: captured demo GUI parsing and tick-rate correction, pages=" << data.npc_gui.size() << '\n';
        }
        std::cout << "PASS: GUI opens, replacements, explicit closes, pause and backward seeks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
}
