using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using GmodDemo.Parser;
using MoonSharp.Interpreter;

namespace GmodDemo.Cli;

internal static partial class PerpData
{
    public static JsonObject VehiclePlates(IReadOnlyList<(string Path, string Text)> files)
    {
        var script = new Script(CoreModules.Preset_SoftSandbox);
        script.DoString("""
            CLIENT = false
            SERVER = false
            GM = {}
            vehicles = {}
            function Vector(x,y,z) return {x,y,z} end
            function Angle(x,y,z) return {x,y,z} end
            function Color(r,g,b,a) return {r,g,b,a or 255} end
            function Sound(s) return s end
            function T(s) return s end
            function CreateVehicleSound() end
            function AddCSLuaFile() end
            PoliceWhitelist = {}
            MedicJob = {}
            FirefighterJob = {}
            DeliveryDriverJob = {}
            SIRENCOLORS = {}
            SIRENANGLES = {}
            function table.shallow_copy(t)
                local out = {}; for k,v in pairs(t) do out[k] = v end; return out
            end
            function table.Copy(t)
                if type(t) ~= "table" then return t end
                local out = {}; for k,v in pairs(t) do out[k] = table.Copy(v) end; return out
            end
            function table.RemoveByValue(t,value)
                for i = #t,1,-1 do if t[i] == value then table.remove(t,i) end end
            end
            function GM:RegisterVehicle(v)
                assert(type(v.ID) == "string", "Vehicle has no ID")
                if not vehicles[v.ID] then
                    local copy = table.Copy(v)
                    copy.LicensePlates = copy.LicensePlates or
                        (copy.LicensePlate and {copy.LicensePlate}) or {}
                    vehicles[v.ID] = copy
                end
            end
            """, codeFriendlyName: "vehicle plate export environment");
        foreach (var file in files.Where(f => !Gluapack.IsStub(f.Text)
            && Regex.IsMatch(f.Text, @"\bGM:RegisterVehicle\(\s*(?:CIV_)?VEHICLE\s*\)"))
            .OrderBy(f => f.Path, StringComparer.Ordinal))
        {
            try
            {
                script.DoString(Glua(file.Text), codeFriendlyName: file.Path);
            }
            catch (InterpreterException error)
            {
                throw new InvalidDataException($"Cannot export vehicle plates from {file.Path}: {error.DecoratedMessage}", error);
            }
        }
        var exported = script.DoString("""
            local out = {}
            for id,v in pairs(vehicles) do
                if #v.LicensePlates > 0 then
                    local placements, kits = {}, {}
                    for _,p in ipairs(v.LicensePlates) do
                        placements[#placements+1] = {pos=p.Pos, ang=p.Ang}
                    end
                    for _,kit in ipairs(v.CustomBodyGroup or {}) do
                        kits[#kits+1] = {groups=kit.Groups,
                            plate=kit.LicensePlate and {pos=kit.LicensePlate.Pos, ang=kit.LicensePlate.Ang}}
                    end
                    out[#out+1] = {id=id, placements=placements, kits=kits}
                end
            end
            table.sort(out, function(a,b) return a.id < b.id end)
            return out
            """, codeFriendlyName: "vehicle plate placements");
        var skins = new JsonArray();
        foreach (var file in files.Where(f => f.Text.Contains("ITEM.ID = \"license_plate_skin\"", StringComparison.Ordinal)
            && !Gluapack.IsStub(f.Text)))
        {
            script.Globals["ITEM"] = new Table(script);
            try
            {
                script.DoString(Glua(file.Text), codeFriendlyName: file.Path);
                var values = script.DoString("""
                    local out = {}
                    for _,s in ipairs(ITEM.BuyableItems or {}) do
                        out[#out+1] = {id=s.VariationID, material=s.PlateTexture, color=s.TextColor}
                    end
                    return out
                    """);
                if (values.Table.Length > 0)
                    foreach (var item in (JsonArray)Convert(values)!) skins.Add(item?.DeepClone());
            }
            catch (InterpreterException error)
            {
                throw new InvalidDataException($"Cannot export plate skins from {file.Path}: {error.DecoratedMessage}", error);
            }
        }
        return new JsonObject
        {
            ["vehicles"] = exported.Table.Length == 0 ? new JsonArray() : Convert(exported),
            ["skins"] = skins,
        };
    }
}
