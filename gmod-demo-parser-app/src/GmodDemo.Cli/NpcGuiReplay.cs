using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;
using MoonSharp.Interpreter;

namespace GmodDemo.Parser;

internal sealed class NpcGuiReplay
{
    internal sealed record Page(int Tick, bool Visible, string Title, string Text, string[] Options, bool Inferred);
    internal sealed record PlayerContext(int Team,IReadOnlyDictionary<string,object?> Variables);
    private readonly List<RecordedGuiInput> inputs=[];
    internal void Add(UserCommand command) { if (RecordedGuiInput.Read(command) is {} input) inputs.Add(input); }

    internal List<Page> Build(IReadOnlyList<(string Path,string Text)> files, IReadOnlyList<NetLogRow> messages, double tickRate,
        Func<int,PlayerContext>? contextAt=null)
    {
        var definitions=new Dictionary<int,(string Name,string Source)>();
        foreach (var (_,source) in files) {
            var id=Regex.Match(source,@"(?m)^\s*NPC\.ID\s*=\s*(\d+)");
            var name=Regex.Match(source,"(?m)^\\s*NPC\\.Name\\s*=\\s*\"([^\"]+)\"");
            if (id.Success && name.Success && int.TryParse(id.Groups[1].Value,out int number))
                definitions[number]=(name.Groups[1].Value,source);
        }
        var translations=files.Where(f=>f.Path.Contains("/translation/en/",StringComparison.OrdinalIgnoreCase)).ToArray();
        var events=new List<(int Tick,int Order,object Value)>();
        events.AddRange(messages.Where(m=>m.Type=="lua" && m.Name is "perpheads_npc_used" or "perpheads_npc_notify" or "perpheads_bank_account_sync_balance")
            .Select(m=>(m.T,0,(object)m)));
        events.AddRange(inputs.Select(i=>(i.Tick,1,(object)i)));
        var result=new List<Page>();
        Session? session=null;
        var previous=new HashSet<int>();
        int opened=0,deadline=0;
        var balances=new Dictionary<int,double>();
        void Emit(int tick) {
            var page=session?.Snapshot(tick) ?? new Page(tick,false,"","",[],false);
            if (result.Count>0) {
                var last=result[^1];
                if (last.Visible==page.Visible && last.Title==page.Title && last.Text==page.Text &&
                    last.Inferred==page.Inferred && last.Options.SequenceEqual(page.Options)) return;
            }
            result.Add(page);
        }
        foreach (var (tick,_,value) in events.OrderBy(e=>e.Tick).ThenBy(e=>e.Order)) {
            if (session is not null && tick>deadline) { session.Close(); Emit(deadline); session=null; }
            if (value is NetLogRow message) {
                var body=Body(message);
                if (message.Name=="perpheads_npc_used" && body.Length>=4) {
                    int id=BitConverter.ToUInt16(body);
                    var def=definitions.GetValueOrDefault(id,($"NPC {id}",""));
                    session=new Session(def.Item1,def.Item2,translations,contextAt?.Invoke(tick));
                    opened=tick;
                    deadline=tick+(int)(Math.Max(1,tickRate)*60);
                    Emit(tick);
                }
                else if (message.Name=="perpheads_npc_notify") {
                    if (session is null) {
                        session=new Session("Notice","",translations);
                        opened=tick; deadline=tick+(int)(Math.Max(1,tickRate)*60);
                    }
                    var parts=System.Text.Encoding.UTF8.GetString(body).Split('\0');
                    session.Notice(parts.ElementAtOrDefault(0)??"",parts.ElementAtOrDefault(1)??"");
                    Emit(tick);
                }
                else if (message.Name=="perpheads_bank_account_sync_balance" && body.Length>=12) {
                    int account=BitConverter.ToInt32(body);
                    double balance=BitConverter.ToDouble(body,4);
                    if (double.IsFinite(balance)) {
                        if (session?.Kind=="atm" && balances.TryGetValue(account,out var old) && old!=balance) {
                            session.Balance(balance); Emit(tick);
                        }
                        balances[account]=balance;
                    }
                }
            }
            else if (value is RecordedGuiInput input) {
                var pressed=input.Keys.Where(k=>!previous.Contains(k)).ToArray();
                previous=input.Keys.ToHashSet();
                if (session is null || tick<=opened) continue;
                // Walking away is stronger evidence of dismissal than an arbitrary
                // eight-second display timeout. Ignore the command that opened it.
                if (pressed.Contains(70) || (tick>opened+2 && input.Moving)) {
                    session.Close(); Emit(tick); session=null; continue;
                }
                foreach (int key in pressed) {
                    if (key is >=1 and <=10) session.Select(key==1?10:key-1);
                    else if (key==107) session.MouseClick();
                }
                Emit(tick);
            }
        }
        if (session is not null) { session.Close(); Emit(deadline); }
        return result;
    }

