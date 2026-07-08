#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>

//==============================================================================
// PRE-RENDER mode: the future is known (file + FX chain), so we trade GPU/CPU
// throughput for latency. A background thread renders the whole file through
// the FX chain into a RAM cache; the audio thread only streams the cache.
//==============================================================================
struct RenderCache
{
    juce::AudioBuffer<float>  data;          // stereo, allocated on the message thread
    std::atomic<juce::int64>  valid { 0 };   // samples rendered so far (initial pass only)
    std::atomic<bool>         primed { false }; // whole buffer has been rendered at least once
    juce::int64               length = 0;    // total samples when complete
    double                    sampleRate = 48000.0;
};

//==============================================================================
// Streams the cache. Reads never pass `valid`: if playback catches up with the
// render cursor it stalls on silence (like video buffering) instead of
// glitching or skipping content.
//==============================================================================
class CacheAudioSource : public juce::PositionableAudioSource
{
public:
    explicit CacheAudioSource (RenderCache& c) : cache (c) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        // Once primed, every sample is playable (possibly stale while a
        // cursor-first re-render is sweeping through) - never stall again.
        const auto valid = cache.primed.load (std::memory_order_acquire)
                             ? cache.length
                             : juce::jmin (cache.valid.load (std::memory_order_acquire), cache.length);
        auto p = pos.load();
        int written = 0;

        while (written < info.numSamples)
        {
            if (looping && cache.length > 0 && p >= cache.length)
                p = 0;

            const auto avail = valid - p;
            if (avail <= 0)
                break;   // stall (not yet rendered) or end of file

            const int n = (int) juce::jmin (avail, (juce::int64) (info.numSamples - written));
            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->copyFrom (ch, info.startSample + written, cache.data,
                                       juce::jmin (ch, cache.data.getNumChannels() - 1),
                                       (int) p, n);
            p += n;
            written += n;

            if (! looping && p >= cache.length)
                break;
        }
        pos.store (p);
    }

    void setNextReadPosition (juce::int64 newPos) override
    {
        pos = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, cache.length), newPos);
    }
    juce::int64 getNextReadPosition() const override  { return pos.load(); }
    juce::int64 getTotalLength() const override       { return cache.length; }
    bool isLooping() const override                   { return looping; }
    void setLooping (bool b) override                 { looping = b; }

private:
    RenderCache& cache;
    std::atomic<juce::int64> pos { 0 };
    bool looping = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CacheAudioSource)
};

//==============================================================================
// Background renderer. Owns nothing: the caller pre-allocates the cache on the
// message thread (so no realloc races the audio thread) and passes an OFFLINE
// plugin instance that nobody else touches while the thread runs.
//==============================================================================
class RenderAheadEngine : private juce::Thread
{
public:
    RenderAheadEngine() : juce::Thread ("render-ahead") {}
    ~RenderAheadEngine() override { stopRender(); }

    // fx may be nullptr (dry copy). Cache must be sized already.
    // startSample <= 0 (or an unprimed cache) renders the whole file from 0.
    // Otherwise: cursor-first render - warm the plugin on ~2s of preroll,
    // write [cursor..end), then wrap around and refresh [0..cursor).
    // quickWindowSamples > 0: "cheat" mode - only refresh [cursor..cursor+window)
    // and stop; the full wrap-around cleanup is deferred until knobs go quiet.
    void startRender (const juce::File& fileToRender, juce::AudioFormatManager& formats,
                      juce::AudioPluginInstance* offlineFx, RenderCache& cacheToFill,
                      juce::int64 startSample = 0, juce::int64 quickWindowSamples = 0)
    {
        stopRender();
        file   = fileToRender;
        fm     = &formats;
        fx     = offlineFx;
        cache  = &cacheToFill;
        cursor = cache->primed.load() ? juce::jmax ((juce::int64) 0, startSample) : 0;
        quickWindow = cursor > 0 ? juce::jmax ((juce::int64) 0, quickWindowSamples) : 0;
        if (! cache->primed.load())
            cache->valid.store (0);
        fromCursor = cursor > 0;
        quickMode  = quickWindow > 0;
        progress = 0.0f; speedX = 0.0f;
        finished = false; failed = false;
        // Low priority: use idle cores freely but never starve the ASIO
        // callback (xruns can leave the driver misbehaving until a reset).
        startThread (juce::Thread::Priority::low);
    }

    void stopRender()            { stopThread (5000); }
    bool isRendering() const     { return isThreadRunning(); }

