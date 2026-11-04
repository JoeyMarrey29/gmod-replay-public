namespace GmodDemo.Parser;

/// <summary>
/// Server-to-client message IDs for the Source 2013 branch as used by Garry's Mod.
/// </summary>
/// <remarks>
/// Garry's Mod keeps the original Source 2013 numbering: net_Tick at 3 and svc_Print at 7, with
/// no splitscreen messages. Later Valve branches (L4D onward, CS:GO) insert net_SplitScreenUser
/// at 3, which shifts everything up to 7 and moves svc_Print to 16 — that numbering does not
/// apply here. Garry's Mod's own addition takes slot 33, where later Valve branches put
/// svc_SetPauseTimed; that message does not exist in Garry's Mod.
/// </remarks>
public enum NetMessageType
{
    NetNop = 0,
    NetDisconnect = 1,
    NetFile = 2,
    NetTick = 3,
    NetStringCmd = 4,
    NetSetConVar = 5,
    NetSignonState = 6,
    SvcPrint = 7,
    SvcServerInfo = 8,
    SvcSendTable = 9,
    SvcClassInfo = 10,
    SvcSetPause = 11,
    SvcCreateStringTable = 12,
    SvcUpdateStringTable = 13,
    SvcVoiceInit = 14,
    SvcVoiceData = 15,

    /// <summary>Commented out in Valve's protocol.h; should never appear.</summary>
    SvcUnused16 = 16,

    SvcSounds = 17,
    SvcSetView = 18,
    SvcFixAngle = 19,
    SvcCrosshairAngle = 20,
    SvcBspDecal = 21,

    /// <summary>Formerly svc_TerrainMod, marked unused by Valve.</summary>
    SvcUnused22 = 22,

    SvcUserMessage = 23,
    SvcEntityMessage = 24,
    SvcGameEvent = 25,
    SvcPacketEntities = 26,
    SvcTempEntities = 27,
    SvcPrefetch = 28,
    SvcMenu = 29,
    SvcGameEventList = 30,
    SvcGetCvarValue = 31,
    SvcCmdKeyValues = 32,

    /// <summary>Garry's Mod specific. Carries Lua net library messages and file transfers.</summary>
    SvcGModServerToClient = 33,
}

/// <summary>
/// Bit widths that differ between stock Source 2013 and Garry's Mod, or that published sources
/// disagree about. Values here are the ones validated against a real demo; see
/// <c>NetMessageWalker</c> for how they were confirmed.
/// </summary>
public static class NetMessageLayout
{
    /// <summary>Message type tag width. NETMSG_TYPE_BITS, unchanged by Garry's Mod.</summary>
    public const int TypeBits = 6;

    /// <summary>
    /// Garry's Mod raises the edict limit to 8192, so entity index fields are 13 bits wide
    /// rather than stock Source's 11. Using 11 here parses the container fine and then silently
    /// misreads every entity.
    /// </summary>
    public const int MaxEdictBits = 13;

    public const int MaxServerClassBits = 9;

    /// <summary>NETMSG_LENGTH_BITS, used by user and entity messages.</summary>
    public const int NetMessageLengthBits = 11;

    /// <summary>
    /// Payload length width in svc_PacketEntities. Garry's Mod widens this from stock Source's
    /// 20 bits to 24. Verified by calibration against a 2026-build demo: 24 parses 100% of
    /// gameplay packets cleanly, 20 parses 6.6%.
    /// </summary>
    public const int DeltaSizeBits = 24;

    public const int MaxEventBits = 9;

    public const int SpModelIndexBits = 13;

    public const int MaxDecalIndexBits = 9;

    /// <summary>Sound index width for network protocol above 22.</summary>
    public const int SoundIndexBits = 14;

    /// <summary>Payload length width on Garry's Mod's own message.</summary>
    public const int GModMessageLengthBits = 20;

    /// <summary>
    /// Payload length width in svc_CreateStringTable. Zero would mean a varint, which is what
    /// newer Valve branches use; Garry's Mod keeps a fixed field.
    /// </summary>
    public const int CreateStringTableLengthBits = 20;
}
