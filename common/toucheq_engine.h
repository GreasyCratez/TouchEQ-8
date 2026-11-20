// toucheq_engine.h - TouchEQ 8: the 8-band EQ engine, its display curve and its spectrum analyser.
//
// SPDX-License-Identifier: MIT
//
// Header-only and free of the plugin framework, so tests/dsp_test.cpp exercises exactly this code.
//
// Signal flow (stereo, both channels identical):
//   in -> band 1 -> band 2 -> ... -> band 8 -> dry/wet mix -> output gain -> out
//                                                                         \-> analyser (mono sum, post EQ)
//
// Each band is one to four sections of a zero-delay-feedback state-variable filter (A. Simper, "Linear trap
// optimised SVF", Cytomic 2013, the published equations; no third-party code is used). The same section
// coefficients drive the on-screen curve: a TPT/bilinear SVF has exactly the response of its analog prototype at
// the pre-warped frequency tan(pi f / fs) / g, so curveDb() is the DSP's true magnitude response, not a sketch.
//
// Filter types (the order is frozen: saved projects store the index):
//   0 LoCut48   8th-order Butterworth high-pass (4 sections), Q = resonance of the last section
//   1 LoCut12   2nd-order high-pass, Q = resonance
//   2 LoShelf   2nd-order low shelf, Q = shelf slope / overshoot
//   3 Bell      peaking, constant-Q (Adaptive Q narrows it as the gain grows)
//   4 HiShelf   2nd-order high shelf
//   5 Notch     2nd-order band reject, Q = width
//   6 HiCut12   2nd-order low-pass, Q = resonance
//   7 HiCut48   8th-order Butterworth low-pass (4 sections)
// Gain only acts on types 2..4; the screen moves the point's height with Q for the other types.
//
// Click-free changes: frequency (log), gain and Q glide with a ~12 ms one-pole per 16-sample chunk and the section
// coefficients are recomputed per chunk while gliding. Switching a band on/off fades it in/out over 6 ms
// (y = x + m (band(x) - x)). Changing the type fades the band out, swaps the filter, clears its state and fades in.
// A settled bell or shelf at exactly 0 dB is an exact pass-through, so it is skipped (no CPU).
//
// Threads: Engine::process is the only thing the audio thread runs. The screen (CurveModel, Analyzer) runs on
// another thread and gets its audio from the lock-free SampleTap that process() fills.
#ifndef TOUCHEQ_ENGINE_H
#define TOUCHEQ_ENGINE_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace teq {

constexpr int kBands = 8;
constexpr int kCurveCols = 192;          // curve columns on screen (6 px each over the 1152 px plot)
constexpr int kSpecCols = 96;            // analyser columns (12 px each)
constexpr float kFMin = 20.0f, kFMax = 20000.0f;
constexpr float kGainRange = 15.0f;      // band gain and the screen's dB axis: -15 .. +15 dB
constexpr float kSpecFloorDb = -90.0f;   // analyser axis: -90 dBFS (bottom) .. 0 dBFS (top)
constexpr float kSpecTiltDb = 4.5f;      // analyser tilt per octave around 1 kHz (makes pink noise look flat)
constexpr int kFftSize = 2048;
constexpr int kChunk = 16;               // coefficient update interval while gliding
constexpr float kPi = 3.14159265358979f;

enum FilterType { kLoCut48, kLoCut12, kLoShelf, kBell, kHiShelf, kNotch, kHiCut12, kHiCut48, kNumTypes };
inline const char* typeName(int t)
{
    static const char* n[kNumTypes] = { "LoCut48", "LoCut12", "LoShelf", "Bell", "HiShelf", "Notch", "HiCut12", "HiCut48" };
    return n[std::min(std::max(t, 0), kNumTypes - 1)];
}
inline bool typeUsesGain(int t) { return t == kLoShelf || t == kBell || t == kHiShelf; }

