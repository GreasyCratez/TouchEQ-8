// End-to-end check of the REAL plugin library, driven through the VST2 interface the way the MPC drives it:
// the screen's curve and analyser columns are parameters the plugin writes, so this proves that touching a point
// (setParameter on Freq/Gain) changes the published curve, that the curve matches the audio, and that the analyser
// columns follow the input. Also checks display texts, that a stray write to a column is corrected, and that every
// column that moves (and only those) is announced to the host, which is what makes the MPC redraw it, from the
// plugin's screen thread and never from the audio thread.
//   g++ -std=c++17 -O2 -I<sdk>/framework -Icommon tests/plugin_e2e.cpp -ldl -o plugin_e2e && ./plugin_e2e <toucheq-8.so>
#include "mpc_vst2_abi.h"
#include "toucheq_engine.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Records every audioMasterAutomate (the plugin telling the host a parameter moved): the MPC redraws a control only
// when it is told, so the display columns must be announced, and only when they actually move.
// They come from the plugin's screen thread, never from the thread that runs processReplacing (the audio thread).
static std::atomic<int> automates[400];
static std::atomic<int> automatesOnAudioThread{ 0 };
static std::thread::id audioThread;
static void clearAutomates() { for (auto& a : automates) a.store(0); }
static int countAutomates(int from, int to) { int n = 0; for (int i = from; i < to; ++i) n += automates[i].load(); return n; }
static VstIntPtr host(AEffect*, VstInt32 op, VstInt32 index, VstIntPtr, void*, float)
{
    if (op == 0 && index >= 0 && index < 400)
    {
        ++automates[index];
        if (std::this_thread::get_id() == audioThread) ++automatesOnAudioThread;
    }
    return op == 1 ? 2400 : 0;
}
// The screen thread ticks ~30 times a second: give it a few ticks to catch up.
static void settle(int ms = 200) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static int findParam(AEffect* e, const char* name)
{
    char buf[256];
    for (int i = 0; i < e->numParams; ++i)
    {
        std::memset(buf, 0, sizeof buf);
        e->dispatcher(e, effGetParamName, i, 0, buf, 0);
        if (std::strcmp(buf, name) == 0) return i;
    }
    std::printf("no parameter %s\n", name);
    return -1;
}
static std::string display(AEffect* e, int i)
{
    char buf[256] = {};
    e->dispatcher(e, effGetParamDisplay, i, 0, buf, 0);
    return buf;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: plugin_e2e <toucheq-8.so>\n"); return 2; }
    audioThread = std::this_thread::get_id();
    clearAutomates();
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("dlopen: %s\n", dlerror()); return 2; }
    AEffect* e = ((AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain"))(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    e->dispatcher(e, effStartProcess, 0, 0, nullptr, 0);
    CHECK(e->numParams == 343, "numParams %d", e->numParams);

    const int curve0 = findParam(e, "Curve 001"), spec0 = findParam(e, "Spectrum 01");
    const int f3 = findParam(e, "Freq 3"), g3 = findParam(e, "Gain 3"), q3 = findParam(e, "Q 3"), t1 = findParam(e, "Type 1");
    CHECK(curve0 == 46 && spec0 == 238 && f3 == 18 && g3 == 19, "layout moved: curve %d spec %d freq3 %d gain3 %d", curve0, spec0, f3, g3);
    CHECK(e->dispatcher(e, effCanBeAutomated, curve0, 0, nullptr, 0) == 0, "curve columns must not be automatable");
    CHECK(e->dispatcher(e, effCanBeAutomated, g3, 0, nullptr, 0) == 1, "Gain 3 must be automatable");

    const int B = 128;
    float l[B], r[B];
    float* io[2] = { l, r };
    auto run = [&](int blocks, float sineHz, float amp) {
        static long n = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < B; ++i, ++n) l[i] = r[i] = amp * std::sin(2.0 * M_PI * sineHz * (double)n / 44100.0);
            e->processReplacing(e, io, io, B);
        }
    };

    // Flat by default: every curve column sits on the 0 dB line.
    run(20, 1000, 0.0f);
    settle();
    float worst = 0;
    for (int i = 0; i < teq::kCurveCols; ++i) worst = std::fmax(worst, std::fabs(e->getParameter(e, curve0 + i) - 0.5f));
    CHECK(worst < 1e-4f, "default curve not flat (%g)", worst);
    // The host was told about every column once (so the screen starts in sync), and never about a user parameter.
    int unsent = 0;
    for (int i = curve0; i < spec0 + teq::kSpecCols; ++i) unsent += automates[i] == 0;
    CHECK(unsent == 0, "%d display columns never announced to the host", unsent);
    CHECK(countAutomates(0, curve0) == 0, "plugin announced user parameters %d times", countAutomates(0, curve0));
    // Nothing changes, nothing is sent (no message flood while idle).
    clearAutomates();
    run(120, 1000, 0.0f);
    settle(400);
    CHECK(countAutomates(0, 400) == 0, "idle plugin sent %d host notifications", countAutomates(0, 400));

    // "Drag" band 3 (a Bell) to 1 kHz, +12 dB, Q 2: x = log position, y = gain position, like the XY pad does.
    const float x = std::log(1000.0f / 20.0f) / std::log(1000.0f), y = (12.0f + 15.0f) / 30.0f;
    e->setParameter(e, f3, x);
    e->setParameter(e, g3, y);
    e->setParameter(e, q3, (float)teq::normFromQ(2.0f));
    clearAutomates();
    run(12, 1000, 0.0f);
    settle();
    int peakCol = 0;
    for (int i = 1; i < teq::kCurveCols; ++i) if (e->getParameter(e, curve0 + i) > e->getParameter(e, curve0 + peakCol)) peakCol = i;
    const float peakDb = (e->getParameter(e, curve0 + peakCol) - 0.5f) * 30.0f;
    const float colHz = teq::colFreq(peakCol, teq::kCurveCols);
    CHECK(colHz > 960 && colHz < 1040 && peakDb > 11.5f && peakDb < 12.01f, "curve peak %.2f dB at column %d (%.0f Hz)", peakDb, peakCol, colHz);
    {
        const int sent = countAutomates(curve0, curve0 + teq::kCurveCols);
        CHECK(automates[curve0 + peakCol] >= 1, "moved peak column %d was not announced to the host", peakCol);
        CHECK(automates[curve0] == 0 && automates[curve0 + teq::kCurveCols - 1] == 0, "unchanged end columns were re-sent");
        CHECK(sent > 20 && sent <= 2 * teq::kCurveCols, "drag announced %d curve columns", sent);
        std::printf("drag: %d curve columns announced\n", sent);
    }
    CHECK(display(e, f3) == "1.00 kHz" && display(e, g3) == "+12.0 dB" && display(e, q3) == "2.00",
          "display texts: '%s' '%s' '%s'", display(e, f3).c_str(), display(e, g3).c_str(), display(e, q3).c_str());

    // The audio agrees with the published curve at the peak column's frequency.
    clearAutomates();
    run(150, colHz, 0.1f);
    double s = 0; for (int i = 0; i < B; ++i) s = std::fmax(s, std::fabs(l[i]));
    for (int k = 0; k < 20; ++k) { run(1, colHz, 0.1f); for (int i = 0; i < B; ++i) s = std::fmax(s, std::fabs(l[i])); }
    const float audioDb = 20.0f * std::log10((float)s / 0.1f);
    CHECK(std::fabs(audioDb - peakDb) < 0.15f, "audio %.2f dB vs curve %.2f dB at %.0f Hz", audioDb, peakDb, colHz);

    // Analyser: a 1 kHz tone lights the column around 1 kHz, well above the floor.
    settle();
    CHECK(countAutomates(spec0, spec0 + teq::kSpecCols) > 0, "analyser columns not announced");
    int best = 0;
    for (int i = 1; i < teq::kSpecCols; ++i) if (e->getParameter(e, spec0 + i) > e->getParameter(e, spec0 + best)) best = i;
    const float bestHz = teq::colFreq(best, teq::kSpecCols);
    CHECK(bestHz > 900 && bestHz < 1250 && e->getParameter(e, spec0 + best) > 0.6f, "analyser peak col %d (%.0f Hz) = %.2f",
          best, bestHz, e->getParameter(e, spec0 + best));
    // Analyzer Off zeroes the columns.
    e->setParameter(e, findParam(e, "Analyzer"), 0.0f);
    run(30, 1000, 0.1f);
    settle();
    float mx = 0; for (int i = 0; i < teq::kSpecCols; ++i) mx = std::fmax(mx, e->getParameter(e, spec0 + i));
    CHECK(mx == 0.0f, "Analyzer Off leaves columns at %.2f", mx);

    // A cut type shows "--" for gain, and the curve drops below the screen bottom.
    e->setParameter(e, findParam(e, "On 1"), 1.0f);
    e->setParameter(e, t1, 0.0f);   // LoCut48, default 30 Hz
    run(12, 1000, 0.0f);
    settle();
    CHECK(display(e, findParam(e, "Gain 1")) == "--", "cut gain text '%s'", display(e, findParam(e, "Gain 1")).c_str());
    CHECK(e->getParameter(e, curve0) < 0.05f, "LoCut48 at 30 Hz: first column (20.4 Hz) at %.3f, expected near the bottom",
          e->getParameter(e, curve0));

    // A stray touch on a column is corrected within ~1/30 s.
    settle();
    clearAutomates();
    e->setParameter(e, curve0 + 100, 0.0f);
    run(12, 1000, 0.0f);
    settle();
    CHECK(std::fabs(e->getParameter(e, curve0 + 100) - 0.5f) < 0.2f, "column 101 not restored (%.2f)", e->getParameter(e, curve0 + 100));
    CHECK(automates[curve0 + 100] == 1, "restored column 101 announced %d times (want 1)", automates[curve0 + 100].load());

    // Band-number tap (the skin toggles Pick 5): the plugin selects band 5, resets the trigger and tells the host.
    {
        const int follow = findParam(e, "QLink Follow"), pick5 = findParam(e, "Pick 5"), band = findParam(e, "Band");
        CHECK(follow == 334 && pick5 == 339 && band == 3, "new parameters moved: follow %d pick5 %d", follow, pick5);
        CHECK(e->getParameter(e, follow) == 1.0f && display(e, follow) == "On", "QLink Follow should default to On");
        CHECK(e->dispatcher(e, effCanBeAutomated, pick5, 0, nullptr, 0) == 0, "Pick triggers must not be automatable");
        CHECK(e->dispatcher(e, effCanBeAutomated, follow, 0, nullptr, 0) == 1, "QLink Follow should be automatable");
        settle();
        clearAutomates();
        e->setParameter(e, pick5, 1.0f);
        settle();
        CHECK(std::fabs(e->getParameter(e, band) - 4.0f / 7.0f) < 1e-4f, "Pick 5 did not select band 5 (Band = %.3f)",
              e->getParameter(e, band));
        CHECK(e->getParameter(e, pick5) == 0.0f, "Pick 5 trigger not reset");
        CHECK(automates[band].load() == 1 && automates[pick5].load() == 1, "host told about Band %d / Pick 5 %d times (want 1/1)",
              automates[band].load(), automates[pick5].load());
        // A second tap on another band works the same way.
        e->setParameter(e, findParam(e, "Pick 2"), 1.0f);
        settle();
        CHECK(std::fabs(e->getParameter(e, band) - 1.0f / 7.0f) < 1e-4f, "Pick 2 did not select band 2");
    }

    // Real time, like the MPC: 128-frame blocks paced at 44.1 kHz for 2 s, music-like audio (a moving tone), the
    // analyser on, and band 3 dragged 30 times a second across its whole range. Each host notification costs the MPC
    // work (too many caused dropouts on the Key 61), so the plugin must stay within its budget.
    {
        e->setParameter(e, findParam(e, "Analyzer"), 1.0f);
        settle();
        clearAutomates();
        const int blocks = (int)(2.0 * 44100 / B);
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < blocks; ++k)
        {
            if (k % 11 == 0)
            {
                const float ph = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * (float)k / 172.0f);   // 2 swipes a second
                e->setParameter(e, f3, 0.15f + 0.7f * ph);
                e->setParameter(e, g3, 1.0f - ph);
            }
            run(1, 80.0f + 40.0f * (float)(k % 60), 0.2f);
            std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)((k + 1) * B * 1e6 / 44100)));
        }
        const double perSec = countAutomates(0, 400) / 2.0;
        std::printf("real-time drag with audio: %.0f host notifications per second (curve %d, analyser %d in 2 s)\n", perSec,
                    countAutomates(curve0, curve0 + teq::kCurveCols), countAutomates(spec0, spec0 + teq::kSpecCols));
        CHECK(perSec <= 2400.0 + 60.0, "too many host notifications: %.0f per second", perSec);
        CHECK(countAutomates(curve0, curve0 + teq::kCurveCols) > 100, "curve hardly updated during the drag");
        // put band 3 back where the state test below expects it, and let the screen catch up fully
        e->setParameter(e, f3, x);
        e->setParameter(e, g3, y);
        run(12, 1000, 0.0f);
        settle(400);
        int peak2 = 0;
        for (int i = 1; i < teq::kCurveCols; ++i) if (e->getParameter(e, curve0 + i) > e->getParameter(e, curve0 + peak2)) peak2 = i;
        CHECK(peak2 == peakCol, "curve did not settle back on the 1 kHz peak (column %d, expected %d)", peak2, peakCol);
    }

    // State round trip keeps the band settings.
    void* chunk = nullptr;
    const VstIntPtr size = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
    std::vector<char> copy((const char*)chunk, (const char*)chunk + size);
    e->setParameter(e, g3, 0.5f);
    e->dispatcher(e, effSetChunk, 0, (VstIntPtr)copy.size(), copy.data(), 0);
    CHECK(display(e, g3) == "+12.0 dB", "state round trip: Gain 3 = %s", display(e, g3).c_str());
    std::printf("chunk %ld bytes\n", (long)size);

    CHECK(automatesOnAudioThread.load() == 0, "%d host notifications came from the audio thread", automatesOnAudioThread.load());
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    std::printf(fails ? "plugin_e2e: %d FAILED\n" : "plugin_e2e: all checks passed\n", fails);
    return fails ? 1 : 0;
}
