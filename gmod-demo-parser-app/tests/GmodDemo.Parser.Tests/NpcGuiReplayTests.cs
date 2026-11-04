using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class NpcGuiReplayTests
{
    private static readonly (string Path,string Text)[] Files=[
        ("gamemodes/darkrp/gamemode/modules/translation/en/sh_npcs.lua","""
            TranslationSystem:RegisterTranslations('en',{{'greeting','Welcome! How can I help?'},{'cars','Choose a car'},{'leave','Goodbye'}})
            """),
        ("npc.lua","""
            local NPC={}
            NPC.ID=5
            NPC.Name="Garage attendant"
            function NPC.OnTalk()
                GAMEMODE.DialogPanel:SetDialog(T('greeting'))
                GAMEMODE.DialogPanel:AddDialog(T('cars'),NPC.NewCar)
                GAMEMODE.DialogPanel:AddDialog(T('leave'),LEAVE_DIALOG)
                GAMEMODE.DialogPanel:Show()
            end
            function NPC.NewCar() GAMEMODE.ShowGarageView() LEAVE_DIALOG() end
            GAMEMODE:LoadNPC(NPC)
            """)];
    private static NetLogRow Open(int tick=10,int id=5)=>new() {T=tick,Type="lua",Name="perpheads_npc_used",Hex=Convert.ToHexString(BitConverter.GetBytes((ushort)id).Concat(new byte[2]).ToArray())};
    private static UserCommand Input(int tick,int[] keys,bool moving=false)
    {
        var bits=new List<bool>();
        void Put(uint value,int n) { for(int i=0;i<n;i++) bits.Add(((value>>i)&1)!=0); }
        Put(0,5); // command, tick and view-angle change flags
        Put(moving?1u:0u,1);
        if(moving) Put((uint)BitConverter.SingleToInt32Bits(100),32);
        Put(0,7); // other movement, buttons, impulse, weapon, mouse x/y
        Put(keys.Length>0?1u:0u,1);
        if(keys.Length>0) {
            foreach(int key in keys.Take(5)) Put((uint)key,8);
            if(keys.Length<5) Put(0,8);
        }
        Put(0,6);
        var bytes=new byte[(bits.Count+7)/8];
        for(int i=0;i<bits.Count;i++) if(bits[i]) bytes[i/8]|=(byte)(1<<(i%8));
        return new UserCommand(tick,0,1,bytes);
    }
    [Fact] public void Raw_gui_keys_survive_without_gameplay_attack_and_full_five_key_lists_need_no_terminator()
    {
        Assert.Equal([107],RecordedGuiInput.Read(Input(20,[107]))!.Keys);
        Assert.Equal([11,33,65,79,81],RecordedGuiInput.Read(Input(20,[11,33,65,79,81]))!.Keys);
        Assert.True(RecordedGuiInput.Read(Input(20,[],true))!.Moving);
        Assert.Null(RecordedGuiInput.Read(new UserCommand(0,0,0,[])));
    }
    [Fact] public void Opening_replacement_page_then_closing_dialogue_keeps_replacement_visible()
    {
        var replay=new NpcGuiReplay(); replay.Add(Input(12,[107])); replay.Add(Input(30,[],true));
        var pages=replay.Build(Files,[Open()],100);
        Assert.Equal("Welcome! How can I help?",pages[0].Text);
        Assert.Equal(["Choose a car","Goodbye"],pages[0].Options);
        Assert.True(pages[1].Visible);
        Assert.Equal("Garage",pages[1].Title);
        Assert.Equal("Select a vehicle",pages[1].Text);
        Assert.False(pages[^1].Visible); Assert.Equal(30,pages[^1].Tick);
    }
    [Fact] public void Held_click_does_not_repeatedly_advance_and_ambiguous_targets_are_not_guessed()
    {
        var files=Files.ToArray();
        files[1]=(files[1].Path,files[1].Text.Replace("GAMEMODE.DialogPanel:Show()","GAMEMODE.DialogPanel:AddDialog('Other action',NPC.NewCar) GAMEMODE.DialogPanel:Show()"));
        var replay=new NpcGuiReplay(); replay.Add(Input(12,[107]));replay.Add(Input(13,[107]));
        var pages=replay.Build(files,[Open()],100);
        Assert.Single(pages,p=>p.Visible);
        Assert.Equal(3,pages[0].Options.Length);
    }
    [Fact] public void Number_key_runs_recorded_choice_and_escape_dismisses_custom_page()
    {
        var replay=new NpcGuiReplay(); replay.Add(Input(12,[2]));replay.Add(Input(20,[70]));
        var pages=replay.Build(Files,[Open()],100);
        Assert.Equal("Garage",pages[1].Title);
        Assert.False(pages[^1].Visible);Assert.Equal(20,pages[^1].Tick);
    }
    [Fact] public void New_npc_replaces_previous_page_and_seeking_reads_snapshots_without_replaying_clicks()
    {
        var replay=new NpcGuiReplay();replay.Add(Input(12,[107]));
        var pages=replay.Build(Files,[Open(),Open(20)],100);
        Assert.Equal("Garage",pages.Last(p=>p.Tick<=15).Title);
        Assert.Equal("Garage attendant",pages.Last(p=>p.Tick<=20).Title);
        Assert.Equal("Garage attendant",pages.Last(p=>p.Tick<=11).Title);
        Assert.Equal("Garage",pages.Last(p=>p.Tick<=15).Title);
    }
    [Fact] public void Notice_replaces_current_page_and_its_only_exit_closes_it()
    {
        var replay=new NpcGuiReplay();replay.Add(Input(12,[107]));replay.Add(Input(18,[]));replay.Add(Input(21,[107]));
        var notice=new NetLogRow {T=20,Type="lua",Name="perpheads_npc_notify",Hex=Convert.ToHexString(System.Text.Encoding.UTF8.GetBytes("Purchase complete\0Close\0"))};
        var pages=replay.Build(Files,[Open(),notice],100);
        Assert.Equal("Purchase complete",pages.Last(p=>p.Tick<=20).Text);
        Assert.False(pages.Last(p=>p.Tick<=21).Visible);
    }
    [Fact] public void Server_script_failure_restores_previous_page_and_infinite_script_is_bounded()
    {
        var files=Files.ToArray();files[1]=(files[1].Path,files[1].Text.Replace("GAMEMODE.ShowGarageView() LEAVE_DIALOG()","missing_server_function()"));
        var replay=new NpcGuiReplay(); replay.Add(Input(12,[2]));
        var pages=replay.Build(files,[Open()],100);
        Assert.Equal("Welcome! How can I help?",pages[0].Text);
        Assert.Single(pages,p=>p.Visible);
        files[1]=(files[1].Path,"NPC={} NPC.ID=5\nNPC.Name=\"Unknown\"\nwhile true do end");
        Assert.True(replay.Build(files,[Open()],100)[0].Visible);
    }
    [Fact] public void Unrelated_clicks_do_not_open_gui_and_idle_page_has_no_eight_second_timeout()
    {
        var replay=new NpcGuiReplay();replay.Add(Input(2,[107]));replay.Add(Input(3,[]));replay.Add(Input(1000,[]));
        var pages=replay.Build(Files,[Open()],100);
        Assert.DoesNotContain(pages,p=>p.Tick<10);
        Assert.True(pages.Last(p=>p.Tick<=1000).Visible);
        Assert.False(pages[^1].Visible);
    }
    [Fact] public void Recorded_player_state_controls_role_and_restraint_branches()
    {
        var files=Files.ToArray();
        files[1]=(files[1].Path,files[1].Text.Replace("GAMEMODE.DialogPanel:SetDialog(T('greeting'))",
            "if LocalPlayer():IsRestrained() then GAMEMODE.DialogPanel:SetDialog('Restrained') elseif LocalPlayer():Team()~=TEAM_CITIZEN then GAMEMODE.DialogPanel:SetDialog('On duty') else GAMEMODE.DialogPanel:SetDialog(T('greeting')) end"));
        var replay=new NpcGuiReplay();
        Assert.Equal("On duty",replay.Build(files,[Open()],100,_=>new(3,new Dictionary<string,object?>()))[0].Text);
        Assert.Equal("Restrained",replay.Build(files,[Open()],100,_=>new(1,new Dictionary<string,object?>{{"restrained",true}}))[0].Text);
    }
    [Fact] public void Npc_that_declines_to_open_does_not_create_a_fallback_window()
    {
        var files=Files.ToArray();
        files[1]=(files[1].Path,files[1].Text.Replace("function NPC.OnTalk()","function NPC.OnTalk() if LocalPlayer():IsRestrained() then return end"));
        var pages=new NpcGuiReplay().Build(files,[Open()],100,_=>new(1,new Dictionary<string,object?>{{"restrained",true}}));
        Assert.DoesNotContain(pages,p=>p.Visible);
    }
}