// Q knob: 0..1 -> 0.10 .. 18, log in two halves so the centre is 0.71 (Butterworth, a flat cut filter) and the
// height of a cut filter's point on screen reads as "more resonance" above the 0 dB line.
constexpr float kQMin = 0.1f, kQMid = 0.70710678f, kQMax = 18.0f;
inline float qFromNorm(float u)
{
    u = std::min(std::max(u, 0.0f), 1.0f);
    return u <= 0.5f ? kQMin * std::pow(kQMid / kQMin, 2.0f * u) : kQMid * std::pow(kQMax / kQMid, 2.0f * u - 1.0f);
}
inline float normFromQ(float q)
{
    q = std::min(std::max(q, kQMin), kQMax);
    return q <= kQMid ? 0.5f * std::log(q / kQMin) / std::log(kQMid / kQMin)
                      : 0.5f + 0.5f * std::log(q / kQMid) / std::log(kQMax / kQMid);
}

// Column centre frequencies, log spaced over 20 Hz .. 20 kHz (the same axis as the Freq parameter and the XY pads).
inline float colFreq(int i, int n) { return kFMin * std::pow(kFMax / kFMin, ((float)i + 0.5f) / (float)n); }

struct BandParams
{
    bool on = false;
    int type = kBell;
    float freq = 1000.0f;   // Hz
    float gain = 0.0f;      // dB, -15..15 (before Scale)
    float q = kQMid;
};

// One SVF section: kernel (a1..a3 from g and k) plus the output mix m0 v0 + m1 v1 + m2 v2.
struct Section
{
    float g = 0.0f, k = 1.0f;          // kept for the analog-prototype response
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;

