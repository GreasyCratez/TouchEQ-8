// TouchEQ8.cpp - TouchEQ 8: an 8-band parametric EQ for standalone MPCs with a touch graph.
//
// SPDX-License-Identifier: MIT
//
// The DSP lives in common/toucheq_engine.h (shared with tests/dsp_test.cpp). This file declares the parameters,
// feeds the engine, and drives the screen.
//
// How the graph works on the MPC: plugin skins cannot draw code-generated graphics, so the plugin publishes what
// the screen should show as read-only parameters that the skin draws:
//   * Curve 001..192: the EQ's magnitude response at 192 log-spaced frequencies, 0..1 = -15..+15 dB. Each is a thin
//     vertical slider column in the skin, filled from the 0 dB line, which together draw the curve.
//   * Spectrum 01..96: the analyser (post EQ), 0..1 = -90..0 dBFS with a 4.5 dB/octave tilt.
// These are not automatable and are published up to ~30 times a second. The MPC does not poll parameter values, so
// each column that moves is announced with audioMasterAutomate (the only way to make the screen redraw it).
// All of that (curve maths, the analyser FFT, the announcements) runs on a separate screen thread: the audio
// thread only runs the EQ and copies its output into a lock-free tap, so the EQ never waits on screen work. The
// announcements themselves cost the MPC work, so they are budgeted per tick (publishColumns).
// The band points are the skin's XY pads bound straight to each band's Freq (x) and Gain (y; Q for cut and notch
// types), so dragging a point edits the real parameters.
//
// PARAMETER ORDER IS FROZEN (saved projects and the skin depend on it). Only ever append.
//   0 Output  1 Scale  2 Mix  3 Band (which point the finger drags)  4 Adapt Q  5 Analyzer
//   6 + 5 b + {0 On, 1 Type, 2 Freq, 3 Gain, 4 Q}   for bands b = 0..7      (6 .. 45)
//   46 .. 237   Curve 001 .. 192
//   238 .. 333  Spectrum 01 .. 96
//   334         QLink Follow (0.1.2): tapping a band number also banks the Q-Links to that band's row
//   335 .. 342  Pick 1 .. 8 (0.1.2): momentary triggers the band-number buttons toggle; the plugin then sets
//               Band and resets the trigger (the skin has no "set to value" action, only Toggle Switch)

