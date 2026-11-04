using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using GmodDemo.Parser;
using MoonSharp.Interpreter;

namespace GmodDemo.Cli;

internal static partial class PerpData
{
    internal const string AttachmentFolder = "gamemodes/darkrp/gamemode/modules/weaponsystem/";
    internal static readonly string[] AttachmentFiles =
        ["sh_attachments.lua", "attachments/magazines.lua", "attachments/other.lua",
         "attachments/rails.lua", "attachments/sights.lua", "attachments/stocks.lua",
         "attachments/suppressors.lua"];

    public static JsonObject Attachments(IReadOnlyList<(string Path, string Text)> files)
    {
        const string folder = AttachmentFolder;
        var unmapped = files.FirstOrDefault(f => f.Path.StartsWith("_unmapped/", StringComparison.Ordinal)
            && Regex.IsMatch(f.Text, @"\bGunAttachment\s*:\s*New\s*\(|\bfunction\s+GunAttachment\s*:\s*GetByID\s*\("));
        if (unmapped.Text is not null)
            throw new InvalidDataException($"Weapon attachment script {unmapped.Path} has no recovered filename; "
                + "its registration order cannot be determined safely.");
        var core = files.FirstOrDefault(f => f.Path.Equals(folder + "sh_attachments.lua",
            StringComparison.OrdinalIgnoreCase));
        var result = new JsonObject { ["attachments"] = new JsonArray() };
        if (core.Text is null) return result;
        if (Gluapack.IsStub(core.Text))
            throw new InvalidDataException("Weapon attachment definitions are still packed.");

        var script = new Script(CoreModules.Preset_SoftSandbox);
        script.DoString("""
            CLIENT = false
            SERVER = false
            function AddCSLuaFile() end
            function Vector(x,y,z) return {x,y,z} end
            function Angle(x,y,z) return {x,y,z} end
            function IsValid(entity) return entity ~= nil end
            function Either(c,a,b) if c then return a else return b end end
            function string.StartWith(s,prefix) return s:sub(1,#prefix) == prefix end
            function table.HasValue(t,value)
                for _,v in pairs(t) do if v == value then return true end end
                return false
            end
            timer = { Simple = function() end }
            """, codeFriendlyName: "attachment export environment");
        script.DoString(Glua(core.Text), codeFriendlyName: core.Path);
        // IncludeFilesInFolder uses file.Find's alphabetically ordered top-level files.
        foreach (var file in files.Where(f => f.Path.StartsWith(folder + "attachments/",
                     StringComparison.OrdinalIgnoreCase)
                 && IsLua(f.Path) && !f.Path[(folder.Length + "attachments/".Length)..].Contains('/'))
                 .OrderBy(f => f.Path, StringComparer.OrdinalIgnoreCase))
        {
            if (Gluapack.IsStub(file.Text))
                throw new InvalidDataException($"{file.Path} is still packed.");
            try {
                script.DoString(Glua(file.Text), codeFriendlyName: file.Path);
            } catch (InterpreterException error) {
                throw new InvalidDataException($"Cannot export {file.Path}: {error.DecoratedMessage}",error);
            }
        }

        var exported = script.DoString("""
            local out = {}
            local function sequenceFor(a, sight)
                local entity = { sequence = 0,
                    SetSequence = function(self, value) self.sequence = value end,
                    GetSequence = function(self) return self.sequence end }
                if a.OnAttachmentAdded and (a.Slot == GunAttachment.SLOT_IRON_FRONT
                    or a.Slot == GunAttachment.SLOT_IRON_REAR) then
                    a:OnAttachmentAdded(sight, sight and {a, sight} or {a}, entity)
                end
                return entity.sequence
            end
            for id = 1, GunAttachment.CurrentID - 1 do
                local a = assert(GunAttachment:GetByID(id))
                local idleEntity = { GetSequence = function() return 0 end }
                local pos, ang = a:GetIronSightsPosition(nil, idleEntity)
                local alternatePos, alternateAng = a:GetAlternateIronSightsPosition()
                out[#out+1] = { id = id, name = a.Name, model = a.Model, slot = a.Slot,
                    sight = a.Slot == GunAttachment.SLOT_SIGHT,
                    sound = a.GetSound and a:GetSound() or nil,
                    magnification = a:GetScopeMagnification(),
                    ironPos = pos, ironAng = ang,
                    alternatePos = alternatePos, alternateAng = alternateAng,
                    sequence = sequenceFor(a),
                    sightSequence = sequenceFor(a, { Slot = GunAttachment.SLOT_SIGHT }) }
            end
            return out
            """, codeFriendlyName: "attachment registry export");
        result["attachments"] = exported.Table.Length == 0 ? new JsonArray() : Convert(exported);
        return result;
    }
}
