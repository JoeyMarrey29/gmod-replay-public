using System.Runtime.InteropServices;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class SoundMessageTests
{
    [Fact]
    public void Ph_soundplay_decodes_the_server_string_table_vector_and_parameters()
    {
        var decoder = new PerpheadsMessageDecoder();
        var tableAdd = new BitWriter();
        tableAdd.UInt(1, 16);
        tableAdd.String("doors/door_latch3.wav");
        decoder.Process(4, "perpheads_stringtable_add", tableAdd.ToArray());

        var body = SoundPlayBody(stringTableIndex: 1, "doors/door_latch3.wav", out var bodyBits);
        decoder.Process(123, "ph_soundplay", body, bodyBits);

        var sound = Assert.Single(decoder.SoundPlays);
        Assert.Equal(123, sound.Tick);
        Assert.Equal("doors/door_latch3.wav", sound.Name);
        Assert.Equal(12.5f, sound.X);
        Assert.Equal(-3.25f, sound.Y);
        Assert.Equal(0f, sound.Z);
        Assert.Equal(70, sound.Level);
        Assert.Equal(95, sound.Pitch);
        Assert.Equal(0.75f, sound.Volume);
    }

    [Fact]
    public void Ph_soundplay_accepts_the_raw_stringtable_fallback()
    {
        var decoder = new PerpheadsMessageDecoder();
        var body = SoundPlayBody(stringTableIndex: null, "doors/door_latch3.wav", out var bodyBits);

        decoder.Process(7, "ph_soundplay", body, bodyBits);

        Assert.Equal("doors/door_latch3.wav", Assert.Single(decoder.SoundPlays).Name);
    }

    [Fact]
    public void Ph_soundplay_does_not_emit_truncated_or_unresolved_sound_paths()
    {
        var decoder = new PerpheadsMessageDecoder();
        var tableAdd = new BitWriter();
        tableAdd.UInt(1, 16);
        tableAdd.String("doors/door_latch3.wav");
        decoder.Process(4, "perpheads_stringtable_add", tableAdd.ToArray());
        var indexed = SoundPlayBody(stringTableIndex: 1, "doors/door_latch3.wav", out var bodyBits);

        decoder.Process(8, "ph_soundplay", indexed, bodyBits - 1);
        decoder.Process(9, "ph_soundplay", indexed, bodyBits);

        var sound = Assert.Single(decoder.SoundPlays);
        Assert.Equal(9, sound.Tick);
        Assert.Equal("doors/door_latch3.wav", sound.Name);

        var unresolvedDecoder = new PerpheadsMessageDecoder();
        unresolvedDecoder.Process(10, "ph_soundplay", indexed, bodyBits);
        Assert.Empty(unresolvedDecoder.SoundPlays);
    }

    [Fact]
    public void Svc_sounds_rejects_truncated_or_overdeclared_sound_bodies()
    {
        var payload = DefaultSourceSoundPayload();

        var completeReader = new BitReader(payload.Bytes);
        var sound = Assert.Single(SoundMessage.Read(ref completeReader, 10, 1, payload.BitCount));
        Assert.Equal(0, sound.Entity);
        Assert.Equal(0, sound.SoundNum);
        Assert.Equal(6, sound.Channel);
        Assert.Equal(1f, sound.Volume);
        Assert.Equal(75, sound.Level);
        Assert.Equal(100, sound.Pitch);

        var truncatedReader = new BitReader(payload.Bytes[..^1]);
        Assert.Empty(SoundMessage.Read(ref truncatedReader, 10, 1, payload.BitCount));
    }

    private static (byte[] Bytes, int BitCount) DefaultSourceSoundPayload()
    {
        var bits = new BitWriter();
        bits.Bool(false); // entity unchanged
        bits.Bool(false); // sound index unchanged
        bits.Bool(false); // flags unchanged
        bits.Bool(false); // channel unchanged
        bits.Bool(false); // ambient
        bits.Bool(false); // sentence
        bits.Bool(false);
        bits.Bool(false);
        bits.UInt(0, 10);
        for (var i = 0; i < 9; i++)
        {
            bits.Bool(false);
        }

        return (bits.ToArray(), bits.Count);
    }

    private static byte[] SoundPlayBody(int? stringTableIndex, string path, out int bitCount)
    {
        var bits = new BitWriter();
        if (stringTableIndex is { } index)
        {
            bits.Bool(true);
            bits.UInt((uint)index, 16);
        }
        else
        {
            bits.Bool(false);
            bits.String(path);
        }

        bits.Vector(12.5f, -3.25f, 0f);
        bits.UInt(70, 8);
        bits.UInt(95, 8);
        bits.Float(0.75f);
        bitCount = bits.Count;
        return bits.ToArray();
    }

    private sealed class BitWriter
    {
        private readonly List<byte> _bytes = [];

        public int Count { get; private set; }

        public void Bool(bool value) => UInt(value ? 1u : 0u, 1);

        public void UInt(uint value, int width)
        {
            for (var i = 0; i < width; i++)
            {
                if ((Count & 7) == 0)
                {
                    _bytes.Add(0);
                }

                if (((value >> i) & 1) != 0)
                {
                    _bytes[^1] |= (byte)(1 << (Count & 7));
                }

                Count++;
            }
        }

        public void Float(float value) => UInt(BitConverter.SingleToUInt32Bits(value), 32);

        public void String(string value)
        {
            foreach (var item in System.Text.Encoding.UTF8.GetBytes(value))
            {
                UInt(item, 8);
            }

            UInt(0, 8);
        }

        public void Vector(float x, float y, float z)
        {
            var values = new[] { x, y, z };
            foreach (var value in values)
            {
                Bool(value != 0);
            }

            foreach (var value in values.Where(value => value != 0))
            {
                var absolute = MathF.Abs(value);
                var integer = (uint)MathF.Floor(absolute);
                var fraction = (uint)MathF.Round((absolute - integer) * 32);
                Bool(integer > 0);
                Bool(fraction > 0);
                Bool(value < 0);
                if (integer > 0)
                {
                    UInt(integer - 1, 14);
                }

                if (fraction > 0)
                {
                    UInt(fraction, 5);
                }
            }
        }

        public byte[] ToArray() => CollectionsMarshal.AsSpan(_bytes).ToArray();
    }
}
