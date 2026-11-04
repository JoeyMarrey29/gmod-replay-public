namespace GmodDemo.Parser;

/// <summary>A position-based sound sent through Perpheads' ph_soundplay Lua net message.</summary>
public sealed record PerpheadsSoundPlay(
    int Tick,
    string Name,
    float X,
    float Y,
    float Z,
    int Level,
    int Pitch,
    float Volume);
