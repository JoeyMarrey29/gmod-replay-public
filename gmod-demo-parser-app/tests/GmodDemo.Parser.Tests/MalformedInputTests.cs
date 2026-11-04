using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

/// <summary>
/// Failure-path behaviour, driven by synthetic fixtures. See <see cref="SyntheticDemo"/> for why
/// these byte sequences must not be read as evidence about real demo contents.
/// </summary>
public sealed class MalformedInputTests
{
    [Fact]
    public void Unrecognised_magic_is_rejected()
    {
        using var stream = SyntheticDemo.Stream(SyntheticDemo.Header(magic: "NOTADEMO"u8));

        var ex = Assert.Throws<DemoFormatException>(() => new DemoReader(stream));
        Assert.Contains("Unrecognised demo magic", ex.Message);
        Assert.Equal(0, ex.Offset);
    }

    [Fact]
    public void Stock_source_magic_is_accepted_as_a_source_variant()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(magic: DemoFormat.SourceMagic),
            SyntheticDemo.Stop(0));

        using var reader = new DemoReader(stream);

        Assert.Equal(DemoVariant.Source, reader.Header.Variant);
    }

    [Fact]
    public void Truncated_header_is_rejected()
    {
        using var stream = new MemoryStream(new byte[100]);

        Assert.Throws<DemoFormatException>(() => new DemoReader(stream));
    }

    [Fact]
    public void Demo_protocol_4_is_rejected_rather_than_misparsed()
    {
        // Protocol 4 adds a player-slot byte and a splitscreen camera array. Parsing it with
        // protocol 3 framing would silently produce garbage, so refuse it up front.
        using var stream = SyntheticDemo.Stream(SyntheticDemo.Header(demoProtocol: 4));

        var ex = Assert.Throws<DemoFormatException>(() => new DemoReader(stream));
        Assert.Contains("Unsupported demo protocol 4", ex.Message);
    }

    [Fact]
    public void Invalid_command_byte_stops_enumeration_and_is_reported()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.SyncTick(1),
            [99, 0, 0, 0, 0]);

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        Assert.Single(commands);
        var error = Assert.Single(reader.Errors);
        Assert.Equal(DemoErrorKind.InvalidCommandByte, error.Kind);
    }

    [Fact]
    public void Negative_payload_length_is_reported_without_throwing()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.DataTables(1, declaredLength: -5, actualBytes: 0));

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        Assert.Empty(commands);
        var error = Assert.Single(reader.Errors);
        Assert.Equal(DemoErrorKind.InvalidLength, error.Kind);
    }

    [Fact]
    public void Length_beyond_end_of_file_recovers_without_allocating_the_missing_payload()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.SyncTick(0),
            SyntheticDemo.DataTables(1, declaredLength: 100_000, actualBytes: 4));

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        Assert.Single(commands);
        Assert.Empty(reader.Errors);
        Assert.Equal(DemoErrorKind.TruncatedInput, reader.TailRecovery?.Kind);
        Assert.Contains("incomplete command was discarded", reader.TailRecovery!.Message);
    }

    [Fact]
    public void Absurd_length_is_rejected_by_the_allocation_ceiling()
    {
        // Guards the non-seekable case, where remaining bytes are unknown and the ceiling is the
        // only thing standing between a corrupt length field and a 2 GiB allocation.
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.DataTables(1, declaredLength: int.MaxValue, actualBytes: 4));

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        Assert.Empty(commands);
        var error = Assert.Single(reader.Errors);
        Assert.Equal(DemoErrorKind.PayloadTooLarge, error.Kind);
    }

    [Fact]
    public void Payload_shorter_than_declared_recovers_the_complete_prefix()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.SyncTick(1),
            SyntheticDemo.DataTables(1, declaredLength: 64, actualBytes: 10));

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        Assert.Single(commands);
        Assert.Empty(reader.Errors);
        Assert.Equal(DemoErrorKind.TruncatedInput, reader.TailRecovery?.Kind);
    }

    [Fact]
    public void Missing_stop_record_recovers_complete_commands()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.SyncTick(1));

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        Assert.Single(commands);
        Assert.False(reader.SawStopCommand);
        Assert.Empty(reader.Errors);
        Assert.Equal(DemoErrorKind.TruncatedInput, reader.TailRecovery?.Kind);
    }

    [Fact]
    public void Console_command_length_includes_the_nul_terminator()
    {
        using var stream = SyntheticDemo.Stream(
            SyntheticDemo.Header(),
            SyntheticDemo.ConsoleCmd(7, "echo hello"),
            SyntheticDemo.Stop(8));

        using var reader = new DemoReader(stream);
        var commands = reader.ReadCommands().ToList();

        var console = Assert.IsType<ConsoleCommand>(commands[0]);
        Assert.Equal("echo hello", console.Command);
        Assert.Equal("echo hello".Length + 1, console.PayloadBytes);
    }

    [Fact]
    public void Enumerating_twice_is_rejected()
    {
        using var stream = SyntheticDemo.Stream(SyntheticDemo.Header(), SyntheticDemo.Stop(0));
        using var reader = new DemoReader(stream);

        _ = reader.ReadCommands().ToList();

        Assert.Throws<InvalidOperationException>(() => reader.ReadCommands().ToList());
    }
}
