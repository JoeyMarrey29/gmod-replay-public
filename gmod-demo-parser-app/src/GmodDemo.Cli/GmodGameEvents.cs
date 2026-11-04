namespace GmodDemo.Parser;

/// <summary>
/// Built-in Source/GMod game events from
/// <see href="https://wiki.facepunch.com/gmod/gameevent">wiki.facepunch.com/gmod/gameevent</see>.
/// Custom gamemodes add more; those still show up in the JSON as <c>type: game</c> with whatever
/// keys the demo carried. Lua <c>GM:*</c> hooks are not in the demo file.
/// </summary>
public static class GmodGameEvents
{
    public const string PlayerSay = "player_say";
    public const string PlayerHurt = "player_hurt";
    public const string PlayerSpawn = "player_spawn";
    public const string PlayerActivate = "player_activate";
    public const string PlayerConnect = "player_connect";
    public const string PlayerConnectClient = "player_connect_client";
    public const string PlayerDisconnect = "player_disconnect";
    public const string PlayerChangename = "player_changename";
    public const string PlayerInfo = "player_info";
    public const string EntityKilled = "entity_killed";
    public const string BreakProp = "break_prop";
    public const string BreakBreakable = "break_breakable";
    public const string ServerSpawn = "server_spawn";
    public const string ServerCvar = "server_cvar";
    public const string ClientDisconnect = "client_disconnect";
    public const string ClientConnected = "client_connected";
    public const string ClientBeginConnect = "client_beginconnect";
    public const string OnRequestFullUpdate = "OnRequestFullUpdate";
    public const string AchievementEarned = "achievement_earned";
    public const string RagdollDissolved = "ragdoll_dissolved";

    /// <summary>
    /// Wiki <c>player_info.index</c> / <c>player_connect</c> index is EntIndex minus one (the slot).
    /// </summary>
    public static int? EntityFromKeys(string? eventName, IReadOnlyDictionary<string, object?> keys)
    {
        if (Int(keys, "entindex") is { } ent)
        {
            return ent;
        }

        if (Int(keys, "entindex_killed") is { } killed)
        {
            return killed;
        }

        if (Int(keys, "entity") is { } entity)
        {
            return entity;
        }

        if (eventName is PlayerInfo or PlayerConnect or PlayerConnectClient
            && Int(keys, "index") is { } slot)
        {
            return slot + 1;
        }

        return null;
    }

    public static int? UserId(IReadOnlyDictionary<string, object?> keys) => Int(keys, "userid");

    public static bool TeamOnly(IReadOnlyDictionary<string, object?> keys)
    {
        var flag = Int(keys, "teamonly") ?? Int(keys, "team") ?? Int(keys, "Team");
        return flag == 1;
    }

    public static string? SayText(IReadOnlyDictionary<string, object?> keys) =>
        keys.TryGetValue("text", out var value) ? value as string : null;

    public static int? Int(IReadOnlyDictionary<string, object?> keys, string name) =>
        keys.TryGetValue(name, out var value) ? value switch
        {
            int i => i,
            short s => s,
            byte b => b,
            long l => (int)l,
            bool b => b ? 1 : 0,
            _ => null,
        } : null;
}