#include "mpc_plugin.h"
#include "../../../common/toucheq_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TouchEQ8 : public mpc::Plugin
{
public:
    enum Params { kOutput, kScale, kMix, kEdit, kAdaptQ, kAnalyzer, kBand0 };
    enum BandParam { kOn, kType, kFreq, kGain, kQ, kPerBand };
    static constexpr int kCurve0 = kBand0 + teq::kBands * kPerBand;   // 46
    static constexpr int kSpec0 = kCurve0 + teq::kCurveCols;          // 238
    static constexpr int kFollow = kSpec0 + teq::kSpecCols;            // 334
    static constexpr int kPick0 = kFollow + 1;                         // 335
    static constexpr int kNumParams = kPick0 + teq::kBands;            // 343
    static int bp(int band, int which) { return kBand0 + band * kPerBand + which; }

    TouchEQ8() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("out", "Output", -24.0f, 24.0f, 0.0f, "dB", 1));
        addParam(floatParam("scale", "Scale", 0.0f, 200.0f, 100.0f, "%", 0));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(choiceParam("edit", "Band", { "1", "2", "3", "4", "5", "6", "7", "8" }, 2));
        addParam(boolParam("adaptq", "Adapt Q", false));
        addParam(boolParam("analyzer", "Analyzer", true));

        std::vector<std::string> types;
        for (int t = 0; t < teq::kNumTypes; ++t) types.push_back(teq::typeName(t));
        for (int b = 0; b < teq::kBands; ++b)
        {
            char id[32], name[32];
            auto add = [&](Param p) { addParam(p); };
            std::snprintf(id, sizeof id, "b%d_on", b + 1);   std::snprintf(name, sizeof name, "On %d", b + 1);
            add(boolParam(id, name, kDefOn[b]));
            std::snprintf(id, sizeof id, "b%d_type", b + 1); std::snprintf(name, sizeof name, "Type %d", b + 1);
            add(choiceParam(id, name, types, kDefType[b]));
            std::snprintf(id, sizeof id, "b%d_freq", b + 1); std::snprintf(name, sizeof name, "Freq %d", b + 1);
            add(logParam(id, name, teq::kFMin, teq::kFMax, kDefFreq[b], "Hz", 0));
            std::snprintf(id, sizeof id, "b%d_gain", b + 1); std::snprintf(name, sizeof name, "Gain %d", b + 1);
            add(floatParam(id, name, -teq::kGainRange, teq::kGainRange, 0.0f, "dB", 1));
            // Q is stored as the 0..1 knob position; formatValue shows the real Q (0.10 .. 18, 0.71 at the centre).
            std::snprintf(id, sizeof id, "b%d_q", b + 1);    std::snprintf(name, sizeof name, "Q %d", b + 1);
            add(floatParam(id, name, 0.0f, 1.0f, 0.5f, "", 3));
        }
        for (int i = 0; i < teq::kCurveCols; ++i)
        {
            char id[16], name[24];
            std::snprintf(id, sizeof id, "c%03d", i + 1); std::snprintf(name, sizeof name, "Curve %03d", i + 1);
            Param p = floatParam(id, name, 0.0f, 1.0f, 0.5f, "", 2); p.automatable = false; addParam(p);
        }
        for (int i = 0; i < teq::kSpecCols; ++i)
        {
            char id[16], name[24];
            std::snprintf(id, sizeof id, "s%02d", i + 1); std::snprintf(name, sizeof name, "Spectrum %02d", i + 1);
            Param p = floatParam(id, name, 0.0f, 1.0f, 0.0f, "", 2); p.automatable = false; addParam(p);
        }
        // Appended in 0.1.2 (never reorder anything above).
        addParam(boolParam("qlfollow", "QLink Follow", true));
        for (int b = 0; b < teq::kBands; ++b)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof id, "pick%d", b + 1); std::snprintf(name, sizeof name, "Pick %d", b + 1);
            Param p = boolParam(id, name, false); p.automatable = false; addParam(p);
        }
        if (numParams() != kNumParams) mpc::log("TouchEQ8: parameter count mismatch");
        addPrograms();
        std::fill(std::begin(lastKey_), std::end(lastKey_), -1.0f);
        std::fill(std::begin(written_), std::end(written_), -1.0f);
        std::fill(std::begin(shown_), std::end(shown_), -1.0f);
        screen_ = std::thread([this] { screenLoop(); });
    }

    ~TouchEQ8() override
    {
        {
            std::lock_guard<std::mutex> lk(screenMutex_);
            stop_.store(true);
        }
        screenWake_.notify_all();
        if (screen_.joinable()) screen_.join();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        engine_.prepare((float)sampleRate);
        sampleRate_.store((float)sampleRate, std::memory_order_release);   // the screen thread follows
    }

    void reset() override
    {
        pushParams();
        engine_.reset();
        resync_.store(true, std::memory_order_release);   // screen thread: rebuild and resend every column
    }

    // The audio thread only runs the EQ (and copies its output into the analyser tap). Everything for the screen
    // happens on screenLoop()'s thread, so no screen work can make the audio late.
    void process(mpc::ProcessContext& ctx) override
    {
        pushParams();
        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        engine_.process(inL, inR ? inR : inL, outL, outR, ctx.numFrames);
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char buf[32];
        if ((index >= kCurve0 && index < kFollow) || index >= kPick0) { out.clear(); return true; }
        if (index == kFollow) { out = v >= 0.5f ? "On" : "Off"; return true; }
        if (index == kOutput) { std::snprintf(buf, sizeof buf, "%+.1f dB", (double)v); out = fixZero(buf); return true; }
        if (index == kScale || index == kMix) { std::snprintf(buf, sizeof buf, "%.0f %%", (double)v); out = buf; return true; }
        if (index >= kBand0)
        {
            const int which = (index - kBand0) % kPerBand;
            const int band = (index - kBand0) / kPerBand;
            if (which == kFreq)
            {
                if (v < 100.0f) std::snprintf(buf, sizeof buf, "%.1f Hz", (double)v);
                else if (v < 1000.0f) std::snprintf(buf, sizeof buf, "%.0f Hz", (double)v);
                else if (v < 10000.0f) std::snprintf(buf, sizeof buf, "%.2f kHz", (double)v * 0.001);
                else std::snprintf(buf, sizeof buf, "%.1f kHz", (double)v * 0.001);
                out = buf; return true;
            }
            if (which == kGain)
            {
                if (!teq::typeUsesGain(getInt(bp(band, kType)))) { out = "--"; return true; }
                std::snprintf(buf, sizeof buf, "%+.1f dB", (double)v); out = fixZero(buf); return true;
            }
            if (which == kQ)
            {
                const float q = teq::qFromNorm(v);
                std::snprintf(buf, sizeof buf, q < 10.0f ? "%.2f" : "%.1f", (double)q); out = buf; return true;
            }
        }
        return false;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "TouchEQ 8";
        i.vendor = "GreasyCratez";
        i.uniqueId = MPC_FOURCC('T', 'E', 'q', '8');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 1;
        return i;
    }

    static std::string fixZero(const char* s)
    {
        std::string r = s;
        if (r.rfind("-0.0", 0) == 0 || r.rfind("+0.0", 0) == 0) r = "0.0" + r.substr(4);
        return r;
    }

    // Host notifications are what the MPC redraws from, but each one costs the MPC work, and on the Key 61 too many
    // of them (up to ~7700 a second in 0.1.2 while dragging with music playing) caused audio dropouts. So per tick
    // (30 a second) only the columns that moved the most are announced, at most a budget of them; the rest follow on
    // later ticks. Worst case now (64 + 16) x 30 = 2400 a second, far less in normal use.
    // Steps: the plot is 264 px tall, so 0.004 is one pixel of curve; 0.012 is about 1 dB of analyser.
    static constexpr float kCurveStep = 0.004f, kSpecStep = 0.012f;
    static constexpr int kCurveBudget = 64, kSpecBudget = 16;

    // Writes display columns first..first+n-1 (values norm[]) and announces the ones whose value moved by at least
    // `step` since the host was last told, largest moves first, at most `budget` of them. A column something else
    // wrote since we did (a stray touch, a project load) is always corrected and announced.
    void publishColumns(int first, const float* norm, int n, float step, int budget)
    {
        int cand[teq::kCurveCols];
        float move[teq::kCurveCols];
        int nc = 0;
        for (int i = 0; i < n; ++i)
        {
            const int idx = first + i, d = idx - kCurve0;
            const float v = norm[i] < 0.0f ? 0.0f : (norm[i] > 1.0f ? 1.0f : norm[i]);
            const float cur = getNormalised(idx);
            const bool foreign = cur != written_[d];
            if (cur != v) setNormalised(idx, v);
            written_[d] = v;
            if (foreign) { shown_[d] = v; notifyHostParameterChanged(idx); continue; }
            const float m = std::fabs(v - shown_[d]);
            if (m >= step) { cand[nc] = i; move[i] = m; ++nc; }
        }
        if (nc > budget)
            std::nth_element(cand, cand + budget, cand + nc, [&](int a, int b) { return move[a] > move[b]; });
        for (int k = 0; k < std::min(nc, budget); ++k)
        {
            const int i = cand[k], idx = first + i;
            shown_[idx - kCurve0] = written_[idx - kCurve0];
            notifyHostParameterChanged(idx);
        }
    }

    teq::BandParams readBand(int b) const
    {
        teq::BandParams p;
        p.on = getBool(bp(b, kOn));
        p.type = getInt(bp(b, kType));
        p.freq = get(bp(b, kFreq));
        p.gain = get(bp(b, kGain));
        p.q = teq::qFromNorm(get(bp(b, kQ)));
        return p;
    }

    void pushParams()
    {
        engine_.setGlobal(get(kOutput), get(kScale) * 0.01f, get(kMix), getBool(kAdaptQ), getBool(kAnalyzer));
        for (int b = 0; b < teq::kBands; ++b) engine_.setBand(b, readBand(b));
    }

    // ----- screen thread ---------------------------------------------------------------------------------------
    // ~30 times a second: rebuild the curve if a user parameter changed, analyse the newest audio from the tap,
    // and announce the display columns that moved to the host (within a budget). Never touches the EQ.
    // Normal priority on purpose: the host may take its own locks while handling a notification, and a thread that
    // holds one should not be starved by everything else (0.1.2 ran this at nice 10).
    void screenLoop()
    {
        std::unique_lock<std::mutex> lk(screenMutex_);
        while (!stop_.load())
        {
            screenWake_.wait_for(lk, std::chrono::milliseconds(33), [this] { return stop_.load(); });
            if (stop_.load()) break;
            screenTick();
        }
    }

    void screenTick()
    {
        const float fs = sampleRate_.load(std::memory_order_acquire);
        if (fs <= 0.0f) return;   // not prepared yet
        if (fs != screenFs_)
        {
            screenFs_ = fs;
            curveModel_.prepare(fs);
            analyzer_.prepare(fs);
            resync_.store(true);
        }
        if (resync_.exchange(false))
        {
            std::fill(std::begin(lastKey_), std::end(lastKey_), -1.0f);
            std::fill(std::begin(written_), std::end(written_), -1.0f);
            std::fill(std::begin(shown_), std::end(shown_), -1.0f);
            analyzer_.reset();
            lastTap_ = engine_.tap().written();
        }

        // Band-number taps: the skin toggles Pick b; select band b and reset the trigger (both announced, so the
        // screen and the host agree). Doing this here keeps it off the audio thread and out of host callbacks.
        for (int b = 0; b < teq::kBands; ++b)
        {
            if (getNormalised(kPick0 + b) < 0.5f) continue;
            setNormalised(kPick0 + b, 0.0f);
            notifyHostParameterChanged(kPick0 + b);
            set(kEdit, (float)b);
            notifyHostParameterChanged(kEdit);
        }

        // Curve: only rebuilt when a user parameter moved; the columns are re-checked every tick (stray touches).
        bool changed = false;
        for (int i = 0; i < kCurve0; ++i)
        {
            const float v = getNormalised(i);
            if (v != lastKey_[i]) { lastKey_[i] = v; changed = true; }
        }
        if (changed)
        {
            teq::BandParams bands[teq::kBands];
            for (int b = 0; b < teq::kBands; ++b) bands[b] = readBand(b);
            curveModel_.compute(bands, get(kScale) * 0.01f, getBool(kAdaptQ), curve_);
        }
        float col[teq::kCurveCols];
        for (int i = 0; i < teq::kCurveCols; ++i) col[i] = 0.5f + curve_[i] / (2.0f * teq::kGainRange);
        publishColumns(kCurve0, col, teq::kCurveCols, kCurveStep, kCurveBudget);

        // Analyser: a new frame at most 15 times a second, once enough new audio has arrived.
        if (getBool(kAnalyzer))
        {
            const uint32_t w = engine_.tap().written();
            const uint32_t fresh = w - lastTap_;
            if (fresh >= std::max((uint32_t)teq::kFftSize, (uint32_t)(screenFs_ / 15.0f)))
            {
                engine_.tap().copyLatest(w, frame_);
                analyzer_.analyse(frame_, fresh);
                lastTap_ = w;
            }
            const float* d = analyzer_.db();
            for (int i = 0; i < teq::kSpecCols; ++i) col[i] = (d[i] - teq::kSpecFloorDb) / -teq::kSpecFloorDb;
        }
        else
        {
            analyzer_.reset();
            lastTap_ = engine_.tap().written();
            for (int i = 0; i < teq::kSpecCols; ++i) col[i] = 0.0f;
        }
        publishColumns(kSpec0, col, teq::kSpecCols, kSpecStep, kSpecBudget);
    }

    void addPrograms()
    {
        std::vector<float> def((size_t)numParams());
        for (int i = 0; i < numParams(); ++i) def[(size_t)i] = param(i).def;
        struct B { int band; int on; int type; float freq; float gain; float q; };   // q as a real Q; on/type -1 = keep
        auto prog = [&](const char* name, std::initializer_list<B> bands, float scale = 100.0f) {
            std::vector<float> v = def;
            v[(size_t)kScale] = scale;
            for (const B& b : bands)
            {
                if (b.on >= 0) v[(size_t)bp(b.band - 1, kOn)] = (float)b.on;
                if (b.type >= 0) v[(size_t)bp(b.band - 1, kType)] = (float)b.type;
                v[(size_t)bp(b.band - 1, kFreq)] = b.freq;
                v[(size_t)bp(b.band - 1, kGain)] = b.gain;
                v[(size_t)bp(b.band - 1, kQ)] = teq::normFromQ(b.q);
            }
            addProgram(name, v);
        };
        using namespace teq;
        prog("Init (Flat)", {});
        prog("Vocal Presence", { { 1, 1, kLoCut12, 90, 0, 0.71f }, { 3, 1, kBell, 300, -2.5f, 1.2f },
                                 { 5, 1, kBell, 3200, 3.0f, 0.9f }, { 6, 1, kBell, 6800, -2.0f, 4.0f },
                                 { 7, 1, kHiShelf, 11000, 3.0f, 0.71f } });
        prog("Kick Punch", { { 1, 1, kLoCut48, 28, 0, 0.71f }, { 2, 1, kBell, 60, 4.0f, 1.4f },
                             { 4, 1, kBell, 350, -5.0f, 1.8f }, { 6, 1, kBell, 3500, 3.5f, 1.2f } });
        prog("Clean Bass", { { 1, 1, kLoCut48, 35, 0, 0.71f }, { 2, 1, kLoShelf, 90, 2.0f, 0.71f },
                             { 4, 1, kBell, 250, -3.0f, 1.0f }, { 8, 1, kHiCut12, 6000, 0, 0.71f } });
        prog("Air Lift", { { 7, 1, kHiShelf, 9000, 4.0f, 0.6f }, { 5, 1, kBell, 2500, -1.5f, 0.8f } });
        prog("Lo-Fi Phone", { { 1, 1, kLoCut48, 400, 0, 1.2f }, { 8, 1, kHiCut48, 3200, 0, 1.4f },
                              { 5, 1, kBell, 1500, 6.0f, 1.0f } });
        prog("Mix Smile", { { 2, 1, kLoShelf, 110, 2.5f, 0.71f }, { 4, 1, kBell, 800, -1.5f, 0.5f },
                            { 7, 1, kHiShelf, 8000, 2.5f, 0.71f } });
        prog("Hat Tamer", { { 1, 1, kLoCut48, 300, 0, 0.71f }, { 6, 1, kNotch, 7500, 0, 6.0f },
                            { 7, 1, kHiShelf, 12000, -3.0f, 0.71f } });
    }

    // Defaults: everything flat. Bands 2-7 are on (a 0 dB bell/shelf passes the signal untouched), the cut
    // filters on 1 and 8 start off, as in most desktop EQs.
    static constexpr bool kDefOn[teq::kBands] = { false, true, true, true, true, true, true, false };
    static constexpr int kDefType[teq::kBands] = { teq::kLoCut48, teq::kLoShelf, teq::kBell, teq::kBell,
                                                   teq::kBell, teq::kBell, teq::kHiShelf, teq::kHiCut48 };
    static constexpr float kDefFreq[teq::kBands] = { 30, 90, 250, 600, 1500, 3500, 8000, 18000 };

    teq::Engine engine_;
    // Screen thread (everything below is touched only by it, apart from the atomics).
    std::thread screen_;
    std::mutex screenMutex_;
    std::condition_variable screenWake_;
    std::atomic<bool> stop_{ false }, resync_{ true };
    std::atomic<float> sampleRate_{ 0.0f };
    float screenFs_ = 0.0f;
    teq::CurveModel curveModel_;
    teq::Analyzer analyzer_;
    float frame_[teq::kFftSize] = {};
    uint32_t lastTap_ = 0;
    float lastKey_[kCurve0];
    float curve_[teq::kCurveCols] = {};
    // Per display column: the value we last wrote, and the value the host was last told about (-1 = never).
    float written_[teq::kCurveCols + teq::kSpecCols];
    float shown_[teq::kCurveCols + teq::kSpecCols];
};

MPC_PLUGIN_ENTRY(TouchEQ8)
