using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;

namespace GmodDemo.Cli;

/// <summary>ANSI styling for an interactive console. No-ops when output is redirected or NO_COLOR is set.</summary>
internal static class Term
{
    private const string Reset = "\x1b[0m";
    private const string Bold = "\x1b[1m";
    private const string Dim = "\x1b[2m";
    private const string Cyan = "\x1b[36m";
    private const string Green = "\x1b[32m";
    private const string Red = "\x1b[31m";
    private const string Yellow = "\x1b[33m";
    private const string White = "\x1b[37m";

    public static bool Color { get; private set; }

    public static bool Unicode { get; private set; }

    public static void Init()
    {
        Console.OutputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
        var noColor = Environment.GetEnvironmentVariable("NO_COLOR") is { Length: > 0 };
        Color = !noColor && !Console.IsOutputRedirected && EnableVirtualTerminal();
        Unicode = Color;
    }

    public static string Accent(string text) => Paint(Bold + Cyan, text);

    public static string Title(string text) => Paint(Bold + White, text);

    public static string Muted(string text) => Paint(Dim, text);

    public static string Ok(string text) => Paint(Green, text);

    public static string Fail(string text) => Paint(Red, text);

    public static string Warn(string text) => Paint(Yellow, text);

    public static void Heading(string text)
    {
        Console.WriteLine();
        Console.WriteLine(Accent(text));
    }

    public static void Kv(string key, string? value, int keyWidth = 18)
    {
        Console.WriteLine($"  {Muted(key.PadRight(keyWidth))}  {value ?? ""}");
    }

    public static void PauseIfLoneConsole()
    {
        if (Console.IsInputRedirected || Console.IsOutputRedirected)
        {
            return;
        }

        if (OperatingSystem.IsWindows() && LoneConsole())
        {
            Console.WriteLine();
            Console.Write(Muted("Press any key to close"));
            Console.ReadKey(true);
        }
    }

    private static string Paint(string code, string text) => Color ? code + text + Reset : text;

    private static bool EnableVirtualTerminal()
    {
        if (!OperatingSystem.IsWindows())
        {
            return true;
        }

        EnableVtOnHandle(-12);
        return EnableVtOnHandle(-11);
    }

    [SupportedOSPlatform("windows")]
    private static bool EnableVtOnHandle(int stdHandle)
    {
        var handle = GetStdHandle(stdHandle);
        if (handle == nint.Zero || handle == (nint)(-1))
        {
            return false;
        }

        if (!GetConsoleMode(handle, out var mode))
        {
            return false;
        }

        return SetConsoleMode(handle, mode | 0x0004);
    }

    [SupportedOSPlatform("windows")]
    private static bool LoneConsole()
    {
        var list = new uint[8];
        var count = GetConsoleProcessList(list, (uint)list.Length);
        return count == 1;
    }

    [SupportedOSPlatform("windows")]
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern nint GetStdHandle(int nStdHandle);

    [SupportedOSPlatform("windows")]
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetConsoleMode(nint hConsoleHandle, out uint lpMode);

    [SupportedOSPlatform("windows")]
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleMode(nint hConsoleHandle, uint dwMode);

    [SupportedOSPlatform("windows")]
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetConsoleProcessList(uint[] processList, uint processCount);
}
