namespace GmodDemo.Parser;

/// <summary>
/// Thrown only for conditions that make the file unusable as a demo at all.
/// Recoverable problems encountered while walking the command stream are reported
/// as <see cref="DemoParseError"/> instead, so a partially readable demo still yields data.
/// </summary>
public sealed class DemoFormatException : Exception
{
    public DemoFormatException(long offset, string message)
        : base($"{message} (at byte offset {offset})")
    {
        Offset = offset;
    }

    public long Offset { get; }
}

public enum DemoErrorKind
{
    /// <summary>Command byte outside the protocol 3 range 1-8. Almost always means we lost sync.</summary>
    InvalidCommandByte,

    /// <summary>The file ended mid-record.</summary>
    TruncatedInput,

    /// <summary>A payload length field was negative.</summary>
    InvalidLength,

    /// <summary>A length field exceeded the allocation sanity ceiling.</summary>
    PayloadTooLarge,
}

/// <summary>A recoverable problem found while walking the command stream.</summary>
public sealed record DemoParseError(DemoErrorKind Kind, long Offset, string Message)
{
    public override string ToString() => $"{Kind} at offset {Offset}: {Message}";
}