    void setKernel(float gg, float kk)
    {
        g = gg; k = kk;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // |H|^2 at a frequency whose pre-warped value is t = tan(pi f / fs).
    float magSq(float t) const
    {
        const float w = t / g;
        const float nr = (m0 + m2) - m0 * w * w, ni = (m0 * k + m1) * w;
        const float dr = 1.0f - w * w, di = k * w;
        return (nr * nr + ni * ni) / std::max(dr * dr + di * di, 1.0e-30f);
    }
};

struct SectionState { float ic1 = 0.0f, ic2 = 0.0f; };

inline float tick(const Section& c, SectionState& s, float v0)
{
    const float v3 = v0 - s.ic2;
    const float v1 = c.a1 * s.ic1 + c.a2 * v3;
    const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}

// Butterworth 8th order = four 2nd-order sections with these Qs (1 / (2 cos((2i-1) pi / 16))).
constexpr float kBw8Q[4] = { 0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f };

// Design one band into up to four sections. Returns the section count.
inline int designBand(int type, float freq, float gainDb, float q, bool adaptiveQ, float fs, Section* sec)
{
    freq = std::min(std::max(freq, 10.0f), 0.45f * fs);
    q = std::min(std::max(q, kQMin), kQMax);
    const float g = std::tan(kPi * freq / fs);
    switch (type)
    {
        case kLoCut12:
        case kHiCut12:
        {
            const float k = 1.0f / q;
            sec[0].setKernel(g, k);
            if (type == kLoCut12) { sec[0].m0 = 1.0f; sec[0].m1 = -k; sec[0].m2 = -1.0f; }
            else                  { sec[0].m0 = 0.0f; sec[0].m1 = 0.0f; sec[0].m2 = 1.0f; }
            return 1;
        }
        case kLoCut48:
        case kHiCut48:
        {
            const float reso = q / kQMid;   // 1 at the centre of the Q knob = plain Butterworth
            for (int i = 0; i < 4; ++i)
            {
                const float qi = i == 3 ? kBw8Q[i] * reso : kBw8Q[i];
                const float k = 1.0f / std::max(qi, 0.05f);
                sec[i].setKernel(g, k);
                if (type == kLoCut48) { sec[i].m0 = 1.0f; sec[i].m1 = -k; sec[i].m2 = -1.0f; }
                else                  { sec[i].m0 = 0.0f; sec[i].m1 = 0.0f; sec[i].m2 = 1.0f; }
            }
            return 4;
        }
        case kNotch:
        {
            const float k = 1.0f / q;
            sec[0].setKernel(g, k);
            sec[0].m0 = 1.0f; sec[0].m1 = -k; sec[0].m2 = 0.0f;
            return 1;
        }
        case kLoShelf:
        {
            const float A = std::pow(10.0f, gainDb / 40.0f);
            const float k = 1.0f / q;
            sec[0].setKernel(g / std::sqrt(A), k);
            sec[0].m0 = 1.0f; sec[0].m1 = k * (A - 1.0f); sec[0].m2 = A * A - 1.0f;
            return 1;
        }
        case kHiShelf:
        {
            const float A = std::pow(10.0f, gainDb / 40.0f);
            const float k = 1.0f / q;
            sec[0].setKernel(g * std::sqrt(A), k);
            sec[0].m0 = A * A; sec[0].m1 = k * (1.0f - A) * A; sec[0].m2 = 1.0f - A * A;
            return 1;
        }
        case kBell:
        default:
        {
            if (adaptiveQ) q = std::min(q * std::pow(2.0f, std::fabs(gainDb) / kGainRange), kQMax);
            const float A = std::pow(10.0f, gainDb / 40.0f);
            const float k = 1.0f / (q * A);
            sec[0].setKernel(g, k);
            sec[0].m0 = 1.0f; sec[0].m1 = k * (A * A - 1.0f); sec[0].m2 = 0.0f;
            return 1;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// One band: smoothing, fades, sections and their state.
// ---------------------------------------------------------------------------------------------------------------
class Band
{
public:
    void prepare(float fs)
    {
        fs_ = fs;
        // one-pole per chunk, ~12 ms time constant
        glide_ = 1.0f - std::exp(-(float)kChunk / (0.012f * fs));
        fadeStep_ = 1.0f / (0.006f * fs);
    }
    void setTarget(const BandParams& p, float scale, bool adaptiveQ)
    {
        if (p.freq != tgt_.freq) tgtLogF_ = std::log(p.freq);
        if (p.q != tgt_.q) tgtLogQ_ = std::log(p.q);
        tgt_ = p;
        tgtGain_ = p.gain * scale;
        adaptive_ = adaptiveQ;
    }
    void reset()
    {
        for (auto& ch : st_) for (auto& s : ch) s = SectionState();
        cur_ = tgt_;
        curLogF_ = std::log(std::min(std::max(tgt_.freq, kFMin * 0.5f), kFMax * 1.1f));
        curGain_ = tgtGain_;
        curLogQ_ = std::log(tgt_.q);
        mix_ = tgt_.on ? 1.0f : 0.0f;
        redesign();
    }
    // Called once per chunk: glide, fade bookkeeping and coefficients. Returns false when the band can be skipped.
    bool beginChunk(int n)
    {
        const bool wantOn = tgt_.on;
        const bool typeChange = tgt_.type != cur_.type;
        fadeTarget_ = (wantOn && !typeChange) ? 1.0f : 0.0f;
        if (typeChange && mix_ <= 0.0f)
        {
            // Faded out: swap the filter now, from a clean state, and snap to the target values.
            cur_.type = tgt_.type;
            curLogF_ = tgtLogF_; curGain_ = tgtGain_; curLogQ_ = tgtLogQ_;
            for (auto& ch : st_) for (auto& s : ch) s = SectionState();
            redesign();
            fadeTarget_ = wantOn ? 1.0f : 0.0f;
        }
        if (mix_ <= 0.0f && fadeTarget_ <= 0.0f)
        {
            // Off: track the targets silently so switching on starts from the right place.
            curLogF_ = tgtLogF_; curGain_ = tgtGain_; curLogQ_ = tgtLogQ_;
            if (needDesign()) { for (auto& ch : st_) for (auto& s : ch) s = SectionState(); redesign(); }
            return false;
        }
        // Glide frequency (log), gain and Q (log) toward the targets.
        bool moved = false;
        moved |= glide(curLogF_, tgtLogF_, 1.0e-4f);
        moved |= glide(curGain_, tgtGain_, 1.0e-3f);
        moved |= glide(curLogQ_, tgtLogQ_, 1.0e-4f);
        if (moved || needDesign()) redesign();
        (void)n;
        // A fully-on bell or shelf resting at exactly 0 dB passes the signal untouched (m1 = m2 = 0): skip it.
        // Its state is cleared so it restarts cleanly; the gain then glides up from 0 dB, so there is no click.
        if (mix_ >= 1.0f && fadeTarget_ >= 1.0f && curGain_ == 0.0f && tgtGain_ == 0.0f && typeUsesGain(cur_.type))
        {
            for (auto& ch : st_) for (auto& s : ch) s = SectionState();
            return false;
        }
        return true;
    }

    // Process n samples in place for one channel (ch 0/1). Fades run on channel 0 and are replayed on channel 1.
    void processChannel(int ch, float* x, int n)
    {
        float m = ch == 0 ? mix_ : mixStart_;
        if (ch == 0) mixStart_ = mix_;
        SectionState* s = st_[ch];
        const int ns = nsec_;
        for (int i = 0; i < n; ++i)
        {
            float y = x[i];
            for (int k = 0; k < ns; ++k) y = tick(sec_[k], s[k], y);
            if (m != fadeTarget_)
            {
                m += m < fadeTarget_ ? fadeStep_ : -fadeStep_;
                if ((fadeTarget_ >= 1.0f && m > 1.0f) || (fadeTarget_ <= 0.0f && m < 0.0f)) m = fadeTarget_;
            }
            x[i] = m >= 1.0f ? y : x[i] + m * (y - x[i]);
        }
        if (ch == 0) mixEnd_ = m;
        // Runaway guard: a non-finite or huge state clears the band.
        for (int k = 0; k < ns; ++k)
            if (!(std::fabs(s[k].ic1) < 1.0e6f) || !(std::fabs(s[k].ic2) < 1.0e6f)) { for (auto& t : st_[ch]) t = SectionState(); break; }
    }
    void endChunk() { mix_ = mixEnd_; }

    int numSections() const { return nsec_; }
    const Section* sections() const { return sec_; }

private:
    bool glide(float& v, float target, float eps) const
    {
        if (v == target) return false;
        const float d = target - v;
        if (std::fabs(d) < eps) { v = target; return true; }
        v += d * glide_;
        return true;
    }
    bool needDesign() const
    {
        return curLogF_ != designedLogF_ || curGain_ != designedG_ || curLogQ_ != designedLogQ_ ||
               cur_.type != designedType_ || adaptive_ != designedAdaptive_;
    }
    void redesign()
    {
        designedLogF_ = curLogF_; designedG_ = curGain_; designedLogQ_ = curLogQ_;
        designedType_ = cur_.type; designedAdaptive_ = adaptive_;
        nsec_ = designBand(cur_.type, std::exp(curLogF_), curGain_, std::exp(curLogQ_), adaptive_, fs_, sec_);
    }

    float fs_ = 44100.0f, glide_ = 0.1f, fadeStep_ = 0.004f;
    BandParams tgt_, cur_;
    float tgtGain_ = 0.0f, tgtLogF_ = std::log(1000.0f), tgtLogQ_ = std::log(kQMid);
    bool adaptive_ = false;
    float curLogF_ = std::log(1000.0f), curGain_ = 0.0f, curLogQ_ = std::log(kQMid);
    float designedLogF_ = -1.0f, designedG_ = 0.0f, designedLogQ_ = -100.0f;
    int designedType_ = -1;
    bool designedAdaptive_ = false;
    float mix_ = 0.0f, mixStart_ = 0.0f, mixEnd_ = 0.0f, fadeTarget_ = 0.0f;
    Section sec_[4];
    SectionState st_[2][4];
    int nsec_ = 1;
};

// ---------------------------------------------------------------------------------------------------------------
// Lock-free single-producer tap: the audio thread appends the mono post-EQ signal, the screen thread copies the
// newest kFftSize samples whenever it wants a frame. Nothing waits; a reader racing a writer at worst sees a
// sample from one block later, which an analyser cannot tell.
// ---------------------------------------------------------------------------------------------------------------
class SampleTap
{
public:
    static constexpr uint32_t kSize = 16384, kMask = kSize - 1;
    void clear()
    {
        for (auto& s : buf_) s.store(0.0f, std::memory_order_relaxed);
        written_.store(0, std::memory_order_release);
    }
    void push(const float* l, const float* r, int n)
    {
        uint32_t w = written_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++w) buf_[w & kMask].store(0.5f * (l[i] + r[i]), std::memory_order_relaxed);
        written_.store(w, std::memory_order_release);
    }
    uint32_t written() const { return written_.load(std::memory_order_acquire); }
    // The kFftSize samples that end at sample count `end` (from written()).
    void copyLatest(uint32_t end, float* out) const
    {
        for (uint32_t i = 0; i < (uint32_t)kFftSize; ++i)
            out[i] = buf_[(end - (uint32_t)kFftSize + i) & kMask].load(std::memory_order_relaxed);
    }

private:
    std::atomic<float> buf_[kSize] = {};
    std::atomic<uint32_t> written_{ 0 };
};

// ---------------------------------------------------------------------------------------------------------------
// Spectrum analyser (screen thread): 2048-point Hann FFT of a frame from the tap, log-spaced columns, tilt,
// instant attack and a fall of about 30 dB/s.
// ---------------------------------------------------------------------------------------------------------------
class Analyzer
{
public:
    static constexpr float kFallDbPerSec = 30.0f;
    void prepare(float fs)
    {
        fs_ = fs;
        for (int i = 0; i < kFftSize; ++i) win_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float)i / (float)kFftSize);
        for (int i = 0; i < kFftSize / 2; ++i)
        {
            twr_[i] = std::cos(2.0f * kPi * (float)i / (float)kFftSize);
            twi_[i] = -std::sin(2.0f * kPi * (float)i / (float)kFftSize);
        }
        int bits = 0; while ((1 << bits) < kFftSize) ++bits;
        for (int i = 0; i < kFftSize; ++i)
        {
            int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[i] = (uint16_t)r;
        }
        const float binHz = fs / (float)kFftSize;
        for (int c = 0; c < kSpecCols; ++c)
        {
            const float lo = kFMin * std::pow(kFMax / kFMin, (float)c / kSpecCols);
            const float hi = kFMin * std::pow(kFMax / kFMin, (float)(c + 1) / kSpecCols);
            const float mid = std::sqrt(lo * hi);
            colLo_[c] = (int)std::ceil(lo / binHz);
            colHi_[c] = (int)std::floor(hi / binHz);
            colMid_[c] = mid / binHz;
            colTilt_[c] = kSpecTiltDb * std::log2(mid / 1000.0f);
        }
        reset();
    }
    void reset() { for (float& d : disp_) d = kSpecFloorDb; }
    // The dB values (floor .. +6, display tilt applied), one per column.
    const float* db() const { return disp_; }

    // Analyse one frame of kFftSize samples. `newSamples` is how much audio arrived since the last frame (sets the
    // fall). Columns jump up instantly and fall at kFallDbPerSec.
    void analyse(const float* frame, uint32_t newSamples)
    {
        const float fall = kFallDbPerSec * (float)newSamples / fs_;
        for (int i = 0; i < kFftSize; ++i) { re_[rev_[i]] = frame[i] * win_[i]; im_[rev_[i]] = 0.0f; }
        for (int len = 2; len <= kFftSize; len <<= 1)
        {
            const int half = len >> 1, step = kFftSize / len;
            for (int i = 0; i < kFftSize; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const float wr = twr_[j * step], wi = twi_[j * step];
                    const int a = i + j, b = a + half;
                    const float tr = re_[b] * wr - im_[b] * wi, ti = re_[b] * wi + im_[b] * wr;
                    re_[b] = re_[a] - tr; im_[b] = im_[a] - ti;
                    re_[a] += tr; im_[a] += ti;
                }
        }
        // Power, normalised so a full-scale sine reads 0 dB (Hann coherent gain 0.5: peak |X| = N/4).
        const float norm = 1.0f / ((float)kFftSize * 0.25f * (float)kFftSize * 0.25f);
        for (int k = 0; k <= kFftSize / 2; ++k) pow_[k] = (re_[k] * re_[k] + im_[k] * im_[k]) * norm;
        for (int c = 0; c < kSpecCols; ++c)
        {
            // Interpolated power at the column centre, or the loudest bin inside the column, whichever is larger.
            const float m = std::min(colMid_[c], (float)(kFftSize / 2 - 1));
            const int k0 = (int)m; const float f = m - (float)k0;
            float p = pow_[k0] + f * (pow_[k0 + 1] - pow_[k0]);
            for (int k = std::max(colLo_[c], 1); k <= std::min(colHi_[c], kFftSize / 2); ++k) p = std::max(p, pow_[k]);
            float d = 10.0f * std::log10(p + 1.0e-12f) + colTilt_[c];
            d = std::min(std::max(d, kSpecFloorDb), 6.0f);
            disp_[c] = d > disp_[c] ? d : std::max(d, disp_[c] - fall);
        }
    }

private:
    float fs_ = 44100.0f;
    float win_[kFftSize], re_[kFftSize], im_[kFftSize], pow_[kFftSize / 2 + 1];
    float twr_[kFftSize / 2], twi_[kFftSize / 2];
    uint16_t rev_[kFftSize];
    int colLo_[kSpecCols], colHi_[kSpecCols];
    float colMid_[kSpecCols], colTilt_[kSpecCols];
    float disp_[kSpecCols];
};

// ---------------------------------------------------------------------------------------------------------------
// The curve: the EQ's exact magnitude response at the screen columns, from band settings. A pure function of its
// inputs (safe on any thread). One log10 per column: the bands' squared magnitudes are multiplied first.
// ---------------------------------------------------------------------------------------------------------------
class CurveModel
{
public:
    void prepare(float fs)
    {
        fs_ = fs;
        for (int i = 0; i < kCurveCols; ++i) t_[i] = std::tan(kPi * std::min(colFreq(i, kCurveCols), 0.49f * fs) / fs);
    }
    void compute(const BandParams* p, float scale, bool adaptive, float* db) const
    {
        double m2[kCurveCols];
        for (double& v : m2) v = 1.0;
        Section sec[4];
        for (int b = 0; b < kBands; ++b)
        {
            if (!p[b].on) continue;
            const int ns = designBand(p[b].type, p[b].freq, p[b].gain * scale, p[b].q, adaptive, fs_, sec);
            for (int i = 0; i < kCurveCols; ++i)
                for (int k = 0; k < ns; ++k) m2[i] *= (double)sec[k].magSq(t_[i]);
        }
        for (int i = 0; i < kCurveCols; ++i) db[i] = (float)(10.0 * std::log10(std::max(m2[i], 1.0e-30)));
    }
    float at(const BandParams* p, float scale, bool adaptive, float freq) const
    {
        const float t = std::tan(kPi * std::min(freq, 0.49f * fs_) / fs_);
        double m2 = 1.0;
        Section sec[4];
        for (int b = 0; b < kBands; ++b)
        {
            if (!p[b].on) continue;
            const int ns = designBand(p[b].type, p[b].freq, p[b].gain * scale, p[b].q, adaptive, fs_, sec);
            for (int k = 0; k < ns; ++k) m2 *= (double)sec[k].magSq(t);
        }
        return (float)(10.0 * std::log10(std::max(m2, 1.0e-30)));
    }

private:
    float fs_ = 44100.0f;
    float t_[kCurveCols] = {};
};

// ---------------------------------------------------------------------------------------------------------------
// The whole EQ.
// ---------------------------------------------------------------------------------------------------------------
class Engine
{
public:
    void prepare(float fs)
    {
        fs_ = fs;
        for (auto& b : bands_) b.prepare(fs);
        curve_.prepare(fs);
        gainStep_ = 1.0f - std::exp(-1.0f / (0.015f * fs));
        reset();
    }
    void reset()
    {
        for (auto& b : bands_) b.reset();
        tap_.clear();
        outGain_ = tgtOut_; mix_ = tgtMix_;
    }
    void setGlobal(float outDb, float scale, float mixPct, bool adaptiveQ, bool analyzerOn)
    {
        if (outDb != outDb_) { outDb_ = outDb; tgtOut_ = std::pow(10.0f, outDb / 20.0f); }
        scale_ = scale; adaptive_ = adaptiveQ;
        tgtMix_ = std::min(std::max(mixPct * 0.01f, 0.0f), 1.0f);
        analyzerOn_ = analyzerOn;
    }
    void setBand(int b, const BandParams& p) { params_[b] = p; bands_[b].setTarget(p, scale_, adaptive_); }

