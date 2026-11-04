using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

/// <summary>
/// Locates optional captured demos. They are not in the repository. Tests that need one skip
/// unless <c>GMOD_DEMO_PATH</c> / <c>GMOD_DEMO_SHORT_PATH</c> point at files.
/// </summary>
internal static class RealDemo
{
    private const string PathVariable = "GMOD_DEMO_PATH";
    private const string ShortPathVariable = "GMOD_DEMO_SHORT_PATH";
    private const string PlankPathVariable = "GMOD_DEMO_PLANK_PATH";

    public static string Path =>
        Environment.GetEnvironmentVariable(PathVariable) is { Length: > 0 } fromEnv ? fromEnv : "";

    public static string ShortPath =>
        Environment.GetEnvironmentVariable(ShortPathVariable) is { Length: > 0 } fromEnv ? fromEnv : "";

    public static bool Available => Path.Length > 0 && File.Exists(Path);

    public static bool ShortAvailable => ShortPath.Length > 0 && File.Exists(ShortPath);
    public static string PlankPath => Environment.GetEnvironmentVariable(PlankPathVariable) ?? "";
    public static bool PlankAvailable => PlankPath.Length > 0 && File.Exists(PlankPath);

    public static string SkipReason =>
        $"No captured demo available. Set {PathVariable} to a .dem file.";

    public static string ShortSkipReason =>
        $"No short captured demo available. Set {ShortPathVariable} to a .dem file.";

    public static string ClientName(string demoPath)
    {
        using var reader = DemoReader.Open(demoPath);
        return reader.Header.ClientName;
    }
}

internal sealed class PlankDemoFactAttribute : FactAttribute
{
    public PlankDemoFactAttribute()
    {
        if (!RealDemo.PlankAvailable) Skip = "Set GMOD_DEMO_PLANK_PATH to the October 21 12-57-32 recording.";
    }
}

/// <summary>A Fact that skips when no captured demo is present.</summary>
internal sealed class RealDemoFactAttribute : FactAttribute
{
    public RealDemoFactAttribute()
    {
        if (!RealDemo.Available)
        {
            Skip = RealDemo.SkipReason;
        }
    }
}

/// <summary>A Fact that skips when the shorter captured demo is absent.</summary>
internal sealed class ShortDemoFactAttribute : FactAttribute
{
    public ShortDemoFactAttribute()
    {
        if (!RealDemo.ShortAvailable)
        {
            Skip = RealDemo.ShortSkipReason;
        }
    }
}
