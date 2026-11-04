using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

/// <summary>
/// Establishes the bit ordering and encodings Valve's bf_read uses. Expected values here are
/// derived by hand from the documented encodings, not from running the implementation.
/// </summary>
public sealed class BitReaderTests
{
    [Fact]
    public void Bits_are_consumed_least_significant_first()
    {
        // 0xB1 = 1011_0001. Read LSB first the sequence is 1,0,0,0,1,1,0,1.
        var reader = new BitReader([0xB1]);

        Assert.Equal(1, reader.ReadOneBit());
        Assert.Equal(0, reader.ReadOneBit());
        Assert.Equal(0, reader.ReadOneBit());
        Assert.Equal(0, reader.ReadOneBit());
        Assert.Equal(1, reader.ReadOneBit());
        Assert.Equal(1, reader.ReadOneBit());
        Assert.Equal(0, reader.ReadOneBit());
        Assert.Equal(1, reader.ReadOneBit());
    }

    [Fact]
    public void Multi_bit_fields_fill_from_the_low_bit()
    {
        var reader = new BitReader([0xB1]);

        Assert.Equal(0x1u, reader.ReadUBitLong(4));
        Assert.Equal(0xBu, reader.ReadUBitLong(4));
    }

    [Fact]
    public void Fields_span_byte_boundaries()
    {
        // Take the high nibble of 0xFF then the low nibble of 0x00: 0000_1111.
        var reader = new BitReader([0xFF, 0x00]);

        Assert.Equal(0xFu, reader.ReadUBitLong(4));
        Assert.Equal(0x0Fu, reader.ReadUBitLong(8));
    }

    [Fact]
    public void Word_and_long_are_little_endian()
    {
        var word = new BitReader([0x34, 0x12]);
        Assert.Equal(0x1234, word.ReadWord());

        var dword = new BitReader([0x78, 0x56, 0x34, 0x12]);
        Assert.Equal(0x12345678, dword.ReadLong());
    }

    [Fact]
    public void Signed_fields_sign_extend_from_their_own_width()
    {
        var reader = new BitReader([0x0F]);

        Assert.Equal(-1, reader.ReadSBitLong(4));
    }

    [Fact]
    public void Signed_fields_keep_positive_values_intact()
    {
        var reader = new BitReader([0x07]);

        Assert.Equal(7, reader.ReadSBitLong(4));
    }

    [Theory]
    [InlineData(new byte[] { 0x00 }, 0u)]
    [InlineData(new byte[] { 0x01 }, 1u)]
    [InlineData(new byte[] { 0x7F }, 127u)]
    [InlineData(new byte[] { 0xAC, 0x02 }, 300u)]
    [InlineData(new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, uint.MaxValue)]
    public void VarInt32_decodes_leb128(byte[] data, uint expected)
    {
        var reader = new BitReader(data);

        Assert.Equal(expected, reader.ReadVarInt32());
    }

    [Fact]
    public void VarInt32_is_not_byte_aligned()
    {
        // Skip one bit first: the varint must still read whole bytes from the shifted position.
        // 300 encodes as AC 02; offset by a leading zero bit that occupies 17 bits, so three
        // bytes are needed to hold it.
        var reader = new BitReader([0x58, 0x05, 0x00]);
        reader.ReadOneBit();

        Assert.Equal(300u, reader.ReadVarInt32());
    }

    [Fact]
    public void UBitVar_selector_zero_reads_four_bits()
    {
        // selector 00, then value 5 in 4 bits => byte 0b0001_0100.
        var reader = new BitReader([0x14]);

        Assert.Equal(5u, reader.ReadUBitVar());
    }

    [Fact]
    public void UBitVar_selector_one_reads_eight_bits()
    {
        // selector 01, then 200 in 8 bits, packed LSB first across two bytes.
        var reader = new BitReader([0x21, 0x03]);

        Assert.Equal(200u, reader.ReadUBitVar());
    }

    [Fact]
    public void BitCoord_with_no_flags_is_zero_and_costs_two_bits()
    {
        var reader = new BitReader([0x00]);

        Assert.Equal(0f, reader.ReadBitCoord());
        Assert.Equal(2, reader.BitsRead);
    }

    [Fact]
    public void BitCoord_integer_part_is_offset_by_one()
    {
        // hasInteger=1, hasFraction=0, sign=0, then 14 bits of value 0, which decodes as 1.0.
        var reader = new BitReader([0b0000_0001, 0x00, 0x00]);

        Assert.Equal(1f, reader.ReadBitCoord());
    }

    [Fact]
    public void BitAngle_scales_by_the_field_width()
    {
        // 64 of 256 steps is a quarter turn.
        var reader = new BitReader([64]);

        Assert.Equal(90f, reader.ReadBitAngle(8));
    }

    [Fact]
    public void Vec3Coord_skips_absent_axes()
    {
        // Flags 1,0,0: only x is present, and it uses the two-bit zero encoding.
        var reader = new BitReader([0b0000_0001]);

        var value = reader.ReadBitVec3Coord();

        Assert.Equal(0f, value.X);
        Assert.Equal(0f, value.Y);
        Assert.Equal(0f, value.Z);
        Assert.Equal(5, reader.BitsRead);
    }

    [Fact]
    public void Strings_stop_at_the_terminator()
    {
        var reader = new BitReader([(byte)'h', (byte)'i', 0, (byte)'x']);

        Assert.Equal("hi", reader.ReadString());
        Assert.Equal(24, reader.BitsRead);
    }

    [Fact]
    public void Truncated_strings_still_consume_through_the_terminator()
    {
        // Losing characters must not shift the stream, or every later message would desync.
        var reader = new BitReader([(byte)'a', (byte)'b', (byte)'c', 0]);

        Assert.Equal("a", reader.ReadString(maxLength: 1));
        Assert.Equal(32, reader.BitsRead);
    }

    [Fact]
    public void Overflow_yields_zeros_and_sets_the_flag_instead_of_throwing()
    {
        var reader = new BitReader([0xFF]);

        Assert.Equal(0xFFu, reader.ReadUBitLong(8));
        Assert.False(reader.IsOverflowed);

        Assert.Equal(0u, reader.ReadUBitLong(8));
        Assert.True(reader.IsOverflowed);
    }

    [Fact]
    public void Bits_left_tracks_consumption()
    {
        var reader = new BitReader([0x00, 0x00]);

        Assert.Equal(16, reader.BitsLeft);
        reader.ReadUBitLong(3);
        Assert.Equal(13, reader.BitsLeft);
    }

    [Fact]
    public void Seek_relative_moves_the_cursor()
    {
        var reader = new BitReader([0xFF, 0x0F]);

        Assert.True(reader.SeekRelative(12));
        Assert.Equal(0x0u, reader.ReadUBitLong(4));
    }

    [Fact]
    public void Seek_past_the_end_fails_and_flags_overflow()
    {
        var reader = new BitReader([0x00]);

        Assert.False(reader.Seek(9));
        Assert.True(reader.IsOverflowed);
    }
}
