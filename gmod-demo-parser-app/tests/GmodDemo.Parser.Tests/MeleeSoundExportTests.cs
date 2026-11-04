using GmodDemo.Cli;

namespace GmodDemo.Parser.Tests;

public class MeleeSoundExportTests
{
    [Fact]
    public void Melee_swing_pool_and_delays_follow_the_recorded_base_and_overrides()
    {
        var weapons=PerpData.Sweps([
            ("weapons/perpheads_melee_base.lua","""
                function SWEP:SwingSound()
                    self:EmitSound("npc/vort/claw_swing" .. util.SharedRandomInt("swing",1,2) .. ".wav")
                end
                """),
            ("weapons/machete.lua","""
                SWEP.ViewModel = "models/a_machete.mdl"
                SWEP.Base = "perpheads_melee_base"
                SWEP.Primary.Delay = 0.2
                SWEP.Secondary.Delay = 0.4
                """),
            ("weapons/custom.lua","""
                SWEP.ViewModel = "models/a_custom.mdl"
                SWEP.Base = "perpheads_melee_base"
                function SWEP:SwingSound() self:EmitSound("custom/swing.wav") end
                """),
            ("weapons/fists.lua","""
                SWEP.ViewModel = "models/c_fisticuffs.mdl"
                SWEP.Base = "weapon_idle_base"
                """),
        ]);
        var info=weapons.Info["models/a_machete"]!;
        Assert.True(info["melee"]!.GetValue<bool>());
        Assert.Equal(2,info["meleeSwing"]!["files"]!.AsArray().Count);
        Assert.Equal(0.2,info["meleePrimaryDelay"]!.GetValue<double>());
        Assert.Equal(0.4,info["meleeSecondaryDelay"]!.GetValue<double>());
        Assert.Null(weapons.Info["models/a_custom"]!["meleeSwing"]);
        Assert.Null(weapons.Info["models/c_fisticuffs"]!["meleeSwing"]);
    }
}
