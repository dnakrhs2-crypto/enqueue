#include "audio/MediaFoundationAudioFormat.h"

#include <juce_core/juce_core.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace gocue
{

namespace
{
    template <typename T>
    struct ComPtr
    {
        T* p = nullptr;
        ComPtr() = default;
        ~ComPtr() { reset(); }
        ComPtr (const ComPtr&) = delete;
        ComPtr& operator= (const ComPtr&) = delete;
        void reset() { if (p != nullptr) { p->Release(); p = nullptr; } }
        T** put() { reset(); return &p; }
        T* operator->() const noexcept { return p; }
        explicit operator bool() const noexcept { return p != nullptr; }
    };

    /** COM wants every thread that touches its objects initialised: the reader is created on the message
        thread and read on JUCE's read-ahead thread, so each of them calls this once. */
    void ensureComOnThisThread()
    {
        thread_local bool done = false;

        if (! done)
        {
            done = true;
            CoInitializeEx (nullptr, COINIT_MULTITHREADED);   // S_FALSE / RPC_E_CHANGED_MODE are fine: already initialised
        }
    }

    bool startMediaFoundation()
    {
        static std::once_flag once;
        static bool ok = false;
        ensureComOnThisThread();

        std::call_once (once, []
        {
            // the DLLs are delay-loaded: on a Windows N edition without the Media Feature Pack they are absent, and
            // calling MFStartup would raise a loader exception; probing first keeps the app alive without AAC/WMA
            if (LoadLibraryW (L"mfplat.dll") == nullptr || LoadLibraryW (L"mfreadwrite.dll") == nullptr)
            {
                ok = false;
                return;
            }

            ok = SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE));
        });

        return ok;
    }

    constexpr juce::int64 ticksPerSecond = 10000000;   // Media Foundation time is in 100 ns units

    /** Start of the first edit of the first audio track of an MP4/M4A file, in samples at 'sampleRate' (0 when the
        file has no edit list, is not MP4, or cannot be read). The AAC encoder delay (priming, usually 1024 samples)
        is expressed as this edit list start: presentation time 0 is media time 'editListStart'. */
    juce::int64 mp4EditListStart (const juce::File& file, double sampleRate)
    {
        juce::FileInputStream in (file);

        if (! in.openedOk() || sampleRate <= 0)
            return 0;

        const juce::int64 fileSize = in.getTotalLength();

        struct Box { juce::int64 payload, end; juce::String type; };

        // reads the box header at 'at' inside [at, limit)
        auto readBox = [&] (juce::int64 at, juce::int64 limit, Box& box) -> bool
        {
            if (at < 0 || at + 8 > limit || ! in.setPosition (at))
                return false;

            juce::int64 size = (juce::uint32) in.readIntBigEndian();
            char type[5] = {};

            if (in.read (type, 4) != 4)
                return false;

            juce::int64 payload = at + 8;

            if (size == 1)
            {
                size = in.readInt64BigEndian();
                payload = at + 16;
            }
            else if (size == 0)
            {
                size = limit - at;
            }

            if (size < payload - at || at + size > limit)
                return false;

            box = { payload, at + size, juce::String (juce::CharPointer_ASCII (type)) };
            return true;
        };

        auto findChild = [&] (const Box& parent, const char* wanted, Box& found) -> bool
        {
            for (juce::int64 at = parent.payload; at + 8 <= parent.end;)
            {
                Box child;

                if (! readBox (at, parent.end, child))
                    return false;

                if (child.type == wanted)
                {
                    found = child;
                    return true;
                }

                if (child.end <= at)
                    return false;

                at = child.end;
            }

            return false;
        };

        Box root { 0, fileSize, "" }, moov;

        if (! findChild (root, "moov", moov))
            return 0;

        for (juce::int64 at = moov.payload; at + 8 <= moov.end;)
        {
            Box trak;

            if (! readBox (at, moov.end, trak) || trak.end <= at)
                return 0;

            at = trak.end;

            if (trak.type != "trak")
                continue;

            Box mdia, hdlr, mdhd, edts, elst;

            if (! findChild (trak, "mdia", mdia) || ! findChild (mdia, "hdlr", hdlr) || ! findChild (mdia, "mdhd", mdhd))
                continue;

            char handler[5] = {};

            if (! in.setPosition (hdlr.payload + 8) || in.read (handler, 4) != 4 || juce::String (juce::CharPointer_ASCII (handler)) != "soun")
                continue;   // not the audio track

            if (! in.setPosition (mdhd.payload))
                return 0;

            const int mdhdVersion = in.readByte();
            in.skipNextBytes (3);
            in.skipNextBytes (mdhdVersion == 1 ? 16 : 8);   // creation + modification time
            const juce::int64 timescale = (juce::uint32) in.readIntBigEndian();

            if (timescale <= 0 || ! findChild (trak, "edts", edts) || ! findChild (edts, "elst", elst) || ! in.setPosition (elst.payload))
                return 0;

            const int elstVersion = in.readByte();
            in.skipNextBytes (3);
            const int entries = in.readIntBigEndian();

            for (int i = 0; i < entries; ++i)
            {
                juce::int64 mediaTime = 0;

                if (elstVersion == 1)
                {
                    in.readInt64BigEndian();              // segment duration
                    mediaTime = in.readInt64BigEndian();
                }
                else
                {
                    in.readIntBigEndian();
                    mediaTime = in.readIntBigEndian();
                }

                in.readIntBigEndian();                    // media rate (16.16)

                if (mediaTime < 0)
                    continue;   // an empty edit (presentation delay) carries no media offset

                return (juce::int64) std::llround ((double) mediaTime * sampleRate / (double) timescale);
            }

            return 0;
        }

        return 0;
    }

    /** One decoded buffer: interleaved float frames with the file position of its first frame. */
    struct DecodedBuffer
    {
        std::vector<float> data;
        int frames = 0;
        juce::int64 position = 0;
    };

    class MediaFoundationReader : public juce::AudioFormatReader
    {
    public:
        MediaFoundationReader (juce::InputStream* stream, const juce::String& formatName, const juce::File& file)
            : juce::AudioFormatReader (stream, formatName)
        {
            usesFloatingPointData = true;
            bitsPerSample = 32;

            if (! startMediaFoundation())
                return;

            ComPtr<IMFAttributes> attributes;

            if (FAILED (MFCreateAttributes (attributes.put(), 1)))
                return;

            attributes->SetUINT32 (MF_LOW_LATENCY, FALSE);

            if (FAILED (MFCreateSourceReaderFromURL (file.getFullPathName().toWideCharPointer(), attributes.p, reader.put())))
                return;

            reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
            reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

            ComPtr<IMFMediaType> wanted;

            if (FAILED (MFCreateMediaType (wanted.put())))
                return;

            wanted->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            wanted->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);   // float PCM at the native rate / channel count

            if (FAILED (reader->SetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, wanted.p)))
                return;

            ComPtr<IMFMediaType> actual;

            if (FAILED (reader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, actual.put())))
                return;

            UINT32 rate = 0, channels = 0, bits = 0;
            actual->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            actual->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channels);
            actual->GetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);

            if (rate == 0 || channels == 0 || bits != 32)
                return;

            sampleRate = (double) rate;
            numChannels = channels;

            PROPVARIANT duration;
            PropVariantInit (&duration);

            if (SUCCEEDED (reader->GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)))
            {
                juce::int64 ticks = 0;

                if (duration.vt == VT_UI8)
                    ticks = (juce::int64) duration.uhVal.QuadPart;
                else if (duration.vt == VT_I8)
                    ticks = duration.hVal.QuadPart;

                lengthInSamples = toSamples (ticks);
            }

            PropVariantClear (&duration);
            valid = lengthInSamples > 0;

            if (valid)
            {
                seekTo (0);

                // Windows before the 2026-09 update applied the MP4 edit list itself and handed the AAC priming frame
                // over with the same timestamp as the first real frame (nextPending() drops that duplicate). Since that
                // update the MP4 source hands out raw media timestamps: priming at 0, first real frame one frame later,
                // and the presentation would start 1024 samples late. Read the edit list start from the file and tell the
                // two behaviours apart from the first two buffers; in the raw case every position is shifted back by the
                // edit list start so the priming frame lands before 0 and is skipped like a seek preroll.
                editListStart = file.hasFileExtension ("m4a;mp4;m4b") ? mp4EditListStart (file, sampleRate) : 0;

                if (editListStart > 0)
                {
                    DecodedBuffer first, second;

                    if (readOne (first) && readOne (second) && second.position > first.position)
                        rawMediaTimestamps = true;

                    seekTo (0);
                }
            }
        }

        bool isValid() const noexcept { return valid; }

        bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                          juce::int64 startSampleInFile, int numSamples) override
        {
            clearSamplesBeyondAvailableLength (destChannels, numDestChannels, startOffsetInDestBuffer,
                                               startSampleInFile, numSamples, lengthInSamples);

            if (numSamples <= 0)
                return true;

            if (! valid)
            {
                clearDest (destChannels, numDestChannels, startOffsetInDestBuffer, numSamples);
                return false;
            }

            ensureComOnThisThread();

            if (startSampleInFile != position || seekFailed)
                seekTo (startSampleInFile);

            if (seekFailed)
            {
                clearDest (destChannels, numDestChannels, startOffsetInDestBuffer, numSamples);   // never serve audio from the wrong place
                return false;
            }

            int written = 0;

            while (written < numSamples)
            {
                if (pendingOffset >= pending.frames)
                {
                    if (! nextPending())
                        break;   // end of stream (or an error): the rest is silence

                    continue;
                }

                const int n = std::min (pending.frames - pendingOffset, numSamples - written);
                const float* src = pending.data.data() + (size_t) pendingOffset * numChannels;

                for (int ch = 0; ch < numDestChannels; ++ch)
                {
                    if (destChannels[ch] == nullptr)
                        continue;

                    float* dest = reinterpret_cast<float*> (destChannels[ch]) + startOffsetInDestBuffer + written;
                    const unsigned int sourceChannel = (unsigned int) ch < numChannels ? (unsigned int) ch : 0;

                    for (int i = 0; i < n; ++i)
                        dest[i] = src[(size_t) i * numChannels + sourceChannel];
                }

                pendingOffset += n;
                written += n;
            }

            if (written < numSamples)
                clearDest (destChannels, numDestChannels, startOffsetInDestBuffer + written, numSamples - written);

            position = startSampleInFile + numSamples;
            return true;
        }

    private:
        juce::int64 toSamples (juce::int64 ticks) const noexcept
        {
            return (juce::int64) std::llround ((double) ticks * sampleRate / (double) ticksPerSecond);
        }

        static void clearDest (int* const* destChannels, int numDestChannels, int offset, int numSamples)
        {
            for (int ch = 0; ch < numDestChannels; ++ch)
                if (destChannels[ch] != nullptr)
                    juce::FloatVectorOperations::clear (reinterpret_cast<float*> (destChannels[ch]) + offset, numSamples);
        }

        void seekTo (juce::int64 sample)
        {
            pending.frames = 0;
            pendingOffset = 0;
            haveLookahead = false;
            endOfStream = false;

            // Start two AAC frames early: the first frame decoded after a seek lacks the previous frame's
            // overlap-add and comes out wrong. Everything before 'sample' is discarded (skipUntil).
            constexpr juce::int64 preroll = 2048;
            const juce::int64 target = std::max<juce::int64> (0, sample);
            // MF is positioned in raw media time; the preroll may reach back into the priming frame (raw 0), which the
            // decoder needs as overlap history for the first real frame, and skipUntil drops everything before 'target'.
            const juce::int64 rawTarget = target + (rawMediaTimestamps ? editListStart : 0);
            const juce::int64 rawFrom = std::max<juce::int64> (0, rawTarget - preroll);

            PROPVARIANT pos;
            PropVariantInit (&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = (LONGLONG) ((double) rawFrom * (double) ticksPerSecond / sampleRate);
            const bool ok = SUCCEEDED (reader->SetCurrentPosition (GUID_NULL, pos));
            PropVariantClear (&pos);

            position = sample;
            seekFailed = ! ok;
            skipUntil = ok ? target : -1;
            expectedNext = -1;
            buffersSinceSeek = 0;
        }

        /** After a media-type change notification: the stream must still be float PCM at the same rate / channels. */
        bool formatStillMatches()
        {
            ComPtr<IMFMediaType> actual;

            if (FAILED (reader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, actual.put())))
                return false;

            UINT32 rate = 0, channels = 0, bits = 0;
            actual->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            actual->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channels);
            actual->GetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            return rate == (UINT32) sampleRate && channels == numChannels && bits == 32;
        }

        /** Pulls one buffer from the decoder. Returns false at the end of the stream. */
        bool readOne (DecodedBuffer& out)
        {
            while (! endOfStream)
            {
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;

                if (FAILED (reader->ReadSample ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, sample.put()))
                    || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) != 0)
                {
                    endOfStream = true;   // a decoder error ends the stream: silence instead of stale data
                    return false;
                }

                if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 && ! formatStillMatches())
                {
                    endOfStream = true;   // the buffers would no longer be what we assume: stop rather than misread them
                    return false;
                }

                if (! sample)
                    continue;   // a stream tick or gap notification carries no data

                ComPtr<IMFMediaBuffer> buffer;

                if (FAILED (sample->ConvertToContiguousBuffer (buffer.put())))
                    continue;

                BYTE* data = nullptr;
                DWORD length = 0;

                if (FAILED (buffer->Lock (&data, nullptr, &length)))
                    continue;

                out.frames = (int) (length / (sizeof (float) * numChannels));
                out.data.resize ((size_t) out.frames * numChannels);
                std::copy (reinterpret_cast<const float*> (data), reinterpret_cast<const float*> (data) + (size_t) out.frames * numChannels, out.data.begin());
                buffer->Unlock();
                out.position = toSamples (timestamp) - (rawMediaTimestamps ? editListStart : 0);

                if (out.frames > 0)
                    return true;
            }

            return false;
        }

        /** Makes 'pending' the next buffer to deliver (after the skip), looking one buffer ahead:
            the decoder's priming frame carries the same timestamp as the first real frame, so a buffer is
            only delivered once the following one has a later timestamp. */
        bool nextPending()
        {
            while (true)
            {
                DecodedBuffer next;
                const bool got = readOne (next);

                if (! got)
                {
                    if (! haveLookahead)
                        return false;

                    haveLookahead = false;
                    std::swap (pending, lookahead);
                }
                else if (! haveLookahead)
                {
                    std::swap (lookahead, next);
                    haveLookahead = true;
                    continue;
                }
                else if (next.position <= lookahead.position && buffersSinceSeek == 0)
                {
                    std::swap (lookahead, next);   // duplicate timestamp right after a seek: the earlier buffer was decoder priming
                    continue;
                }
                else
                {
                    std::swap (pending, lookahead);
                    std::swap (lookahead, next);
                    haveLookahead = true;
                }

                // timestamps come from the container: keep the stream sample-continuous across rounding jitter
                if (expectedNext >= 0 && std::abs (pending.position - expectedNext) <= 2)
                    pending.position = expectedNext;

                expectedNext = pending.position + pending.frames;
                pendingOffset = 0;
                ++buffersSinceSeek;

                if (skipUntil >= 0)
                {
                    if (pending.position + pending.frames <= skipUntil)
                    {
                        pending.frames = 0;   // entirely before the target
                        continue;
                    }

                    pendingOffset = (int) std::max<juce::int64> (0, skipUntil - pending.position);
                    skipUntil = -1;
                }

                return pending.frames > pendingOffset;
            }
        }

        ComPtr<IMFSourceReader> reader;
        bool valid = false;
        bool endOfStream = false;
        juce::int64 position = 0;       // next sample readSamples() delivers without a seek
        juce::int64 skipUntil = -1;
        juce::int64 expectedNext = -1;
        DecodedBuffer pending, lookahead;
        bool haveLookahead = false;
        bool seekFailed = false;
        int buffersSinceSeek = 0;
        int pendingOffset = 0;
        juce::int64 editListStart = 0;      // MP4 edit list start (AAC priming) in samples, 0 without one
        bool rawMediaTimestamps = false;    // this Windows hands out raw media timestamps (edit list not applied)
    };
}

