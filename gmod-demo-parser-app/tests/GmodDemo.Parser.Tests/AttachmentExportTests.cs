using GmodDemo.Cli;

namespace GmodDemo.Parser.Tests;

public class AttachmentExportTests
{
    [RealDemoFact]
    public void Recorded_attachment_references_have_exact_registry_definitions()
    {
        var tree = PerpData.LoadLua(RealDemo.Path, null);
        var result = PerpData.Attachments(tree.Files);
        Assert.NotEmpty(result["attachments"]!.AsArray());
    }

    [Fact]
    public void Registration_order_not_guessed_ids_defines_optics()
    {
        const string folder = "gamemodes/darkrp/gamemode/modules/weaponsystem/";
        var core = """
            GunAttachment = { CurrentID = 1 }
            local entries = {}
            function GunAttachment:New(name,model,slot)
                local a = setmetatable({Name=name,Model=model,Slot=slot}, {__index=self})
                entries[self.CurrentID] = a
                self.CurrentID = self.CurrentID + 1
                return a
            end
            function GunAttachment:GetByID(id) return entries[id] end
            function GunAttachment:GetScopeMagnification() end
            function GunAttachment:GetIronSightsPosition() end
            function GunAttachment:GetAlternateIronSightsPosition() end
            """;
        var data = PerpData.Attachments([
            (folder + "sh_attachments.lua",core),
            (folder + "attachments/sights.lua","""
                local a = GunAttachment:New("acog","models/acog.mdl",4)
                function a:GetScopeMagnification() return 3 end
                function a:GetIronSightsPosition() return Vector(0,2.45,1.64721),Vector(0,0,0) end
                """),
            (folder + "attachments/magazines.lua",
                "GunAttachment:New('magazine','models/mag.mdl',5)"),
            (folder + "attachments/nested/ignored.lua","error('not included by the game')"),
        ]);
        var items = data["attachments"]!.AsArray();
        Assert.Equal(2,items.Count);
        Assert.Equal("magazine",items[0]!["name"]!.GetValue<string>());
        Assert.Equal(2,items[1]!["id"]!.GetValue<long>());
        Assert.Equal(3,items[1]!["magnification"]!.GetValue<long>());
        Assert.Equal(2.45,items[1]!["ironPos"]![1]!.GetValue<double>(),5);
        Assert.Null(items[0]!["magnification"]);
    }

    [Fact]
    public void Suppressor_sound_comes_from_its_method_not_the_attachment_slot_or_name()
    {
        const string folder = "gamemodes/darkrp/gamemode/modules/weaponsystem/";
        var data = PerpData.Attachments([
            (folder + "sh_attachments.lua", """
                GunAttachment = { CurrentID = 1, SLOT_SILENCER = 2 }
                local entries = {}
                function GunAttachment:New(name,model,slot)
                    local a = setmetatable({Name=name,Model=model,Slot=slot}, {__index=self})
                    entries[self.CurrentID] = a
                    self.CurrentID = self.CurrentID + 1
                    return a
                end
                function GunAttachment:GetByID(id) return entries[id] end
                function GunAttachment:GetSound() end
                function GunAttachment:GetScopeMagnification() end
                function GunAttachment:GetIronSightsPosition() end
                function GunAttachment:GetAlternateIronSightsPosition() end
                """),
            (folder + "attachments/suppressors.lua", """
                local flash = GunAttachment:New("rifle_suppressor_flash","models/flash.mdl",2)
                local suppressed = GunAttachment:New("custom_name","models/silencer.mdl",2)
                function suppressed:GetSound() return "historical_suppressed_919" end
                """),
        ]);
        var items = data["attachments"]!.AsArray();
        Assert.Null(items[0]!["sound"]);
        Assert.Equal("historical_suppressed_919",items[1]!["sound"]!.GetValue<string>());
    }

    [Fact]
    public void Iron_sight_sequences_follow_historical_callbacks_not_names_or_assumed_indices()
    {
        const string folder = "gamemodes/darkrp/gamemode/modules/weaponsystem/";
        var data = PerpData.Attachments([
            (folder + "sh_attachments.lua", """
                GunAttachment = { CurrentID = 1, SLOT_SIGHT = 4, SLOT_IRON_FRONT = 6, SLOT_IRON_REAR = 7 }
                local entries = {}
                function GunAttachment:New(name,model,slot)
                    local a = setmetatable({Name=name,Model=model,Slot=slot}, {__index=self})
                    entries[self.CurrentID] = a
                    self.CurrentID = self.CurrentID + 1
                    return a
                end
                function GunAttachment:GetByID(id) return entries[id] end
                function GunAttachment:GetScopeMagnification() end
                function GunAttachment:GetIronSightsPosition() end
                function GunAttachment:GetAlternateIronSightsPosition() end
                """),
            (folder + "attachments/sights.lua", """
                local a = GunAttachment:New("custom_front","models/front.mdl",6)
                function a:OnAttachmentAdded(added, attachments, entity)
                    if SERVER or not IsValid(entity) then return end
                    entity:SetSequence(2)
                    for _, fitted in pairs(attachments) do
                        if fitted.Slot == GunAttachment.SLOT_SIGHT then entity:SetSequence(3) end
                    end
                end
                GunAttachment:New("fixed_rear","models/rear.mdl",7)
                GunAttachment:New("optic","models/optic.mdl",4)
                """),
        ]);
        var items = data["attachments"]!.AsArray();
        Assert.Equal(2, items[0]!["sequence"]!.GetValue<long>());
        Assert.Equal(3, items[0]!["sightSequence"]!.GetValue<long>());
        Assert.Equal(0, items[1]!["sightSequence"]!.GetValue<long>());
        Assert.Equal(0, items[2]!["sightSequence"]!.GetValue<long>());
    }

    [Fact]
    public void Missing_registry_is_empty()
    {
        Assert.Empty(PerpData.Attachments([])["attachments"]!.AsArray());
    }

    [Fact]
    public void Unmapped_registration_fails_instead_of_silently_dropping_attachments()
    {
        var error = Assert.Throws<InvalidDataException>(() => PerpData.Attachments([
            ("_unmapped/hash.lua", "local optic = GunAttachment:New('optic', 'models/optic.mdl', 4)"),
        ]));
        Assert.Contains("registration order", error.Message);
        Assert.Contains("_unmapped/hash.lua", error.Message);
    }

    [Fact]
    public void Broken_registration_reports_which_historical_file_failed()
    {
        const string folder = "gamemodes/darkrp/gamemode/modules/weaponsystem/";
        var error = Assert.Throws<InvalidDataException>(() => PerpData.Attachments([
            (folder + "sh_attachments.lua","GunAttachment = {}"),
            (folder + "attachments/broken.lua","error('bad attachment')"),
        ]));
        Assert.Contains("attachments/broken.lua",error.Message);
        Assert.Contains("bad attachment",error.Message);
    }
}
