// dsp_test.cpp - TouchEQ 8 engine checks. Build: g++ -std=c++17 -O2 -Icommon tests/dsp_test.cpp
//
// What is checked:
//   * flat settings are bit-transparent; every filter type's measured sine response matches the on-screen curve
//     (curveDb / responseDbAt) to 0.1 dB, which proves the screen shows what you hear
//   * bell gain and width, shelf plateaus, 12 and 48 dB/oct slopes, notch depth, resonance, Adaptive Q, Scale
//   * parameter moves, on/off and type switches are click-free; random-parameter fuzz stays finite and bounded
//   * Q knob mapping, the analyser's level calibration and frequency placement, Mix and Output, CPU use
#include "toucheq_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace teq;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const float kFs = 44100.0f;

static void flatBands(Engine& e)
{
    for (int b = 0; b < kBands; ++b) { BandParams p; p.on = false; e.setBand(b, p); }
}

// Gain of the engine at frequency f (dB), measured with a sine after the transient has died away.
static float measureDb(Engine& e, float f, float seconds = 0.6f)
{
    const int n = (int)(seconds * kFs), tail = (int)(0.2f * kFs);
    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; ++i) l[i] = r[i] = 0.25f * std::sin(2.0f * kPi * f * (float)i / kFs);
    e.reset();
    for (int at = 0; at < n; at += 128) { int m = std::min(128, n - at); e.process(&l[at], &r[at], &l[at], &r[at], m); }
    double sumY = 0.0, sumX = 0.0;
    for (int i = n - tail; i < n; ++i) { double x = 0.25 * std::sin(2.0 * M_PI * f * i / kFs); sumX += x * x; sumY += (double)l[i] * l[i]; }
    return (float)(10.0 * std::log10(sumY / sumX + 1e-30));
}

static Engine* makeEngine()
{
    Engine* e = new Engine();
    e->setGlobal(0.0f, 1.0f, 100.0f, false, false);
    e->prepare(kFs);
    flatBands(*e);
    e->reset();
    return e;
}

static void setOne(Engine& e, int type, float f, float g, float q, int band = 3)
{
    flatBands(e);
    BandParams p; p.on = true; p.type = type; p.freq = f; p.gain = g; p.q = q;
    e.setBand(band, p);
    e.reset();
}

