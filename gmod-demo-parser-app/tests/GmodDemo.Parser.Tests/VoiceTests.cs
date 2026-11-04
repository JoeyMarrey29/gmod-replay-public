using System.Runtime.InteropServices;
using System.Text;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class VoiceTests
{
    [Fact]
    public void Steam_voice_frame_parses_magic_sample_rate_and_opus_length()
    {
        var body = new List<byte>();
        body.AddRange([0xFF, 0x39, 0x32, 0x46, 0x01, 0x00, 0x10, 0x01]);
        body.Add(11);
        body.AddRange(BitConverter.GetBytes((ushort)24000));
        body.Add(6);
        body.AddRange(BitConverter.GetBytes((ushort)8));
        body.AddRange(BitConverter.GetBytes((ushort)4));
        body.AddRange(BitConverter.GetBytes((ushort)7));
        body.AddRange([0x11, 0x22, 0x33, 0x44]);
        var crc = SteamVoice.Crc32(CollectionsMarshal.AsSpan(body));
        body.AddRange(BitConverter.GetBytes(crc));

        Assert.True(SteamVoice.HasMagic(body.ToArray()));
        Assert.True(SteamVoice.TryParse(body.ToArray(), out var frame));
        Assert.Equal(24000, frame.SampleRate);
        Assert.Equal(8, frame.OpusBytes);
        Assert.True(SteamVoice.TryEnumerate(body.ToArray(), out var ops));
        var opus = Assert.Single(ops, o => o.Kind == SteamVoiceOpKind.Opus);
        Assert.Equal(7, opus.Value);
        Assert.Equal([0x11, 0x22, 0x33, 0x44], opus.Packet);
    }

    [Fact]
    public void Wave_file_is_riff_pcm_mono_16()
    {
        var path = Path.Combine(Path.GetTempPath(), $"gmoddem-wav-{Guid.NewGuid():N}.wav");
        try
        {
            WaveFile.Write(path, 24000, [0, 1, -1, 12345]);
            var bytes = File.ReadAllBytes(path);
            Assert.True(WaveFile.IsRiffWave(bytes));
            Assert.Equal(44 + 8, bytes.Length);
            Assert.Equal(24000, BitConverter.ToInt32(bytes, 24));
            Assert.Equal(16, BitConverter.ToInt16(bytes, 34));
        }
        finally
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    [Fact]
    public void Utterances_split_when_the_tick_gap_is_larger_than_the_threshold()
    {
        var packets = new[]
        {
            new VoicePacket(10, 1, false, [1]),
            new VoicePacket(11, 1, false, [2]),
            new VoicePacket(12, 1, false, [3]),
            new VoicePacket(40, 1, false, [4]),
            new VoicePacket(41, 1, false, [5]),
        };

        var lines = VoiceTrack.SplitUtterances(packets, gapTicks: 20);
        Assert.Equal(2, lines.Count);
        Assert.Equal([10, 11, 12], lines[0].Select(p => p.Tick));
        Assert.Equal([40, 41], lines[1].Select(p => p.Tick));
    }

    [Fact]
    public void Utterances_split_when_proximity_mode_changes()
    {
        var packets = new[]
        {
            new VoicePacket(10, 1, false, [1]),
            new VoicePacket(11, 1, false, [2]),
            new VoicePacket(12, 1, true, [3]),
            new VoicePacket(13, 1, true, [4]),
        };

        var lines = VoiceTrack.SplitUtterances(packets, gapTicks: 20);
        Assert.Equal(2, lines.Count);
        Assert.All(lines[0], packet => Assert.False(packet.Proximity));
        Assert.All(lines[1], packet => Assert.True(packet.Proximity));
    }

    [Fact]
    public void Sidecars_are_one_file_per_line_named_with_the_start_tick()
    {
        var track = new VoiceTrack();
        track.AddPacket(100, 3, false, [1]);
        track.AddPacket(101, 3, false, [2]);
        track.AddPacket(200, 3, false, [3]);
        var players = new[]
        {
            new DemoPlayer(0, 3, null, "A", "STEAM_0:0:1", "76561197960265730", false, false),
        };
        var jsonPath = Path.Combine(Path.GetTempPath(), $"gmoddem-voice-unit-{Guid.NewGuid():N}.json");
        var audioDir = Path.ChangeExtension(jsonPath, ".audio");
        try
        {
            var clips = track.WriteSidecars(jsonPath, players, 66.6666666667);
            Assert.Equal(2, clips.Count);
            Assert.Equal(100, clips[0].StartTick);
            Assert.Equal(200, clips[1].StartTick);
            Assert.EndsWith("76561197960265730_100.gmodvoice", clips[0].File.Replace('\\', '/'));
            Assert.EndsWith("76561197960265730_200.gmodvoice", clips[1].File.Replace('\\', '/'));
            Assert.Equal(100 / 66.6666666667, clips[0].Time, 5);
            Assert.All(clips, clip => Assert.False(clip.Proximity));
            foreach (var clip in clips)
            {
                var full = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(jsonPath)!, clip.File));
                Assert.True(File.Exists(full), clip.File);
                using var stream = File.OpenRead(full);
                var magic = new byte[VoiceTrack.FileMagic.Length];
                Assert.Equal(magic.Length, stream.Read(magic));
                Assert.Equal(VoiceTrack.FileMagic, Encoding.ASCII.GetString(magic));
            }
        }
        finally
        {
            if (Directory.Exists(audioDir))
            {
                Directory.Delete(audioDir, true);
            }
        }
    }
}
