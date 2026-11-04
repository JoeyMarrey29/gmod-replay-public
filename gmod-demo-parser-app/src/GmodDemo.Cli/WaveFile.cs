using System.Buffers.Binary;

namespace GmodDemo.Parser;

/// <summary>16-bit mono PCM WAVE (VLC / Unreal / anything that plays WAV).</summary>
public static class WaveFile
{
    public static void Write(string path, int sampleRate, ReadOnlySpan<short> pcm)
    {
        var dataBytes = pcm.Length * sizeof(short);
        using var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read);
        Span<byte> header = stackalloc byte[44];
        "RIFF"u8.CopyTo(header);
        BinaryPrimitives.WriteInt32LittleEndian(header[4..], 36 + dataBytes);
        "WAVE"u8.CopyTo(header[8..]);
        "fmt "u8.CopyTo(header[12..]);
        BinaryPrimitives.WriteInt32LittleEndian(header[16..], 16);
        BinaryPrimitives.WriteUInt16LittleEndian(header[20..], 1);
        BinaryPrimitives.WriteUInt16LittleEndian(header[22..], 1);
        BinaryPrimitives.WriteInt32LittleEndian(header[24..], sampleRate);
        BinaryPrimitives.WriteInt32LittleEndian(header[28..], sampleRate * 2);
        BinaryPrimitives.WriteUInt16LittleEndian(header[32..], 2);
        BinaryPrimitives.WriteUInt16LittleEndian(header[34..], 16);
        "data"u8.CopyTo(header[36..]);
        BinaryPrimitives.WriteInt32LittleEndian(header[40..], dataBytes);
        stream.Write(header);
        stream.Write(System.Runtime.InteropServices.MemoryMarshal.AsBytes(pcm));
    }

    public static bool IsRiffWave(ReadOnlySpan<byte> file) =>
        file.Length >= 44
        && file.StartsWith("RIFF"u8)
        && file[8..].StartsWith("WAVE"u8);
}
