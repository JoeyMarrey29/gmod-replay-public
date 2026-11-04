namespace GmodDemo.Parser;

/// <summary>
/// Classifies networked entities and unpacks Source EHANDLEs.
/// Handle layout is GMod's 13-bit edict index plus 10 serial bits (23 bits total), matching
/// the <c>m_hOwner</c> / <c>m_hActiveWeapon</c> sendprops in current demos.
/// </summary>
public static class EntityIdentity
{
    public const int HandleBits = NetMessageLayout.MaxEdictBits + EntityWorld.SerialBits;

    public const int InvalidHandle = (1 << HandleBits) - 1;

    public static int? DecodeHandle(long value)
    {
        if (value <= 0 || value == InvalidHandle)
        {
            return null;
        }

        var index = (int)(value & ((1 << NetMessageLayout.MaxEdictBits) - 1));
        return index > 0 ? index : null;
    }

    /// <summary>The serial that tells two entities apart when the engine reuses an index.</summary>
    public static int DecodeSerial(long value) =>
        value <= 0 || value == InvalidHandle
            ? -1
            : (int)((value >> NetMessageLayout.MaxEdictBits) & ((1 << EntityWorld.SerialBits) - 1));

    public static bool IsWeaponClass(string className) =>
        className.Equals("CWeaponSWEP", StringComparison.Ordinal)
        || className.StartsWith("CWeapon", StringComparison.Ordinal)
        || className.Contains("CombatWeapon", StringComparison.Ordinal);

    public static bool IsDoorClass(string className) =>
        className is "CBaseDoor" or "CBasePropDoor" or "CPropDoorRotating";

    public static bool IsVehicleClass(string className) =>
        className.Contains("Vehicle", StringComparison.Ordinal)
        || className.Contains("Jeep", StringComparison.Ordinal)
        || className.Contains("Airboat", StringComparison.Ordinal);

    public static bool IsScriptedClass(string className) =>
        className.StartsWith("CSENT", StringComparison.Ordinal)
        || className.Equals("CWeaponSWEP", StringComparison.Ordinal)
        || className.Equals("CLuaNextBot", StringComparison.Ordinal);

    /// <summary>
    /// Weapons, scripted entities, and driveable vehicles carry an owner handle worth attributing.
    /// </summary>
    public static bool TracksOwner(string className) =>
        IsWeaponClass(className) || IsVehicleClass(className) || IsScriptedClass(className);

    public static string? DoorStateName(int? state) => state switch
    {
        0 => "closed",
        1 => "opening",
        2 => "open",
        3 => "closing",
        4 => "ajar",
        _ => null,
    };

    public static bool? DoorIsOpen(int? state) => state switch
    {
        0 or 3 => false,
        1 or 2 or 4 => true,
        _ => null,
    };

    /// <summary>
    /// Engine entity classnames for C++ classes that have no <c>m_strScriptName</c>.
    /// These are the names GMod registers for the same classes, not guessed aliases.
    /// </summary>
    public static string? StockEntityName(string serverClass) => serverClass switch
    {
        "CWeaponPhysGun" => "weapon_physgun",
        "CWeaponPhysCannon" => "weapon_physcannon",
        "CWeaponCrowbar" => "weapon_crowbar",
        "CWeaponPistol" => "weapon_pistol",
        "CWeaponSMG1" => "weapon_smg1",
        "CWeaponAR2" => "weapon_ar2",
        "CWeaponShotgun" => "weapon_shotgun",
        "CWeapon357" => "weapon_357",
        "CWeaponCrossbow" => "weapon_crossbow",
        "CWeaponRPG" => "weapon_rpg",
        "CWeaponFrag" => "weapon_frag",
        "CWeaponStunStick" => "weapon_stunstick",
        "CPropDoorRotating" => "prop_door_rotating",
        "CBaseDoor" => "func_door",
        "CPropJeepEpisodic" => "prop_vehicle_jeep",
        "CPropJeep" => "prop_vehicle_jeep",
        "CPropAirboat" => "prop_vehicle_airboat",
        "CPropVehiclePrisonerPod" => "prop_vehicle_prisoner_pod",
        _ => null,
    };
}