//==============================================================================
MediaFoundationAudioFormat::MediaFoundationAudioFormat()
    : juce::AudioFormat ("AAC/M4A/WMA (Media Foundation)", { ".m4a", ".aac", ".mp4", ".m4b", ".wma" })   // WMA decodes through the same Windows codecs
{
}

MediaFoundationAudioFormat::~MediaFoundationAudioFormat() = default;

bool MediaFoundationAudioFormat::isAvailable()
{
    return startMediaFoundation();
}

juce::Array<int> MediaFoundationAudioFormat::getPossibleSampleRates()
{
    return { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000 };
}

juce::Array<int> MediaFoundationAudioFormat::getPossibleBitDepths()
{
    return { 16, 24, 32 };
}

juce::AudioFormatReader* MediaFoundationAudioFormat::createReaderFor (juce::InputStream* sourceStream, bool deleteStreamIfOpeningFails)
{
    auto* fileStream = dynamic_cast<juce::FileInputStream*> (sourceStream);

    if (fileStream == nullptr)
    {
        if (deleteStreamIfOpeningFails)
            delete sourceStream;

        return nullptr;
    }

    std::unique_ptr<MediaFoundationReader> r (new MediaFoundationReader (sourceStream, getFormatName(), fileStream->getFile()));

    if (r->isValid())
        return r.release();

    if (! deleteStreamIfOpeningFails)
        r->input = nullptr;   // the caller keeps the stream

    return nullptr;
}

} // namespace gocue
