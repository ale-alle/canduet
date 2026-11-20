// SPDX-License-Identifier: GPLv3-or-later
// CanDuet: MP3 recording through the MP3 encoder that ships with Windows (Media Foundation).
//
// The encoder is a plain synchronous Media Foundation transform: 16-bit PCM in, MP3 frames out.
// An MP3 file is just those frames back to back, so they go straight to JUCE's output stream
// (no sink writer and no container), which keeps the recording code path the same as FLAC/WAV.

#if JUCE_WINDOWS || defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mferror.h>
 #include <mftransform.h>
 #include <wrl/client.h>
 #pragma comment (lib, "mfplat.lib")
 #pragma comment (lib, "mfuuid.lib")
 #define TINCAN_HAS_MF_MP3 1
#else
 #define TINCAN_HAS_MF_MP3 0
#endif

#include "TinCanMp3Format.h"

#include <map>
#include <mutex>
#include <vector>

using namespace juce;

#if TINCAN_HAS_MF_MP3

using Microsoft::WRL::ComPtr;

namespace
{
    // MFStartup/MFShutdown are reference counted by Windows; each user holds one.
    struct MfSession
    {
        MfSession() : ok (SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE))) {}
        ~MfSession() { if (ok) MFShutdown(); }
        const bool ok;
    };

    // Makes the Windows MP3 encoder and sets it up for this rate and channel count.
    ComPtr<IMFTransform> makeEncoder (int sampleRate, int numChannels)
    {
        MFT_REGISTER_TYPE_INFO in  { MFMediaType_Audio, MFAudioFormat_PCM };
        MFT_REGISTER_TYPE_INFO out { MFMediaType_Audio, MFAudioFormat_MP3 };
        IMFActivate** found = nullptr;
        UINT32 count = 0;

        if (FAILED (MFTEnumEx (MFT_CATEGORY_AUDIO_ENCODER,
                               MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               &in, &out, &found, &count)))
            return {};

        ComPtr<IMFTransform> mft;
        for (UINT32 i = 0; i < count; ++i) {
            if (! mft) found[i]->ActivateObject (IID_PPV_ARGS (mft.GetAddressOf()));
            found[i]->Release();
        }
        CoTaskMemFree (found);
        if (! mft) return {};

        const UINT32 blockAlign = (UINT32) numChannels * 2;
        const UINT32 wantBps = (UINT32) TinCanMp3AudioFormat::bitrateFor (numChannels) * 1000 / 8;

        // The encoder only takes output types from its own list (they carry its MP3 wave-format
        // details), so pick the listed one with our channels, rate and bitrate.
        ComPtr<IMFMediaType> outType;
        for (DWORD k = 0; ; ++k) {
            ComPtr<IMFMediaType> t;
            if (FAILED (mft->GetOutputAvailableType (0, k, t.GetAddressOf()))) break;
            UINT32 ch = 0, rate = 0, bps = 0;
            t->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &ch);
            t->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            t->GetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
            if (ch == (UINT32) numChannels && rate == (UINT32) sampleRate && bps == wantBps) { outType = t; break; }
        }
        if (! outType || FAILED (mft->SetOutputType (0, outType.Get(), 0))) return {};

        ComPtr<IMFMediaType> inType;
        if (FAILED (MFCreateMediaType (inType.GetAddressOf()))) return {};
        inType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) numChannels);
        inType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        inType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        inType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, blockAlign * (UINT32) sampleRate);
        inType->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (FAILED (mft->SetInputType (0, inType.Get(), 0))) return {};

        mft->ProcessMessage (MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage (MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return mft;
    }

    class Mp3Writer : public AudioFormatWriter
    {
    public:
        Mp3Writer (OutputStream* out, double rate, unsigned int chans, ComPtr<IMFTransform> encoder,
                   std::unique_ptr<MfSession> sessionIn)
            : AudioFormatWriter (out, "MP3", rate, chans, 16),
              session (std::move (sessionIn)), mft (std::move (encoder))
        {
            MFT_OUTPUT_STREAM_INFO info {};
            if (SUCCEEDED (mft->GetOutputStreamInfo (0, &info))) {
                encoderMakesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                outBufferSize = jmax ((DWORD) 16384, info.cbSize);
            }
        }

        ~Mp3Writer() override
        {
            if (mft != nullptr && ! failed) {
                mft->ProcessMessage (MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                mft->ProcessMessage (MFT_MESSAGE_COMMAND_DRAIN, 0);
                pullOutput();
            }
            if (output != nullptr) output->flush();
            mft.Reset();
        }

        bool write (const int** samples, int numSamples) override
        {
            if (failed) return false;
            if (numSamples <= 0) return true;

            const int chans = (int) numChannels;
            pcm.resize ((size_t) numSamples * (size_t) chans);
            for (int c = 0; c < chans; ++c) {
                const int* src = samples[c];
                int16* dst = pcm.data() + c;
                for (int i = 0; i < numSamples; ++i, dst += chans)
                    *dst = src != nullptr ? (int16) (src[i] >> 16) : 0;   // JUCE hands over full-scale 32-bit ints
            }

            const DWORD bytes = (DWORD) (pcm.size() * sizeof (int16));
            ComPtr<IMFMediaBuffer> buf;
            ComPtr<IMFSample> sample;
            BYTE* p = nullptr;
            if (FAILED (MFCreateMemoryBuffer (bytes, buf.GetAddressOf())) || FAILED (buf->Lock (&p, nullptr, nullptr)))
                return fail();
            memcpy (p, pcm.data(), bytes);
            buf->Unlock();
            buf->SetCurrentLength (bytes);
            if (FAILED (MFCreateSample (sample.GetAddressOf())) || FAILED (sample->AddBuffer (buf.Get())))
                return fail();

            const LONGLONG t = (LONGLONG) (framesIn * 10000000.0 / sampleRate);
            framesIn += numSamples;
            sample->SetSampleTime (t);
            sample->SetSampleDuration ((LONGLONG) (framesIn * 10000000.0 / sampleRate) - t);

            HRESULT hr = mft->ProcessInput (0, sample.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                if (! pullOutput()) return false;
                hr = mft->ProcessInput (0, sample.Get(), 0);
            }
            if (FAILED (hr)) return fail();
            return pullOutput();
        }

    private:
        bool fail() { failed = true; return false; }

        // Takes every MP3 frame the encoder has ready and appends it to the file.
        bool pullOutput()
        {
            for (;;) {
                MFT_OUTPUT_DATA_BUFFER od {};
                ComPtr<IMFSample> own;
                if (! encoderMakesSamples) {
                    ComPtr<IMFMediaBuffer> ob;
                    if (FAILED (MFCreateMemoryBuffer (outBufferSize, ob.GetAddressOf()))
                        || FAILED (MFCreateSample (own.GetAddressOf())) || FAILED (own->AddBuffer (ob.Get())))
                        return fail();
                    od.pSample = own.Get();
                }

                DWORD status = 0;
                const HRESULT hr = mft->ProcessOutput (0, 1, &od, &status);
                if (od.pEvents != nullptr) od.pEvents->Release();

                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    if (encoderMakesSamples && od.pSample != nullptr) od.pSample->Release();
                    return true;
                }
                if (FAILED (hr)) {
                    if (encoderMakesSamples && od.pSample != nullptr) od.pSample->Release();
                    return fail();
                }

                if (od.pSample != nullptr) {
                    ComPtr<IMFMediaBuffer> flat;
                    if (SUCCEEDED (od.pSample->ConvertToContiguousBuffer (flat.GetAddressOf()))) {
                        BYTE* p = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED (flat->Lock (&p, nullptr, &len))) {
                            if (len > 0 && ! output->write (p, (size_t) len)) failed = true;
                            flat->Unlock();
                        }
                    }
                    if (encoderMakesSamples) od.pSample->Release();
                }
                if (failed) return false;
            }
        }

        std::unique_ptr<MfSession> session;   // keeps Media Foundation up while the encoder lives
        ComPtr<IMFTransform> mft;
        bool encoderMakesSamples = false, failed = false;
        DWORD outBufferSize = 16384;
        int64 framesIn = 0;
        std::vector<int16> pcm;
    };
}

