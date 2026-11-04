using GmodDemo.Cli;

namespace GmodDemo.Parser.Tests;

public sealed class ClothingExportTests
{
    private const string Folder = "gamemodes/darkrp/gamemode/modules/clothing/";

    [Theory]
    [InlineData("legacy", 6)]
    [InlineData("clothing", 3)]
    [InlineData("bodyparts", 6)]
    public void Required_slots_load_both_registry_files_before_using_the_legacy_table(string definition, int expected)
    {
        var lua = """
            Clothing.BODY_SLOT_HANDS = 3
            Clothing.BODY_SLOT_FEET = 6
            """ + (definition == "clothing" ? "\nClothing.RequiredSlots = { [3] = true }\n" : "\n") + """
            IncludeSH("sh_bodyparts.lua")
            """ + (definition == "legacy" ? """
            function AttachMissingBodyParts()
                local slotsLeftToFill = { [Clothing.BODY_SLOT_FEET] = true }
            end
            """ : "");
        var result = PerpData.Clothing(Files(lua, definition == "bodyparts"
            ? "Clothing.RequiredSlots = { [Clothing.BODY_SLOT_FEET] = true }" : ""), out var count);
        Assert.Equal(1, count);
        var slots = result["requiredSlots"]!.AsArray();
        Assert.Single(slots);
        Assert.Equal(expected, slots[0]!.GetValue<long>());
    }

    [RealDemoFact]
    public void Captured_demo_exports_its_exact_clothing_registry()
    {
        var tree = PerpData.LoadLua(RealDemo.Path, null);
        var result = PerpData.Clothing(tree.Files, out var count);
        Assert.True(count > 0, "The recording's clothing registry is empty.");
        Assert.NotEmpty(result["requiredSlots"]!.AsArray());
    }

    [Fact]
    public void Missing_required_slot_definitions_fail_explicitly()
    {
        var error = Assert.Throws<InvalidDataException>(() =>
            PerpData.Clothing(Files("IncludeSH(\"sh_bodyparts.lua\")"), out _));
        Assert.Contains("required slots", error.Message);
    }

    private static (string Path, string Text)[] Files(string lua, string bodyParts = "") =>
    [
        (Folder + "sh_clothing.lua", lua),
        (Folder + "sh_bodyparts.lua", "Clothing.ClothingTypesByID = { [1] = { Model = 'hands', Slots = {3} } }\n" + bodyParts),
        (Folder + "sh_clothingtypes.lua", ""),
    ];
}
