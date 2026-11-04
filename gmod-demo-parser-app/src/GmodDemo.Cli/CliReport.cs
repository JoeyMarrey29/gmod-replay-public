using System.Diagnostics;
using System.Text;

namespace GmodDemo.Cli;

/// <summary>
/// Progress on the console, optional sibling transcript, and a short footer of written paths.
/// </summary>
internal sealed class CliReport : IDisposable
{
    public const int BarWidth = 28;

    private readonly Stopwatch _stopwatch = Stopwatch.StartNew();
    private readonly TextWriter _console;
    private readonly TextWriter? _file;
    private readonly TextWriter? _previousOut;
    private readonly long _totalBytes;
    private readonly List<string> _outputs = [];
    private int _lastPercent = -1;
    private bool _disposed;
    private bool _barEnded;

    private CliReport(
        string command,
        string demoPath,
        string? outputPath,
        TextWriter console,
        TextWriter? file,
        long totalBytes)
    {
        OutputPath = outputPath;
        _console = console;
        _file = file;
        _totalBytes = totalBytes;
        Current = this;
        if (file is not null)
        {
            _previousOut = Console.Out;
            Console.SetOut(new TeeWriter(console, file));
        }

        if (outputPath is not null)
        {
            _outputs.Add(outputPath);
        }

        if (demoPath.Length > 0)
        {
            _console.WriteLine();
            _console.WriteLine($"  {Term.Title("gmoddem")}  {Term.Accent(command)}");
            _console.WriteLine($"  {Term.Muted(Path.GetFullPath(demoPath))}");
            _console.WriteLine();
        }

        if (totalBytes > 0)
        {
            Draw(0);
            _lastPercent = 0;
        }
    }

    public static CliReport? Current { get; private set; }

    public string? OutputPath { get; }

    public static void ReportProgress(long bytesRead) => Current?.Report(bytesRead);

    public static CliReport Attach(string command, string[] rest)
    {
        var console = Console.Out;
        if (command is "" or "help" || rest.Length == 0)
        {
            return new CliReport(command, "", null, console, null, 0);
        }

        var demoPath = rest[0];
        if (!File.Exists(demoPath))
        {
            return new CliReport(command, demoPath, null, console, null, 0);
        }

        var outputPath = ResolveOutputPath(command, rest);
        var length = new FileInfo(demoPath).Length;
        var transcript = command is not "json" and not "window";
        StreamWriter? file = null;
        if (transcript)
        {
            file = new StreamWriter(outputPath, false, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        }

        return new CliReport(command, demoPath, outputPath, console, file, length);
    }

    public void AddOutput(string path)
    {
        var full = Path.GetFullPath(path);
        if (!_outputs.Contains(full, StringComparer.OrdinalIgnoreCase))
        {
            _outputs.Add(full);
        }
    }

    public void Report(long bytesRead)
    {
        if (_totalBytes <= 0)
        {
            return;
        }

        var percent = (int)Math.Clamp(bytesRead * 100 / _totalBytes, 0, 100);
        if (percent == _lastPercent)
        {
            return;
        }

        _lastPercent = percent;
        Draw(percent);
    }

    public void EndBar()
    {
        if (_barEnded || _totalBytes <= 0)
        {
            return;
        }

        if (_lastPercent < 100)
        {
            Draw(100);
        }

        _console.WriteLine();
        _barEnded = true;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _stopwatch.Stop();

        if (_totalBytes > 0 && !_barEnded)
        {
            if (_lastPercent < 100)
            {
                Draw(100);
            }

            _console.WriteLine();
        }

        if (_file is not null)
        {
            _file.Flush();
            if (_previousOut is not null)
            {
                Console.SetOut(_previousOut);
            }

            _file.Dispose();
        }

        if (_outputs.Count > 0)
        {
            for (var i = 0; i < _outputs.Count; i++)
            {
                var label = i == 0 ? Term.Ok("wrote") : Term.Muted("    +");
                _console.WriteLine($"  {label}  {_outputs[i]}");
            }

            _console.WriteLine($"  {Term.Muted("done")}   {_stopwatch.Elapsed.TotalSeconds:0.000}s");
        }

        Current = null;
    }

    private void Draw(int percent)
    {
        var filled = percent * BarWidth / 100;
        string bar;
        if (Term.Unicode)
        {
            bar = new string('█', filled) + new string('░', BarWidth - filled);
        }
        else
        {
            bar = new string('#', filled) + new string('-', BarWidth - filled);
        }

        var body = Term.Color ? Term.Accent(bar) : bar;
        _console.Write($"\r  {body}  {percent,3}%");
        _console.Flush();
    }

    private static string ResolveOutputPath(string command, string[] rest)
    {
        if (command == "json")
        {
            for (var i = 1; i < rest.Length; i++)
            {
                if (rest[i] is "-o" or "--out" && i + 1 < rest.Length)
                {
                    return Path.GetFullPath(rest[i + 1]);
                }
            }

            return Path.ChangeExtension(Path.GetFullPath(rest[0]), ".json");
        }

        if (command == "window")
        {
            return Path.ChangeExtension(Path.GetFullPath(rest[0]), ".window.json");
        }

        if (command == "perp-data")
        {
            for (var i = 1; i < rest.Length; i++)
            {
                if (rest[i] is "-o" or "--out" && i + 1 < rest.Length)
                {
                    var dir = Path.GetFullPath(rest[i + 1]);
                    Directory.CreateDirectory(dir);
                    return Path.Combine(dir, "perp-data.log");
                }
            }
        }

        return Path.ChangeExtension(Path.GetFullPath(rest[0]), $".{command}.txt");
    }

    private sealed class TeeWriter(TextWriter a, TextWriter b) : TextWriter
    {
        public override Encoding Encoding => a.Encoding;

        public override void Write(char value)
        {
            a.Write(value);
            if (value != '\x1b')
            {
                b.Write(value);
            }
        }

        public override void Write(char[]? buffer, int index, int count)
        {
            if (buffer is null)
            {
                return;
            }

            a.Write(buffer, index, count);
            b.Write(StripAnsi(new string(buffer, index, count)));
        }

        public override void Write(string? value)
        {
            a.Write(value);
            b.Write(StripAnsi(value));
        }

        public override void WriteLine(string? value)
        {
            a.WriteLine(value);
            b.WriteLine(StripAnsi(value));
        }

        public override void Flush()
        {
            a.Flush();
            b.Flush();
        }

        private static string? StripAnsi(string? value)
        {
            if (value is null || value.IndexOf('\x1b') < 0)
            {
                return value;
            }

            var output = new StringBuilder(value.Length);
            for (var i = 0; i < value.Length; i++)
            {
                if (value[i] == '\x1b' && i + 1 < value.Length && value[i + 1] == '[')
                {
                    i += 2;
                    while (i < value.Length && value[i] != 'm')
                    {
                        i++;
                    }

                    continue;
                }

                output.Append(value[i]);
            }

            return output.ToString();
        }
    }
}