TinCanMp3AudioFormat::TinCanMp3AudioFormat() : AudioFormat ("MP3 (Windows encoder)", ".mp3") {}
TinCanMp3AudioFormat::~TinCanMp3AudioFormat() = default;

bool TinCanMp3AudioFormat::canEncode (double sampleRate, int numChannels)
{
    const int rate = roundToInt (sampleRate);
    if (numChannels < 1 || numChannels > 2 || (rate != 32000 && rate != 44100 && rate != 48000))
        return false;

    static std::mutex lock;
    static std::map<int, bool> known;   // the answer for each rate/channel pair doesn't change while running
    const std::lock_guard<std::mutex> g (lock);
    const int key = rate * 4 + numChannels;
    if (auto it = known.find (key); it != known.end()) return it->second;

    MfSession s;
    const bool ok = s.ok && makeEncoder (rate, numChannels) != nullptr;
    known[key] = ok;
    return ok;
}

AudioFormatWriter* TinCanMp3AudioFormat::createWriterFor (OutputStream* out, double sampleRate, unsigned int numberOfChannels,
                                                          int, const StringPairArray&, int)
{
    if (out == nullptr || ! canEncode (sampleRate, (int) numberOfChannels)) return nullptr;

    auto session = std::make_unique<MfSession>();
    if (! session->ok) return nullptr;
    auto mft = makeEncoder (roundToInt (sampleRate), (int) numberOfChannels);
    if (! mft) return nullptr;
    return new Mp3Writer (out, sampleRate, numberOfChannels, std::move (mft), std::move (session));
}

#else

TinCanMp3AudioFormat::TinCanMp3AudioFormat() : AudioFormat ("MP3", ".mp3") {}
TinCanMp3AudioFormat::~TinCanMp3AudioFormat() = default;
bool TinCanMp3AudioFormat::canEncode (double, int) { return false; }
AudioFormatWriter* TinCanMp3AudioFormat::createWriterFor (OutputStream*, double, unsigned int, int, const StringPairArray&, int) { return nullptr; }

#endif