    // In place is fine (each sample is read before it is written). outR may be null for mono.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        int done = 0;
        while (done < n)
        {
            const int m = std::min(kChunk, n - done);
            float l[kChunk], r[kChunk], dl[kChunk], dr[kChunk];
            for (int i = 0; i < m; ++i)
            {
                dl[i] = l[i] = inL[done + i];
                dr[i] = r[i] = inR ? inR[done + i] : l[i];
            }
            for (auto& b : bands_)
            {
                if (!b.beginChunk(m)) continue;
                b.processChannel(0, l, m);
                b.processChannel(1, r, m);
                b.endChunk();
            }
            for (int i = 0; i < m; ++i)
            {
                outGain_ += gainStep_ * (tgtOut_ - outGain_);
                mix_ += gainStep_ * (tgtMix_ - mix_);
                const float yl = (dl[i] + mix_ * (l[i] - dl[i])) * outGain_;
                const float yr = (dr[i] + mix_ * (r[i] - dr[i])) * outGain_;
                l[i] = yl; r[i] = yr;
                outL[done + i] = yl;
                if (outR) outR[done + i] = yr;
            }
            if (analyzerOn_) tap_.push(l, r, m);
            done += m;
        }
    }

    // Magnitude response in dB at the curve columns, from the target settings (the curve jumps straight to where
    // the audio is gliding). The plugin computes the same thing on its screen thread with its own CurveModel.
    void curveDb(float* db) const { curve_.compute(params_, scale_, adaptive_, db); }
    float responseDbAt(float freq) const { return curve_.at(params_, scale_, adaptive_, freq); }   // any frequency (tests)

    SampleTap& tap() { return tap_; }
    bool analyzerOn() const { return analyzerOn_; }
    float sampleRate() const { return fs_; }

private:
    float fs_ = 44100.0f;
    Band bands_[kBands];
    BandParams params_[kBands];
    CurveModel curve_;
    SampleTap tap_;
    float scale_ = 1.0f;
    bool adaptive_ = false, analyzerOn_ = true;
    float outDb_ = 0.0f;
    float tgtOut_ = 1.0f, outGain_ = 1.0f, tgtMix_ = 1.0f, mix_ = 1.0f, gainStep_ = 0.001f;
};

}  // namespace teq

#endif