    std::atomic<float> progress { 0.0f };   // 0..1
    std::atomic<float> speedX   { 0.0f };   // rendered seconds / elapsed seconds
    std::atomic<bool>  finished { false };
    std::atomic<bool>  failed   { false };
    std::atomic<bool>  fromCursor { false };
    std::atomic<bool>  quickMode  { false };

private:
    // Renders output samples [writeStart..writeEnd) into the cache, reading
    // input from inStart onwards. Output before writeStart (preroll warm-up)
    // and the plugin's latency are processed but discarded.
    bool renderSegment (juce::AudioFormatReader& reader, juce::AudioBuffer<float>& buf,
                        juce::int64 inStart, juce::int64 writeStart, juce::int64 writeEnd,
                        juce::int64 fileLen, int latency,
                        juce::int64& workDone, juce::int64 totalWork, double t0)
    {
        const int block = buf.getNumSamples();
        const double rate = cache->sampleRate;
        juce::MidiBuffer midi;

        juce::int64 readPos  = fx != nullptr ? inStart : writeStart;
        juce::int64 written  = writeStart;
        juce::int64 discard  = fx != nullptr ? (writeStart - readPos) + latency : 0;

        while (written < writeEnd && ! threadShouldExit())
        {
            buf.clear();
            const int toRead = (int) juce::jmin ((juce::int64) block,
                                                 juce::jmax ((juce::int64) 0, fileLen - readPos));
            if (toRead > 0)
            {
                reader.read (&buf, 0, toRead, readPos, true, true);
                readPos += toRead;
            }
            else if (fx == nullptr)
                break;   // dry copy: nothing left to read

            int produced = block, start = 0;
            if (fx != nullptr)
            {
                midi.clear();
                fx->processBlock (buf, midi);   // keeps running on silence to flush latency
                if (discard > 0)
                {
                    const int d = (int) juce::jmin (discard, (juce::int64) produced);
                    start = d; produced -= d; discard -= d;
                }
            }
            else
                produced = toRead;

            const int w = (int) juce::jmin ((juce::int64) produced, writeEnd - written);
            if (w > 0)
            {
                for (int ch = 0; ch < 2; ++ch)
                    cache->data.copyFrom (ch, (int) written, buf,
                                          juce::jmin (ch, buf.getNumChannels() - 1), start, w);
                written += w;
                if (! cache->primed.load())
                    cache->valid.store (written, std::memory_order_release);
                workDone += w;
            }

            progress.store (totalWork > 0 ? (float) workDone / (float) totalWork : 0.0f);
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            if (elapsed > 0.01)
                speedX.store ((float) ((double) workDone / rate / elapsed));
        }

        return written >= writeEnd;
    }

    void run() override
    {
        std::unique_ptr<juce::AudioFormatReader> reader (fm->createReaderFor (file));
        if (reader == nullptr || cache->length <= 0)
        {
            failed = true;
            return;
        }

        const double rate  = cache->sampleRate;
        const int    block = 4096;
        const juce::int64 total = juce::jmin (cache->length, (juce::int64) reader->lengthInSamples);

        int latency = 0;
        if (fx != nullptr)
        {
            fx->setNonRealtime (true);
            fx->setPlayConfigDetails (2, 2, rate, block);
            fx->prepareToPlay (rate, block);
            latency = juce::jmax (0, fx->getLatencySamples());
        }

        const int chans = fx != nullptr
            ? juce::jmax (2, fx->getTotalNumInputChannels(), fx->getTotalNumOutputChannels())
            : 2;
        juce::AudioBuffer<float> buf (chans, block);

        juce::int64 workDone = 0;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        bool ok = false;

        const juce::int64 P = juce::jlimit ((juce::int64) 0, total, cursor);
        if (P <= 0)
        {
            // Full render from the top (initial pass, or cursor at 0).
            ok = renderSegment (*reader, buf, 0, 0, total, total, latency, workDone, total, t0);
            if (ok && ! threadShouldExit())
                cache->primed.store (true, std::memory_order_release);
        }
        else
        {
            // Pass A: warm the plugin just before the cursor, then write
            // [cursor..end) so the listener hears the new sound within ~a block.
            // Quick mode only refreshes a short window ahead of the cursor.
            const juce::int64 warm = juce::jmin ((juce::int64) (2.0 * rate), P);
            const juce::int64 end  = quickWindow > 0 ? juce::jmin (total, P + quickWindow) : total;
            const juce::int64 work = quickWindow > 0 ? end - P : total;
            ok = renderSegment (*reader, buf, P - warm, P, end, total, latency, workDone, work, t0);

            // Pass B: wrap around - fresh plugin state, refresh [0..cursor).
            // Skipped in quick mode; the deferred full render cleans up later.
            if (ok && quickWindow == 0 && ! threadShouldExit())
            {
                if (fx != nullptr)
                {
                    fx->releaseResources();
                    fx->prepareToPlay (rate, block);
                }
                ok = renderSegment (*reader, buf, 0, 0, P, total, latency, workDone, total, t0);
            }
        }

        if (fx != nullptr)
            fx->releaseResources();

        if (! threadShouldExit())
        {
            finished = ok;
            failed   = ! ok;
        }
    }

    juce::File file;
    juce::AudioFormatManager* fm = nullptr;
    juce::AudioPluginInstance* fx = nullptr;
    RenderCache* cache = nullptr;
    juce::int64 cursor = 0;
    juce::int64 quickWindow = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RenderAheadEngine)
};
