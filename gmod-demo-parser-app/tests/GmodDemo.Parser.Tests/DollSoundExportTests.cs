using GmodDemo.Cli;

namespace GmodDemo.Parser.Tests;

public class DollSoundExportTests
{
    [Fact]
    public void Doll_sound_pools_come_from_the_recorded_lua_aliases()
    {
        var weapons = PerpData.Sweps([
            ("gamemodes/darkrp/gamemode/modules/core/sh_sounds.lua", """
                AddCSLuaFile()
                sound.Add({name="ElfShake", sound={"elf/shake1.mp3","elf/shake2.mp3"}, level=75, volume=1})
                sound.Add({name="ElfFidget", sound="elf/fidget.mp3", level=70, volume=0.8})
                """),
            ("weapons/item_holiday2015doll.lua", """
                SWEP.ViewModel = "models/c_elf.mdl"
                SWEP.ElfShake = "ElfShake"
                SWEP.ElfFidget = "ElfFidget"
                """),
        ]);
        var info = weapons.Info["models/c_elf"]!;
        Assert.Equal(2, info["dollShake"]!["files"]!.AsArray().Count);
        Assert.Equal("elf/shake2.mp3", info["dollShake"]!["files"]![1]!.GetValue<string>());
        Assert.Equal("elf/fidget.mp3", info["dollFidget"]!["files"]![0]!.GetValue<string>());
        Assert.Equal(70, info["dollFidget"]!["level"]!.GetValue<long>());
        Assert.Equal(0.8, info["dollFidget"]!["volume"]!.GetValue<double>());
    }
}
