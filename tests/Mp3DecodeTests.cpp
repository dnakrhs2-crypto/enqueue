#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace gocue::tests
{

/** JUCE's MP3 decoder (tools/juce-patches/0004) on frames that borrow from the bit reservoir.

    tests/assets/reservoir48k.mp3: 3 s stereo 48 kHz, 128 kbps CBR, LAME via ffmpeg (a chord with a tremolo plus pink
    noise, no ID3), with LAME's Info frame first. Nearly every frame starts its data 430-510 bytes back in the frames
    before it - more than the 348 bytes one frame holds.

    A copy cut out of it (what ffmpeg -c copy does - the file a user heard a burst of noise at the start of,
    2026-10-25) starts with frames whose reservoir was never in the file: they must come out silent, in their place in
    time, and everything after them as in the uncut file. Reading after a jump (a loop, a trim start, a click in the
    wave) must give the same samples as reading in order. */
class Mp3DecodeTests : public juce::UnitTest
{
public:
    Mp3DecodeTests() : juce::UnitTest ("MP3 decoder", "Enqueue") {}

    static constexpr int frameLength = 1152;

    static juce::File findAsset()
    {
        // the test binary lives in build/vs2022/tests/EnqueueTests_artefacts/<config>/; walk up to the repo
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int i = 0; i < 8; ++i)
        {
            const auto candidate = dir.getChildFile ("tests").getChildFile ("assets").getChildFile ("reservoir48k.mp3");

            if (candidate.existsAsFile())
                return candidate;

            dir = dir.getParentDirectory();
        }

        return {};
    }

    /** Byte offsets of the MPEG-1 layer III frames in 'data' (the fixture has no tags around them). */
    static juce::Array<int> findFrames (const juce::MemoryBlock& data)
    {
        static const int bitrates[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
        static const int rates[] = { 44100, 48000, 32000, 0 };
        const auto* bytes = static_cast<const juce::uint8*> (data.getData());
        const int size = (int) data.getSize();
        juce::Array<int> offsets;

        for (int pos = 0; pos + 4 <= size;)
        {
            const auto header = ((juce::uint32) bytes[pos] << 24) | ((juce::uint32) bytes[pos + 1] << 16)
                              | ((juce::uint32) bytes[pos + 2] << 8) | (juce::uint32) bytes[pos + 3];
            const int bitrate = bitrates[(header >> 12) & 15];
            const int rate = rates[(header >> 10) & 3];

            if ((header >> 21) != 0x7ff || ((header >> 19) & 3) != 3 || ((header >> 17) & 3) != 1 || bitrate == 0 || rate == 0)
                break;

            offsets.add (pos);
            pos += 144000 * bitrate / rate + (int) ((header >> 9) & 1);
        }

        return offsets;
    }

    /** The file with frames [first, end) only, the Info frame (frame 0) in front if 'withInfo'. */
    static juce::MemoryBlock cut (const juce::MemoryBlock& data, const juce::Array<int>& frames, int first, bool withInfo)
    {
        juce::MemoryBlock result;
        const auto* bytes = static_cast<const char*> (data.getData());

        if (withInfo)
            result.append (bytes, (size_t) frames[1]);

        result.append (bytes + frames[first], data.getSize() - (size_t) frames[first]);
        return result;
    }

    static std::unique_ptr<juce::AudioFormatReader> open (const juce::MemoryBlock& data)
    {
        juce::MP3AudioFormat mp3;
        return std::unique_ptr<juce::AudioFormatReader> (mp3.createReaderFor (new juce::MemoryInputStream (data, true), true));
    }

    /** 'numSamples' from the start, in order block by block, as playback reads. */
    static juce::AudioBuffer<float> decodeInOrder (juce::AudioFormatReader& reader, int numSamples)
    {
        juce::AudioBuffer<float> out ((int) reader.numChannels, numSamples);
        out.clear();

        for (int pos = 0; pos < numSamples; pos += 4096)
            reader.read (&out, pos, juce::jmin (4096, numSamples - pos), pos, true, true);

        return out;
    }

    /** Largest difference between a[aStart...] and b[bStart...] over n samples, all channels. */
    static float maxDiff (const juce::AudioBuffer<float>& a, int aStart, const juce::AudioBuffer<float>& b, int bStart, int n)
    {
        float worst = 0.0f;

        for (int ch = 0; ch < juce::jmin (a.getNumChannels(), b.getNumChannels()); ++ch)
            for (int i = 0; i < n; ++i)
                worst = juce::jmax (worst, std::abs (a.getSample (ch, aStart + i) - b.getSample (ch, bStart + i)));

        return worst;
    }

    static float peak (const juce::AudioBuffer<float>& a, int start, int n)
    {
        float worst = 0.0f;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            worst = juce::jmax (worst, a.getMagnitude (ch, juce::jmax (0, start), juce::jmin (n, a.getNumSamples() - juce::jmax (0, start))));

        return worst;
    }

    static bool isSilent (const juce::AudioBuffer<float>& a, int start, int n)
    {
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < n; ++i)
                if (a.getSample (ch, start + i) != 0.0f)
                    return false;

        return true;
    }

    void runTest() override
    {
        const auto asset = findAsset();

        beginTest ("fixture");
        {
            if (asset == juce::File())
            {
                logMessage ("tests/assets/reservoir48k.mp3 not found: skipped");
                expect (false, "tests/assets/reservoir48k.mp3 not found");
                return;
            }
        }

        juce::MemoryBlock data;
        expect (asset.loadFileAsData (data));
        const auto frames = findFrames (data);
        expect (frames.size() > 100, "frames found: " + juce::String (frames.size()));

        if (frames.size() <= 100)
            return;

        const int audioFrames = frames.size() - 1;   // frame 0 is LAME's Info frame: no audio
        auto original = open (data);
        expect (original != nullptr);

        if (original == nullptr)
            return;

        const auto whole = decodeInOrder (*original, audioFrames * frameLength);

        beginTest ("every frame of the uncut file is decoded");
        {
            int silentFrames = 0;

            for (int f = 0; f < audioFrames; ++f)
                if (isSilent (whole, f * frameLength, frameLength))
                    ++silentFrames;

            expectEquals (silentFrames, 0);
            expect (peak (whole, 0, whole.getNumSamples()) > 0.3f);
        }

        beginTest ("a file cut out of a longer one: silent until its reservoir is there, then the same samples in the same place");
        {
            int checked = 0;
            juce::StringArray failures;

            for (const bool withInfo : { true, false })
            {
                for (int first = 2; first < audioFrames - 8; ++first)
                {
                    const auto copy = cut (data, frames, first, withInfo);
                    auto reader = open (copy);

                    if (reader == nullptr)
                    {
                        failures.add ("frame " + juce::String (first) + ": no reader");
                        continue;
                    }

                    const int copyFrames = audioFrames - first + 1;
                    const auto decoded = decodeInOrder (*reader, copyFrames * frameLength);
                    const int offset = (first - 1) * frameLength;   // where the copy's first frame is in the uncut file

                    // frames that reach back into what was cut away: silence (a 128 kbps frame holds 348 bytes, so
                    // two frames always cover the 511 the reservoir can reach)
                    int silent = 0;

                    while (silent < 3 && isSilent (decoded, silent * frameLength, frameLength))
                        ++silent;

                    // the first decoded frame lacks the overlap and filter history of the frame before (cut away):
                    // not exact, but no louder than the music around it
                    const float firstPeak = peak (decoded, silent * frameLength, frameLength);
                    const float aroundPeak = peak (whole, offset + (silent - 1) * frameLength, 3 * frameLength);

                    // from the next frame on, the samples of the uncut file
                    const int from = (silent + 1) * frameLength;
                    const float diff = maxDiff (decoded, from, whole, offset + from, (copyFrames - silent - 2) * frameLength);

                    ++checked;

                    if (silent > 2 || firstPeak > 1.5f * aroundPeak || diff > 1.0e-4f)
                        failures.add ("frame " + juce::String (first) + (withInfo ? " with" : " without") + " Info: "
                                      + juce::String (silent) + " silent frames, first decoded peak " + juce::String (firstPeak, 3)
                                      + " (around " + juce::String (aroundPeak, 3) + "), then max diff " + juce::String (diff, 6));
                }
            }

            logMessage (juce::String (checked) + " cut copies, " + juce::String (failures.size()) + " wrong"
                        + (failures.isEmpty() ? juce::String() : ": " + failures.joinIntoString ("; ").substring (0, 600)));
            expectEquals (failures.size(), 0);
            expect (checked > 200);
        }

        beginTest ("reading after a jump gives the samples reading in order gives");
        {
            juce::StringArray failures;
            int checked = 0;

            for (const int first : { 1, 13, 41 })
            {
                for (const bool withInfo : { true, false })
                {
                    if (first == 1 && ! withInfo)
                        continue;   // that's the uncut file without its Info frame: covered by the others

                    const auto copy = first == 1 ? data : cut (data, frames, first, withInfo);
                    const int copyFrames = audioFrames - first + 1;
                    auto inOrderReader = open (copy);
                    auto jumpReader = open (copy);

                    if (inOrderReader == nullptr || jumpReader == nullptr)
                    {
                        failures.add ("frame " + juce::String (first) + ": no reader");
                        continue;
                    }

                    const auto inOrder = decodeInOrder (*inOrderReader, copyFrames * frameLength);
                    const int chunk = 3000;

                    // jumps forward and back, onto frame starts, just before and after them and in between
                    for (const int pos : { 0, 1, 1151, 1152, 1153, 5000, 4 * 1152 - 1, 4 * 1152, 7 * 1152 + 5, 13 * 1152 + 600,
                                           30000, 9 * 1152 + 1, 40000, 52345, 20 * 1152, 61111, 2500, 44 * 1152 - 2 })
                    {
                        if (pos + chunk > inOrder.getNumSamples())
                            continue;

                        juce::AudioBuffer<float> got (inOrder.getNumChannels(), chunk);
                        got.clear();
                        jumpReader->read (&got, 0, chunk, pos, true, true);
                        const float diff = maxDiff (got, 0, inOrder, pos, chunk);
                        ++checked;

                        if (diff > 1.0e-4f)
                            failures.add ("frame " + juce::String (first) + (withInfo ? " with" : " without") + " Info, jump to "
                                          + juce::String (pos) + ": max diff " + juce::String (diff, 6));
                    }
                }
            }

            logMessage (juce::String (checked) + " jumps, " + juce::String (failures.size()) + " wrong"
                        + (failures.isEmpty() ? juce::String() : ": " + failures.joinIntoString ("; ").substring (0, 600)));
            expectEquals (failures.size(), 0);
            expect (checked > 60);
        }

        // tests/assets/lsf24k.mp3: 3.000 s mono 24 kHz, 32 kbps, LAME via ffmpeg (a tone with a tremolo plus pink noise),
        // with LAME's Info frame first. MPEG-2 (16 / 22.05 / 24 kHz) frames hold 576 samples, not 1152.
        beginTest ("an MPEG-2 file (24 kHz): its own length, nothing after its end, jumps land right");
        {
            juce::MemoryBlock lsfData;
            expect (asset.getSiblingFile ("lsf24k.mp3").loadFileAsData (lsfData), "tests/assets/lsf24k.mp3 not found");
            auto inOrderReader = open (lsfData);
            auto jumpReader = open (lsfData);
            expect (inOrderReader != nullptr && jumpReader != nullptr);

            if (inOrderReader != nullptr && jumpReader != nullptr)
            {
                // the 3 s made plus LAME's encoder delay and the last frame's padding (a frame is 24 ms here)
                const int realEnd = 72000 + 4 * 576;
                const auto length = inOrderReader->lengthInSamples;
                expect (length >= 72000 && length <= realEnd, "length " + juce::String (length));

                // JUCE used to count 1152 samples a frame here: twice the length, and earlier audio played again past
                // the real end
                const auto inOrder = decodeInOrder (*inOrderReader, realEnd + 20000);
                expect (peak (inOrder, 0, 72000) > 0.2f);
                expect (isSilent (inOrder, realEnd, 20000), "sound after the end: peak " + juce::String (peak (inOrder, realEnd, 20000), 3));

                int wrong = 0;

                for (const int pos : { 0, 575, 576, 577, 4 * 576, 6000, 20 * 576, 33333, 2304, 50000, 60000, 1 })
                {
                    juce::AudioBuffer<float> got (1, 3000);
                    got.clear();
                    jumpReader->read (&got, 0, 3000, pos, true, true);

                    if (maxDiff (got, 0, inOrder, pos, 3000) > 1.0e-4f)
                        ++wrong;
                }

                expectEquals (wrong, 0);
            }
        }

        // tests/assets/lsf24k_vbr.mp3: 5 s stereo 24 kHz, LAME VBR (-q:a 4) via ffmpeg: silence, a tremolo chord from 2 s to
        // 3.2 s, silence, a tone from 3.7 s to 4.6 s. LAME stores the silence in 8 kbps frames with 3 bytes of main data
        // each, so the first frame of sound reaches about 240 bytes back: a fixed 10-frame pre-roll left it silent
        // after a jump to just before the sound (a trim start set at the end of a pause).
        beginTest ("a 24 kHz VBR file: a jump to just before the sound after a pause starts with the sound");
        {
            juce::MemoryBlock vbrData;
            expect (asset.getSiblingFile ("lsf24k_vbr.mp3").loadFileAsData (vbrData), "tests/assets/lsf24k_vbr.mp3 not found");
            auto inOrderReader = open (vbrData);
            auto jumpReader = open (vbrData);
            expect (inOrderReader != nullptr && jumpReader != nullptr);

            if (inOrderReader != nullptr && jumpReader != nullptr)
            {
                const auto inOrder = decodeInOrder (*inOrderReader, 5 * 24000);
                juce::Array<int> onsets;

                // where the sound starts after each pause (encoder delay included; the pause decodes to below -60 dB)
                for (int i = 24000; i < inOrder.getNumSamples() && onsets.size() < 2; ++i)
                {
                    if (std::abs (inOrder.getSample (0, i)) > 0.01f && peak (inOrder, i - 4800, 4000) < 1.0e-3f)
                    {
                        onsets.add (i);
                        i += 24000;   // past this sound
                    }
                }

                expectEquals (onsets.size(), 2);
                juce::StringArray wrong;

                for (const auto onset : onsets)
                {
                    for (const int before : { 300, 1200, 2400, 4800, 9000 })
                    {
                        juce::AudioBuffer<float> got (inOrder.getNumChannels(), 4000);
                        got.clear();
                        jumpReader->read (&got, 0, 4000, onset - before, true, true);
                        const float diff = maxDiff (got, 0, inOrder, onset - before, 4000);

                        if (diff > 1.0e-4f)
                            wrong.add (juce::String (before) + " before " + juce::String (onset) + ": max diff " + juce::String (diff, 4));
                    }
                }

                expect (wrong.isEmpty(), wrong.joinIntoString ("; "));
            }
        }
    }
};

static Mp3DecodeTests mp3DecodeTests;

} // namespace gocue::tests