int main()
{
    // ---- Q mapping -------------------------------------------------------------------------------------------------
    CHECK(std::fabs(qFromNorm(0.5f) - 0.7071f) < 1e-3f, "Q centre = %f", qFromNorm(0.5f));
    CHECK(std::fabs(qFromNorm(0.0f) - 0.1f) < 1e-4f && std::fabs(qFromNorm(1.0f) - 18.0f) < 1e-2f, "Q ends");
    {
        float worst = 0.0f;
        for (int i = 0; i <= 100; ++i) worst = std::max(worst, std::fabs(normFromQ(qFromNorm(i / 100.0f)) - i / 100.0f));
        CHECK(worst < 1e-5f, "Q round trip error %g", worst);
    }

    Engine& e = *makeEngine();

    // ---- transparency ----------------------------------------------------------------------------------------------
    {
        // Default-style setup: six 0 dB bells/shelves on. Must be an exact pass-through.
        for (int b = 0; b < kBands; ++b)
        {
            BandParams p; p.on = b >= 1 && b <= 6; p.type = b == 1 ? kLoShelf : (b == 6 ? kHiShelf : kBell);
            p.freq = colFreq(b * 20, kCurveCols); p.gain = 0.0f; p.q = 0.71f; e.setBand(b, p);
        }
        e.reset();
        std::mt19937 rng(1); std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        std::vector<float> l(4096), r(4096), l0, r0;
        for (int i = 0; i < 4096; ++i) { l[i] = u(rng); r[i] = u(rng); }
        l0 = l; r0 = r;
        e.process(l.data(), r.data(), l.data(), r.data(), 4096);
        float err = 0.0f;
        for (int i = 0; i < 4096; ++i) err = std::max(err, std::max(std::fabs(l[i] - l0[i]), std::fabs(r[i] - r0[i])));
        CHECK(err < 1e-6f, "0 dB bands are not transparent (max error %g)", err);
        float cv[kCurveCols]; e.curveDb(cv);
        float cmax = 0.0f; for (float v : cv) cmax = std::max(cmax, std::fabs(v));
        CHECK(cmax < 1e-4f, "flat curve deviates %g dB", cmax);
    }

    // ---- every type: measured response == displayed curve ---------------------------------------------------------
    {
        struct Case { int type; float f, g, q; };
        const Case cases[] = {
            { kBell, 1000, 12, 1.0f }, { kBell, 120, -9, 4.0f }, { kBell, 9000, 6, 0.4f },
            { kLoShelf, 200, 8, 0.71f }, { kLoShelf, 80, -10, 1.5f }, { kHiShelf, 5000, 6, 0.71f }, { kHiShelf, 12000, -12, 0.5f },
            { kLoCut12, 300, 0, 0.71f }, { kLoCut12, 300, 0, 4.0f }, { kHiCut12, 2000, 0, 2.0f },
            { kLoCut48, 200, 0, 0.71f }, { kHiCut48, 3000, 0, 0.71f }, { kHiCut48, 3000, 0, 3.0f }, { kNotch, 1000, 0, 2.0f },
        };
        const float probes[] = { 40, 100, 180, 300, 520, 1000, 1700, 3000, 5200, 9000, 15000 };
        for (const Case& c : cases)
        {
            setOne(e, c.type, c.f, c.g, c.q);
            float worst = 0.0f, wf = 0.0f, wm = 0.0f, wp = 0.0f;
            for (float f : probes)
            {
                const float pred = e.responseDbAt(f);
                if (pred < -70.0f) continue;   // below the measurement's noise floor
                const float meas = measureDb(e, f);
                // 0.1 dB, relaxed to 0.25 dB below -30 dB where the float sine measurement itself wanders
                const float err = std::fabs(meas - pred) * (pred < -30.0f ? 0.4f : 1.0f);
                if (err > worst) { worst = err; wf = f; wm = meas; wp = pred; }
            }
            CHECK(worst < 0.1f, "%s %.0f Hz %+.0f dB Q %.2f: curve vs audio off by %.3f (weighted) dB at %.0f Hz (audio %.2f, curve %.2f)",
                  typeName(c.type), c.f, c.g, c.q, worst, wf, wm, wp);
        }
    }

    // ---- shapes ----------------------------------------------------------------------------------------------------
    {
        setOne(e, kBell, 1000, 12, 1.0f);
        CHECK(std::fabs(e.responseDbAt(1000) - 12.0f) < 0.05f, "bell peak %.2f", e.responseDbAt(1000));
        CHECK(std::fabs(e.responseDbAt(60)) < 0.3f, "bell far skirt %.2f", e.responseDbAt(60));
        // Simper/RBJ bell: the +6 dB (half gain in dB) points of a Q=1 bell sit near 1 octave apart around the centre
        float lo = 1000, hi = 1000;
        while (e.responseDbAt(lo) > 6.0f) lo *= 0.995f;
        while (e.responseDbAt(hi) > 6.0f) hi *= 1.005f;
        CHECK(std::log2(hi / lo) > 0.8f && std::log2(hi / lo) < 1.6f, "bell half-gain width %.2f oct", std::log2(hi / lo));

        setOne(e, kLoShelf, 200, 8, 0.71f);
        CHECK(std::fabs(e.responseDbAt(20) - 8.0f) < 0.2f && std::fabs(e.responseDbAt(5000)) < 0.1f, "low shelf plateau");
        CHECK(std::fabs(e.responseDbAt(200) - 4.0f) < 0.3f, "low shelf midpoint %.2f", e.responseDbAt(200));
        setOne(e, kHiShelf, 4000, -6, 0.71f);
        CHECK(std::fabs(e.responseDbAt(19000) + 6.0f) < 0.3f && std::fabs(e.responseDbAt(100)) < 0.1f, "high shelf plateau");

        setOne(e, kLoCut12, 1000, 0, 0.7071f);
        CHECK(std::fabs(e.responseDbAt(1000) + 3.01f) < 0.1f, "12 dB cut -3 dB point %.2f", e.responseDbAt(1000));
        CHECK(std::fabs(e.responseDbAt(125) - e.responseDbAt(250) + 12.0f) < 0.6f, "12 dB/oct slope %.2f",
              e.responseDbAt(125) - e.responseDbAt(250));
        setOne(e, kLoCut48, 1000, 0, 0.7071f);
        CHECK(std::fabs(e.responseDbAt(1000) + 3.01f) < 0.1f, "48 dB cut -3 dB point %.2f", e.responseDbAt(1000));
        CHECK(std::fabs(e.responseDbAt(250) - e.responseDbAt(500) + 48.0f) < 1.0f, "48 dB/oct slope %.2f",
              e.responseDbAt(250) - e.responseDbAt(500));
        CHECK(e.responseDbAt(2000) > -0.2f && e.responseDbAt(2000) < 0.01f, "48 dB passband flat %.3f", e.responseDbAt(2000));
        setOne(e, kHiCut48, 1000, 0, 4.0f);
        float pk = -100; for (float f = 300; f < 1500; f *= 1.01f) pk = std::max(pk, e.responseDbAt(f));
        CHECK(pk > 6.0f, "48 dB resonance peak %.2f dB", pk);
        setOne(e, kHiCut12, 1000, 0, 4.0f);
        CHECK(std::fabs(e.responseDbAt(1000) - 20.0f * std::log10(4.0f)) < 0.2f, "12 dB resonance at fc %.2f", e.responseDbAt(1000));

        setOne(e, kNotch, 1000, 0, 2.0f);
        CHECK(e.responseDbAt(1000) < -60.0f, "notch depth %.1f", e.responseDbAt(1000));
        CHECK(measureDb(e, 1000) < -50.0f, "notch depth (audio) %.1f", measureDb(e, 1000));
    }

    // ---- Scale and Adaptive Q --------------------------------------------------------------------------------------
    {
        setOne(e, kBell, 1000, 10, 1.0f);
        e.setGlobal(0.0f, 0.5f, 100.0f, false, false);
        BandParams p; p.on = true; p.type = kBell; p.freq = 1000; p.gain = 10; p.q = 1.0f; e.setBand(3, p);
        CHECK(std::fabs(e.responseDbAt(1000) - 5.0f) < 0.05f, "Scale 50%% %.2f", e.responseDbAt(1000));
        e.setGlobal(0.0f, 2.0f, 100.0f, false, false); e.setBand(3, p);
        CHECK(std::fabs(e.responseDbAt(1000) - 20.0f) < 0.05f, "Scale 200%% %.2f", e.responseDbAt(1000));
        e.setGlobal(0.0f, 1.0f, 100.0f, false, false); e.setBand(3, p);
        const float wide = e.responseDbAt(1400);
        e.setGlobal(0.0f, 1.0f, 100.0f, true, false); e.setBand(3, p);
        CHECK(e.responseDbAt(1400) < wide - 1.0f && std::fabs(e.responseDbAt(1000) - 10.0f) < 0.05f,
              "Adaptive Q narrows the bell (%.2f -> %.2f dB at 1.4 kHz)", wide, e.responseDbAt(1400));
        e.setGlobal(0.0f, 1.0f, 100.0f, false, false);
    }

    // ---- Mix and Output --------------------------------------------------------------------------------------------
    {
        setOne(e, kNotch, 1000, 0, 2.0f);
        e.setGlobal(0.0f, 1.0f, 50.0f, false, false); e.reset();
        CHECK(std::fabs(measureDb(e, 1000) + 6.02f) < 0.3f, "50%% mix of a notch = -6 dB, got %.2f", measureDb(e, 1000));
        flatBands(e);
        e.setGlobal(-6.0f, 1.0f, 100.0f, false, false); e.reset();
        CHECK(std::fabs(measureDb(e, 500) + 6.0f) < 0.05f, "Output -6 dB, got %.2f", measureDb(e, 500));
        e.setGlobal(0.0f, 1.0f, 100.0f, false, false); e.reset();
    }

    // ---- click-free moves, toggles and type switches ---------------------------------------------------------------
    {
        // A 100 Hz sine through a bell; jump the bell from 0 to +15 dB, flip it off/on and change its type mid-signal.
        // Largest sample-to-sample step must stay near the sine's own (2 pi f / fs * amplitude * max gain).
        setOne(e, kBell, 100, 0, 1.0f);
        const int n = (int)(1.5f * kFs);
        std::vector<float> l(n), r(n);
        for (int i = 0; i < n; ++i) l[i] = r[i] = 0.2f * std::sin(2.0f * kPi * 100.0f * (float)i / kFs);
        float maxStep = 0.0f, prev = 0.0f;
        for (int at = 0; at < n; at += 64)
        {
            BandParams p; p.on = true; p.type = kBell; p.freq = 100; p.q = 1.0f; p.gain = 0;
            if (at > (int)(0.2f * kFs)) p.gain = 15.0f;
            if (at > (int)(0.5f * kFs)) p.on = false;
            if (at > (int)(0.7f * kFs)) p.on = true;
            if (at > (int)(0.9f * kFs)) p.type = kLoShelf;
            if (at > (int)(1.1f * kFs)) { p.type = kLoCut48; p.freq = 60; }
            if (at > (int)(1.3f * kFs)) p.freq = 2000;
            e.setBand(3, p);
            e.process(&l[at], &r[at], &l[at], &r[at], std::min(64, n - at));
            for (int i = at; i < std::min(at + 64, n); ++i) { maxStep = std::max(maxStep, std::fabs(l[i] - prev)); prev = l[i]; }
        }
        const float sineStep = 0.2f * 2.0f * kPi * 100.0f / kFs * 5.7f;   // +15 dB = x5.6 amplitude
        CHECK(maxStep < sineStep * 1.6f, "click: max step %.4f (smooth sine at +15 dB steps %.4f)", maxStep, sineStep);
        bool finite = true; for (float v : l) finite &= std::isfinite(v);
        CHECK(finite, "non-finite output after switching");
    }

    // ---- analyser --------------------------------------------------------------------------------------------------
    {
        flatBands(e);
        e.setGlobal(0.0f, 1.0f, 100.0f, false, true); e.reset();
        const int n = 8 * kFftSize;
        std::vector<float> l(n), r(n);
        const float f = 1000.0f, amp = 0.5f;   // -6 dBFS
        for (int i = 0; i < n; ++i) l[i] = r[i] = amp * std::sin(2.0f * kPi * f * (float)i / kFs);
        e.process(l.data(), r.data(), l.data(), r.data(), n);
        // The screen thread's view: a frame from the tap, analysed separately from the audio.
        Analyzer an; an.prepare(kFs);
        std::vector<float> frame(kFftSize);
        CHECK(e.tap().written() == (uint32_t)n, "tap holds %u samples, expected %d", e.tap().written(), n);
        e.tap().copyLatest(e.tap().written(), frame.data());
        CHECK(std::fabs(frame[kFftSize - 1] - l[n - 1]) < 1e-6f, "tap's newest sample is not the output's last");
        an.analyse(frame.data(), kFftSize);
        const float* d = an.db();
        int best = 0; for (int c = 1; c < kSpecCols; ++c) if (d[c] > d[best]) best = c;
        const float lo = kFMin * std::pow(kFMax / kFMin, (float)best / kSpecCols), hi = kFMin * std::pow(kFMax / kFMin, (float)(best + 1) / kSpecCols);
        CHECK(f >= lo * 0.97f && f <= hi * 1.03f, "analyser peak in column %d (%.0f..%.0f Hz), expected 1 kHz", best, lo, hi);
        const float tilt = kSpecTiltDb * std::log2(std::sqrt(lo * hi) / 1000.0f);
        CHECK(std::fabs(d[best] - tilt + 6.02f) < 1.5f, "analyser level %.2f dB (tilt %.2f) for a -6 dBFS sine", d[best], tilt);
        CHECK(d[0] < -60.0f && d[kSpecCols - 1] < -60.0f, "analyser leakage far away: %.1f / %.1f", d[0], d[kSpecCols - 1]);
        // silence falls back to the floor, at about 30 dB/s: not yet after 1 s, fully after 4 s
        std::vector<float> z(kFftSize, 0.0f), z2(kFftSize, 0.0f);
        const int perSec = (int)(kFs / kFftSize);
        for (int k = 0; k < perSec; ++k)
        {
            e.process(z.data(), z2.data(), z.data(), z2.data(), kFftSize);
            e.tap().copyLatest(e.tap().written(), frame.data());
            an.analyse(frame.data(), kFftSize);
        }
        CHECK(an.db()[best] > kSpecFloorDb + 20.0f, "analyser fell too fast (%.1f dB after 1 s)", an.db()[best]);
        for (int k = 0; k < 3 * perSec; ++k) { e.tap().copyLatest(e.tap().written(), frame.data()); an.analyse(frame.data(), kFftSize); }
        CHECK(an.db()[best] <= kSpecFloorDb + 0.01f, "analyser does not fall to the floor (%.1f)", an.db()[best]);
        // Analyzer off: the audio thread stops feeding the tap.
        e.setGlobal(0.0f, 1.0f, 100.0f, false, false);
        const uint32_t before = e.tap().written();
        e.process(z.data(), z2.data(), z.data(), z2.data(), kFftSize);
        CHECK(e.tap().written() == before, "tap still fed with the analyser off");
    }

    // ---- fuzz ------------------------------------------------------------------------------------------------------
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        e.setGlobal(0.0f, 1.0f, 100.0f, true, true); e.reset();
        const int blocks = (int)(20.0f * kFs / 128);
        float peak = 0.0f; bool finite = true;
        std::vector<float> l(128), r(128);
        for (int k = 0; k < blocks; ++k)
        {
            if (k % 20 == 0)
                for (int b = 0; b < kBands; ++b)
                {
                    BandParams p; p.on = u(rng) < 0.7f; p.type = (int)(u(rng) * kNumTypes) % kNumTypes;
                    p.freq = kFMin * std::pow(kFMax / kFMin, u(rng)); p.gain = (u(rng) * 2 - 1) * kGainRange;
                    p.q = qFromNorm(u(rng)); e.setBand(b, p);
                }
            for (int i = 0; i < 128; ++i) { l[i] = 0.3f * (u(rng) * 2 - 1); r[i] = 0.3f * (u(rng) * 2 - 1); }
            e.process(l.data(), r.data(), l.data(), r.data(), 128);
            for (int i = 0; i < 128; ++i) { finite &= std::isfinite(l[i]) && std::isfinite(r[i]); peak = std::max(peak, std::fabs(l[i])); }
            float cv[kCurveCols]; e.curveDb(cv);
            for (float v : cv) finite &= std::isfinite(v);
        }
        CHECK(finite, "fuzz: non-finite audio or curve");
        CHECK(peak < 400.0f, "fuzz: output peak %.1f (8 x 15 dB of gain can reach ~200 on noise)", peak);
    }

    // ---- bands at 0 dB are skipped: bit-exact pass-through, and close to free -------------------------------------
    {
        const int types[3] = { kLoShelf, kBell, kHiShelf };
        for (int b = 0; b < kBands; ++b)
        {
            BandParams p; p.on = true; p.type = types[b % 3]; p.freq = colFreq(b * 24 + 10, kCurveCols); p.gain = 0; p.q = 2.0f;
            e.setBand(b, p);
        }
        e.setGlobal(0.0f, 1.0f, 100.0f, false, false); e.reset();
        std::vector<float> l(4096), r(4096), x(4096);
        std::mt19937 rng(3); std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (int i = 0; i < 4096; ++i) x[i] = l[i] = r[i] = u(rng);
        for (int at = 0; at < 4096; at += 128) e.process(&l[at], &r[at], &l[at], &r[at], 128);
        bool exact = true; for (int i = 0; i < 4096; ++i) exact &= l[i] == x[i] && r[i] == x[i];
        CHECK(exact, "eight 0 dB bells/shelves are not a bit-exact pass-through");
        std::vector<float> bl(128, 0.1f), br(128, 0.1f);
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < (int)(10.0f * kFs / 128); ++k) e.process(bl.data(), br.data(), bl.data(), br.data(), 128);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("CPU: 10 s with eight 0 dB bands on (skipped): %.1f ms = %.2f%% of one core\n", s * 1000.0, s / 10.0 * 100.0);
    }

    // ---- CPU -------------------------------------------------------------------------------------------------------
    {
        for (int b = 0; b < kBands; ++b)
        {
            BandParams p; p.on = true; p.type = b == 0 ? kLoCut48 : (b == 7 ? kHiCut48 : kBell);
            p.freq = colFreq(b * 24 + 10, kCurveCols); p.gain = 3; p.q = 1.0f; e.setBand(b, p);
        }
        e.setGlobal(0.0f, 1.0f, 100.0f, false, true); e.reset();
        std::vector<float> l(128, 0.1f), r(128, 0.1f);
        const int blocks = (int)(10.0f * kFs / 128);
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < blocks; ++k)
        {
            if (k % 8 == 0) { BandParams p; p.on = true; p.type = kBell; p.freq = 500.0f + (float)(k % 400); p.gain = 3; p.q = 1; e.setBand(3, p); }
            e.process(l.data(), r.data(), l.data(), r.data(), 128);
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        float cv[kCurveCols];
        auto t1 = std::chrono::steady_clock::now();
        for (int k = 0; k < 1000; ++k) e.curveDb(cv);
        const double cs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() / 1000.0;
        Analyzer an; an.prepare(kFs);
        std::vector<float> frame(kFftSize);
        e.tap().copyLatest(e.tap().written(), frame.data());
        auto t2 = std::chrono::steady_clock::now();
        for (int k = 0; k < 1000; ++k) an.analyse(frame.data(), kFftSize);
        const double fs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count() / 1000.0;
        std::printf("CPU (x86, this machine): audio thread, 10 s of stereo, all 8 bands on (two 48 dB cuts), analyser tap on:"
                    " %.1f ms = %.2f%% of one core\n  screen thread: one curve rebuild %.1f us, one analyser frame %.1f us\n",
                    s * 1000.0, s / 10.0 * 100.0, cs * 1e6, fs * 1e6);
        CHECK(s < 1.0, "too slow: %.2f s for 10 s", s);
    }

    delete &e;
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