    private static byte[] Body(NetLogRow message) {
        try { return message.Hex is {} hex ? Convert.FromHexString(hex) : []; }
        catch (FormatException) { return []; }
    }

    internal void Write(Utf8JsonWriter writer, IReadOnlyList<(string Path,string Text)> files,
        IReadOnlyList<NetLogRow> messages,double tickRate,Func<int,PlayerContext>? contextAt=null)
    {
        var pages=Build(files,messages,tickRate,contextAt);
        writer.WriteStartArray("npcGui");
        foreach (var page in pages) {
            writer.WriteStartObject();
            writer.WriteNumber("time",page.Tick/Math.Max(1,tickRate));
            writer.WriteBoolean("visible",page.Visible);
            writer.WriteString("title",page.Title);
            writer.WriteString("text",page.Text);
            writer.WriteBoolean("inferred",page.Inferred);
            writer.WriteStartArray("options");
            foreach (var option in page.Options) writer.WriteStringValue(option);
            writer.WriteEndArray();writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }

    // Only executes in a Lua interpreter with no OS, filesystem, CLR, network or
    // GUI access. Coroutines have a fixed instruction budget. No live input is sent.
    private sealed class Session
    {
        private readonly Script script=new(CoreModules.Preset_HardSandbox);
        private readonly string title;
        private bool inferred;
        private bool fallback;
        internal string Kind=>script.Globals.Get("custom_kind").CastToString()??"";
        internal Session(string name,string source,(string Path,string Text)[] translations,PlayerContext? context=null) {
            title=name;
            inferred=true; // Text/layout are reconstructed; random variants are not recorded.
            Run(script.LoadString(Prelude));
            script.Globals["recorded_team"]=context?.Team ?? 1;
            var variables=new Table(script);
            if (context is not null) foreach (var (key,value) in context.Variables)
                if (value is string or bool or int or float or double)
                    variables[key.ToLowerInvariant()]=value;
            script.Globals["recorded_variables"]=variables;
            foreach (var (path,text) in translations) {
                try { Run(script.LoadString(Glua(text),codeFriendlyName:path)); }
                catch (InterpreterException) { /* Other translation groups remain usable. */ }
            }
            try {
                if (source.Length==0) throw new InvalidOperationException();
                Run(script.LoadString(Glua(source),codeFriendlyName:"recorded NPC definition"));
                var npc=script.Globals.Get("loaded_npc");
                if (npc.Type!=DataType.Table || npc.Table.Get("OnTalk").Type!=DataType.Function) throw new InvalidOperationException();
                Run(npc.Table.Get("OnTalk"));
            } catch (Exception e) when (e is InterpreterException or InvalidOperationException) {
                Console.Error.WriteLine($"NPC_GUI_FALLBACK: {name}: {e.Message}");
                fallback=true;
                // Preserve the correct interface type even if a server-specific
                // dependency prevents evaluating its complete menu definition.
                script.Globals["custom_kind"]=name.Contains("ATM",StringComparison.OrdinalIgnoreCase)?"atm":
                    name.Contains("Storage",StringComparison.OrdinalIgnoreCase)?"storage":"dialog";
                script.Globals["fallback_visible"]=true;
            }
        }
        private void Run(DynValue function, params DynValue[] args) {
            var coroutine=script.CreateCoroutine(function).Coroutine;
            coroutine.AutoYieldCounter=50000;
            coroutine.Resume(args);
            if (coroutine.State!=CoroutineState.Dead) throw new InvalidOperationException("NPC script instruction limit");
        }
        private void Call(string name, params DynValue[] args) {
            Run(script.Globals.Get("save_display"));
            try { Run(script.Globals.Get(name),args); }
            catch (Exception e) when (e is InterpreterException or InvalidOperationException) {
                Run(script.Globals.Get("restore_display"));
            }
        }
        internal void Close() { script.Globals["fallback_visible"]=false; Call("close_all"); }
        internal void Notice(string text,string exit) { fallback=false; Call("show_notice",DynValue.NewString(text),DynValue.NewString(exit)); }
        internal void Balance(double balance) { inferred=true; Call("show_balance",DynValue.NewString(balance.ToString("N0",CultureInfo.InvariantCulture))); }
        internal void Select(int index) { if (!fallback) Call("select_option",DynValue.NewNumber(index)); }
        internal void MouseClick() {
            if (fallback) return;
            // A screen cursor is absent. Infer a transition only when this page
            // has exactly one non-exit action. Never guess among several replies.
            inferred=true;
            Call("single_action");
        }
        internal Page Snapshot(int tick) {
            if (fallback) return new(tick,script.Globals.Get("fallback_visible").Boolean,title,
                Kind=="storage"?"Personal storage":Kind=="atm"?"ATM":"Talking to this NPC.",[],true);
            var page=script.Globals.Get("custom_page");
            bool custom=page.Type==DataType.Table;
            if (!custom) page=script.Globals.Get("dialog");
            bool visible=custom || page.Table.Get("visible").Boolean;
            var options=page.Table.Get("options");
            var labels=options.Type==DataType.Table ? options.Table.Values
                .Where(v=>v.Type==DataType.Table).Select(v=>v.Table.Get("text").CastToString()??"").Where(s=>s.Length>0).Take(20).ToArray() : [];
            return new(tick,visible,custom?page.Table.Get("title").CastToString()??title:title,
                page.Table.Get("text").CastToString()??"",labels,inferred);
        }
    }

    private static string Glua(string text) {
        // Rewrite operators outside strings and comments, preserving translated text.
        return Regex.Replace(text,"\"(?:\\\\.|[^\"\\\\])*\"|'(?:\\\\.|[^'\\\\])*'|//[^\\r\\n]*|--[^\\r\\n]*|!=|&&|\\|\\||!",m=>
            m.Value.StartsWith("//")?"--"+m.Value[2..]:m.Value switch {"!="=>"~=","&&"=>" and ","||"=>" or ","!"=>" not ",_=>m.Value});
    }

    private const string Prelude="""
        translations = {}
        TranslationSystem = {}
        function TranslationSystem:RegisterTranslations(lang, entries)
            if lang == 'en' then for _,entry in ipairs(entries) do translations[entry[1]]=entry[2] end end
        end
        function TranslationSystem:RegisterTranslation(lang,key,value) if lang=='en' then translations[key]=value end end
        function TranslationSystem:TranslateTable(key)
            local value=translations[key]
            return type(value)=='table' and value or {value or ''}
        end
        function T(key, ...)
            local value=translations[key]
            if type(value)=='table' then value=value[1] end
            value=value or ''
            local args={...}
            return (value:gsub('{(%d+)}',function(n) return tostring(args[tonumber(n)] or '…') end))
        end
        TranslationSystem.Translate=function(self,...) return T(...) end
        RegisterTranslations=function(...) TranslationSystem:RegisterTranslations(...) end
        RegisterTranslation=function(...) TranslationSystem:RegisterTranslation(...) end
        table.Random=function(t) return type(t)=='table' and t[1] or '' end
        table.Count=function(t) local n=0 for _ in pairs(t or {}) do n=n+1 end return n end
        TEAM_CITIZEN=1 TEAM_POLICE=3 TEAM_MEDIC=4 TEAM_FIREFIGHTER=5 TEAM_DISPATCHER=15 CLIENT=true SERVER=false
        NPC_DIALOG_COLOR_NORMAL=0 NPC_DIALOG_COLOR_NEGATIVE=0 NPC_DIALOG_COLOR_WARNING=0 NPC_DIALOG_COLOR_HIGHLIGHTED=0
        NPC_DIALOG_COLOR_POSITIVE=0 NPC_DIALOG_COLOR_INFO=0
        function Color(...) return {} end
        function Vector(...) return {Distance=function() return 0 end} end
        function Angle(...) return {} end
        function Model(s) return s end
        function Sound(s) return s end
        function GetNPCLocation() return {} end
        function GetNPCAngle() return {} end
        function GetNPCModel() return {} end
        function GetDealerLocations() return {} end
        function IsParalakeV6() return true end
        function IsValid() return false end
        function Either(c,a,b) if c then return a else return b end end
        function formatMoney(n) return n and ('$'..tostring(n)) or '…' end
        function AddCSLuaFile() end
        function RunConsoleCommand() end
        function LEAVE_JOB() close_all() end
        VEHICLE_DATABASE={} NPC_DATABASE={}
        ents={FindInSphere=function() return {} end}
        BountySystem={HasBounty=function() return false end}
        local player={}
        recorded_variables={}
        function player:Team() return recorded_team or TEAM_CITIZEN end
        function player:IsRestrained() local v=recorded_variables.restrained return v==true or v==1 end
        function player:GetNWString(k,default) return recorded_variables[string.lower(k)] or default or '' end
        function player:GetNWInt(k,default) return recorded_variables[string.lower(k)] or default or 0 end
        function player:GetPos() return Vector() end
        function player:GetOrganization() return nil end
        function player:Notify() end
        function player:OwnsVehicle() return false end
        function LocalPlayer() return player end
        net={Start=function() end,Receive=function() end,SendToServer=function() end,WriteString=function() end,
            WriteUInt=function() end,WriteBool=function() end,WriteInt=function() end}
        dialog={visible=false,text='',options={}}
        function dialog:ClearDialog() self.text='' self.options={} end
        function dialog:SetDialog(text) self.text=text or '' end
        function dialog:AddDialog(text,action,args)
            if text and text~='' then table.insert(self.options,{text=text,action=action,args=args,exit=action==LEAVE_DIALOG}) end
        end
        function dialog:SetNPC(id) end
        function dialog:Show() self.visible=true end
        function dialog:Hide() self.visible=false self:ClearDialog() end
        function dialog:DisplayAfterFetchingData() self:SetDialog('Loading…') self:Show() end
        GAMEMODE={DialogPanel=dialog}
        function GAMEMODE:LoadNPC(npc) loaded_npc=npc NPC_DATABASE[npc.ID]=npc end
        function LEAVE_DIALOG() dialog:Hide() end
        function close_all() custom_page=nil custom_kind='' dialog:Hide() end
        function open_custom(kind,title,text,labels)
            custom_kind=kind
            custom_page={title=title,text=text or '',options={}}
            for _,label in ipairs(labels or {}) do table.insert(custom_page.options,{text=label}) end
            dialog:Hide()
        end
        function GAMEMODE.ShowGarageView() open_custom('garage','Garage','Select a vehicle',{'Vehicles','Accessories','Close'}) end
        function ShowStorageUI() open_custom('storage','Storage','Personal storage',{'Inventory','Storage','Search','Close'}) end
        function GAMEMODE.OpenShop() open_custom('shop','Shop','Browse items',{'Items','Close'}) end
        vgui={Create=function(class)
            if class=='perpheads_atm_menu' then
                open_custom('atm','ATM','Personal account',{'Fast cash - $2,500','Balance','Withdraw','Deposit','Transfer','Exit'})
            else open_custom(class,'Menu','',{'Close'}) end
            return {}
        end}
        function show_notice(text,exit)
            custom_page=nil custom_kind='' dialog:ClearDialog() dialog:SetDialog(text)
            dialog:AddDialog(exit~='' and exit or 'Close',LEAVE_DIALOG) dialog:Show()
        end
        function show_balance(balance) open_custom('atm','ATM','Balance: $'..balance,{'Back','Exit'}) end
        function select_option(index)
            if custom_page then return end
            local choice=dialog.options[index]
            if not choice or type(choice.action)~='function' then return end
            dialog:ClearDialog()
            choice.action(choice.args)
        end
        function single_action()
            if custom_page then return end
            if #dialog.options==1 and dialog.options[1].exit then select_option(1) return end
            local index=nil
            for i,choice in ipairs(dialog.options) do
                if not choice.exit then if index then return end index=i end
            end
            if index then select_option(index) end
        end
        function save_display()
            saved_display={custom_page=custom_page,custom_kind=custom_kind,
                visible=dialog.visible,text=dialog.text,options=dialog.options}
        end
        function restore_display()
            custom_page=saved_display.custom_page custom_kind=saved_display.custom_kind
            dialog.visible=saved_display.visible dialog.text=saved_display.text dialog.options=saved_display.options
        end
        """;
}
