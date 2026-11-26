#pragma once
// IR Fit Reverb (0.3.0): the hybrid - the IR itself next to its ALGO model.
//  The early part (ER) and the tail each play the ALGO model, the IR (convolution), or a blend of both (equal power).
//  The IR is the one the model was fitted to (48 kHz, dry removed, at the plug-in's reference wet energy), or another IR
//  loaded into the ER slot (A) or the tail slot (B). Its edits (start, attack, length, stretch, decay per band, reverse) are
//  rendered offline on a worker thread; the convolution cross-fades to each new IR by itself (JUCE). Level, width, the
//  cuts and the pre-delay act in real time.
//  With both blends at 0 nothing here runs: the plug-in is the 0.2.x one, sample for sample.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "IrFit.h"
#include <thread>
#include <cstring>

namespace irfit
{
// ---------------------------------------------------------------------------------------------------------------------
//  an IR at 48 kHz, stereo, its energy at the reference; measured once (onset, decay per band)
// ---------------------------------------------------------------------------------------------------------------------
struct IRData
{
    std::vector<float> L, R;
    juce::String name;
    float onsetMs = 0;                    // the first sample within 30 dB of the peak
    std::array<float, 3> t60 { 2, 2, 2 }; // low (< 250 Hz) / mid / high (> 4 kHz), from the onset (T20 x 3)
    bool valid() const { return L.size() > 480 && L.size() == R.size(); }
    float midT() const { return t60[1]; }
};
using IRPtr = std::shared_ptr<const IRData>;

namespace hyb
{
constexpr double kSR = 48000.0;
inline juce::String bitsOf (float v) { uint32_t u; std::memcpy (&u, &v, 4); return juce::String::toHexString ((juce::int64) u).paddedLeft ('0', 8); }
inline float fromBits (const juce::String& b) { const uint32_t u = (uint32_t) b.getHexValue64(); float v; std::memcpy (&v, &u, 4); return v; }
constexpr float kLoX = 250.0f, kHiX = 4000.0f;       // the band edges of the decay edits

inline double energyDb (const std::vector<float>& L, const std::vector<float>& R)
{
    double e = 0; for (size_t i = 0; i < L.size(); ++i) e += 0.5 * ((double) L[i] * L[i] + (double) R[i] * R[i]);
    return 10 * std::log10 (e + 1e-30);
}
inline int onsetIdx (const std::vector<float>& L, const std::vector<float>& R, double db = 30)
{
    double pk = 0; for (size_t i = 0; i < L.size(); ++i) pk = std::max (pk, (double) L[i] * L[i] + (double) R[i] * R[i]);
    const double th = pk * std::pow (10.0, -db / 10);
    for (size_t i = 0; i < L.size(); ++i) if ((double) L[i] * L[i] + (double) R[i] * R[i] >= th) return (int) i;
    return 0;
}
// 2nd-order Butterworth, run forward and backward (zero phase, 4th-order magnitude): low + mid + high = x exactly
struct Bq2 { double b0, b1, b2, a1, a2; };
inline Bq2 butter (bool high, double fc, double fs)
{
    const double w = 2 * juce::MathConstants<double>::pi * fc / fs, c = std::cos (w), al = std::sin (w) / std::sqrt (2.0), a0 = 1 + al;
    if (high) return { (1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0 };
    return { (1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0 };
}
inline void filtfilt (const Bq2& q, const float* x, float* y, int n)
{
    double z1 = 0, z2 = 0;
    std::vector<double> t ((size_t) n);
    for (int i = 0; i < n; ++i) { const double v = x[i], o = q.b0 * v + z1; z1 = q.b1 * v - q.a1 * o + z2; z2 = q.b2 * v - q.a2 * o; t[(size_t) i] = o; }
    z1 = z2 = 0;
    for (int i = n; i-- > 0;) { const double v = t[(size_t) i], o = q.b0 * v + z1; z1 = q.b1 * v - q.a1 * o + z2; z2 = q.b2 * v - q.a2 * o; y[i] = (float) o; }
}
// x -> low / mid / high (each n samples)
inline void split3 (const float* x, int n, std::vector<float>& lo, std::vector<float>& mid, std::vector<float>& hi)
{
    lo.resize ((size_t) n); hi.resize ((size_t) n); mid.resize ((size_t) n);
    filtfilt (butter (false, kLoX, kSR), x, lo.data(), n);
    filtfilt (butter (true, kHiX, kSR), x, hi.data(), n);
    for (int i = 0; i < n; ++i) mid[(size_t) i] = x[i] - lo[(size_t) i] - hi[(size_t) i];
}
// T60 from the Schroeder curve (-5 .. -35 dB x 2, or -5 .. -25 dB x 3 when the IR is too short / noisy), from sample o on
inline float t60Of (const std::vector<double>& e, int o)
{
    const int n = (int) e.size(); if (n - o < 480) return 1.0f;
    std::vector<double> s ((size_t) (n - o)); double acc = 0;
    for (int i = n; i-- > o;) { acc += e[(size_t) i]; s[(size_t) (i - o)] = acc; }
    const double tot = s[0] + 1e-30; int i5 = -1, i25 = -1, i35 = -1;
    for (int i = 0; i < (int) s.size(); ++i)
    {
        const double d = 10 * std::log10 (s[(size_t) i] / tot + 1e-30);
        if (i5 < 0 && d <= -5) i5 = i;
        if (i25 < 0 && d <= -25) i25 = i;
        if (d <= -35) { i35 = i; break; }
    }
    // (the last 10 % of the curve bends down with the IR's end: not used)
    if (i35 > i5 && i5 >= 0 && i35 < (int) (0.9 * s.size())) return (float) juce::jlimit (0.05, 30.0, 2.0 * (i35 - i5) / kSR);
    if (i25 > i5 && i5 >= 0) return (float) juce::jlimit (0.05, 30.0, 3.0 * (i25 - i5) / kSR);
    return (float) juce::jlimit (0.1, 30.0, 3.0 * (n - o) / kSR);
}
}

// the measures of an IR (once, on load)
inline void analyseIR (IRData& d)
{
    const int n = (int) d.L.size(); if (n < 480) return;
    const int o = hyb::onsetIdx (d.L, d.R);
    d.onsetMs = (float) (o / 48.0);
    std::vector<float> lo, mid, hi, lo2, mid2, hi2;
    hyb::split3 (d.L.data(), n, lo, mid, hi); hyb::split3 (d.R.data(), n, lo2, mid2, hi2);
    std::vector<double> e ((size_t) n);
    auto band = [&] (const std::vector<float>& a, const std::vector<float>& b)
    {   // T30 as the fit measures it (noise floor out), from the onset; T20 / the curve's own if that fails
        std::vector<double> eb ((size_t) (n - o));
        for (int i = o; i < n; ++i) eb[(size_t) (i - o)] = (double) a[(size_t) i] * a[(size_t) i] + (double) b[(size_t) i] * b[(size_t) i];
        const auto d = fit::edcDb (eb, hyb::kSR);
        double t = fit::fitTime (d, -5, -35, hyb::kSR); if (! std::isfinite (t)) t = fit::fitTime (d, -5, -25, hyb::kSR);
        if (std::isfinite (t) && t > 0.05) return (float) std::min (30.0, t);
        for (int i = 0; i < n; ++i) e[(size_t) i] = 0.5 * ((double) a[(size_t) i] * a[(size_t) i] + (double) b[(size_t) i] * b[(size_t) i]);
        return hyb::t60Of (e, o); };
    d.t60 = { band (lo, lo2), band (mid, mid2), band (hi, hi2) };
}
// to the reference wet energy (as the ALGO models are)
inline void normaliseIR (IRData& d, double refDb)
{
    const float g = (float) std::pow (10.0, (refDb - hyb::energyDb (d.L, d.R)) / 20);
    for (auto& x : d.L) x *= g; for (auto& x : d.R) x *= g;
}
juce::String prepareStereoIR (const juce::File& in, const juce::File& outWav, juce::String& displayName, int* channelsOut, double* lengthSOut);   // (IRFitConvert)
// any IR file (mono / stereo / true stereo / an _L + _R pair, any rate) -> IRData, as the converter reads it. "" = ok
inline juce::String loadIRFile (const juce::File& f, IRData& out, double refDb)
{
    out = IRData();
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("IRFitReverb-slot-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()) + ".wav");
    juce::String name;
    if (auto err = prepareStereoIR (f, tmp, name, nullptr, nullptr); err.isNotEmpty()) { tmp.deleteFile(); return err; }
    std::vector<float> L, R;
    const bool ok = fit::loadTarget (tmp, {}, L, R);
    tmp.deleteFile();
    if (! ok || L.size() < 480) return "Cannot read this IR (wav / aif, at least 10 ms)";
    out.L = std::move (L); out.R = std::move (R); out.name = name;
    normaliseIR (out, refDb); analyseIR (out);
    return {};
}

// embedded in a session / preset / model: 24-bit FLAC (scaled to its peak), base64
inline juce::String encodeIR (const IRData& d)
{
    if (! d.valid()) return {};
    float pk = 1e-9f; for (size_t i = 0; i < d.L.size(); ++i) pk = std::max ({ pk, std::abs (d.L[i]), std::abs (d.R[i]) });
    juce::AudioBuffer<float> b (2, (int) d.L.size());
    for (int i = 0; i < b.getNumSamples(); ++i) { b.setSample (0, i, d.L[(size_t) i] / pk * 0.999f); b.setSample (1, i, d.R[(size_t) i] / pk * 0.999f); }
    juce::MemoryBlock mb;
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::MemoryOutputStream> (mb, false);
        auto o = juce::AudioFormatWriterOptions().withSampleRate (hyb::kSR).withNumChannels (2).withBitsPerSample (24);
        auto w = flac.createWriterFor (os, o); if (w == nullptr) return {};
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
    return hyb::bitsOf (pk / 0.999f) + ":" + mb.toBase64Encoding();
}
inline bool decodeIR (const juce::String& s, IRData& d)
{
    const auto sc = s.upToFirstOccurrenceOf (":", false, false), data = s.fromFirstOccurrenceOf (":", false, false);
    if (sc.length() != 8 || data.isEmpty()) return false;
    const float pk = hyb::fromBits (sc);
    juce::MemoryBlock mb; if (! mb.fromBase64Encoding (data)) return false;
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> rd (flac.createReaderFor (new juce::MemoryInputStream (mb, false), true));
    if (rd == nullptr || rd->numChannels < 1 || rd->lengthInSamples < 480) return false;
    juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples); rd->read (&b, 0, b.getNumSamples(), 0, true, true);
    d.L.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    d.R.assign (b.getReadPointer (1), b.getReadPointer (1) + b.getNumSamples());
    for (auto& x : d.L) x *= pk; for (auto& x : d.R) x *= pk;
    analyseIR (d);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
//  what an IR is rendered with (everything that needs a new IR; the rest acts in real time)
// ---------------------------------------------------------------------------------------------------------------------
struct BuildSpec
{
    float splitMs = 80;                  // the ER / tail split, after the onset (a 10 ms cross-over)
    float startMs = 0;                   // cut from the start (moves the IR earlier)
    float attackMs = 0;                  // a fade-in from the onset
    float lengthPct = 100;               // the IR's end (a fade over its last 10 %)
    float stretch = 1;                   // the ER's time (0.5 .. 2) and the tail's start
    std::array<float, 3> decay { 1, 1, 1 };   // the decay time per band x (after the stretch)
    bool reverse = false;                // the tail backwards
    float tailK = 1;                     // the tail's decay x (IR Size)
    bool operator== (const BuildSpec& o) const
    {
        return tailK == o.tailK && splitMs == o.splitMs && startMs == o.startMs && attackMs == o.attackMs && lengthPct == o.lengthPct && stretch == o.stretch
            && decay == o.decay && reverse == o.reverse;
    }
    bool operator!= (const BuildSpec& o) const { return ! (*this == o); }
};

// one part (ER: er = true, or the tail) of an IR with the edits, 48 kHz stereo
inline void buildPart (const IRData& src, const BuildSpec& s, bool er, juce::AudioBuffer<float>& out)
{
    const int n0 = (int) src.L.size();
    const int cut = juce::jlimit (0, std::max (0, n0 - 480), (int) std::lround (s.startMs * 48.0));
    std::vector<float> L (src.L.begin() + cut, src.L.end()), R (src.R.begin() + cut, src.R.end());
    const double st = juce::jlimit (0.5, 2.0, (double) s.stretch);
    int n = (int) L.size();
    const int on = juce::jlimit (0, n - 1, (int) std::lround (src.onsetMs * 48.0 - cut));
    // the decay per band: the band's own decay curve (Schroeder, dB from the onset) scaled by 1 / k - every decay time x k,
    // whatever the curve's shape (two slopes, a noise floor); at most +30 dB
    if (s.decay[0] != 1 || s.decay[1] != 1 || s.decay[2] != 1 || (! er && s.tailK != 1))
    {
        std::array<std::vector<float>, 3> bl, br;
        hyb::split3 (L.data(), n, bl[0], bl[1], bl[2]); hyb::split3 (R.data(), n, br[0], br[1], br[2]);
        std::fill (L.begin(), L.end(), 0.0f); std::fill (R.begin(), R.end(), 0.0f);
        for (int b = 0; b < 3; ++b)
        {
            const double k = juce::jlimit (0.1, 10.0, (double) s.decay[(size_t) b] * (er ? 1.0 : (double) s.tailK));
            auto& xl = bl[(size_t) b]; auto& xr = br[(size_t) b];
            if (k == 1.0) { for (int i = 0; i < n; ++i) { L[(size_t) i] += xl[(size_t) i]; R[(size_t) i] += xr[(size_t) i]; } continue; }
            // the band's level curve stretched in time by k around the onset: the gain makes the level at t what it was at
            // on + (t - on) / k (20 ms average energy) - EDT, T30 and every drop take k times as long, whatever the curve's shape;
            // past the IR's own floor its noise is shaped into the longer decay (at most +30 dB)
            std::vector<double> lv ((size_t) n); { double acc = 0; const int w = 960;
                std::vector<double> en ((size_t) n);
                for (int i = 0; i < n; ++i) en[(size_t) i] = (double) xl[(size_t) i] * xl[(size_t) i] + (double) xr[(size_t) i] * xr[(size_t) i];
                for (int i = 0; i < n + w / 2; ++i)
                {
                    if (i < n) acc += en[(size_t) i];
                    if (i - w >= 0) acc -= en[(size_t) (i - w)];
                    const int c = i - w / 2; if (c >= 0 && c < n) lv[(size_t) c] = 10 * std::log10 (std::max (0.0, acc) / std::min (w, std::min (i + 1, n - std::max (0, i - w))) + 1e-30);
                } }
            auto lvAt = [&] (double x) { if (x >= n - 1) return lv[(size_t) (n - 1)] - 60.0 * (x - (n - 1)) / (0.5 * hyb::kSR); const int i = (int) x; const double f = x - i; return lv[(size_t) i] + f * (lv[(size_t) i + 1] - lv[(size_t) i]); };
            for (int i = 0; i < n; ++i)
            {
                double g = 1;
                if (i > on) g = std::pow (10.0, juce::jlimit (-120.0, 30.0, lvAt (on + (i - on) / k) - lv[(size_t) i]) / 20.0);
                L[(size_t) i] += xl[(size_t) i] * (float) g; R[(size_t) i] += xr[(size_t) i] * (float) g;
            }
        }
    }
    // the size: the ER's reflections further apart (resampled around the onset: t' = on + (t - on) s), the tail starts later by
    // as much (its decay is the Decay's; IR Size lengthens it through tailK)
    if (std::abs (st - 1.0) > 1e-4)
    {
        if (er)
        {
            const int m = std::max (on + 2, (int) std::lround (on + (n - on) * st));
            auto rs = [&] (const std::vector<float>& x) {
                std::vector<float> y ((size_t) m);
                auto at = [&] (int i) { return i < 0 || i >= n ? 0.0f : x[(size_t) i]; };
                for (int j = 0; j < m; ++j)
                {
                    const double p = j <= on ? j : on + (j - on) / st; const int i = (int) std::floor (p); const float t = (float) (p - i);
                    const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
                    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                    y[(size_t) j] = ((c3 * t + c2) * t + c1) * t + y1;
                }
                const float g = (float) (1.0 / std::sqrt (st)); for (int j = on; j < m; ++j) y[(size_t) j] *= g;     // (the energy per second kept)
                return y; };
            L = rs (L); R = rs (R);
        }
        else
        {
            const int sh = (int) std::lround ((st - 1.0) * s.splitMs * 48.0);
            auto mv = [&] (std::vector<float>& x) { if (sh > 0) x.insert (x.begin() + on, (size_t) sh, 0.0f); else x.erase (x.begin() + on, x.begin() + std::min ((int) x.size(), on - sh)); };
            mv (L); mv (R);
        }
        n = (int) L.size();
    }
    // the attack (a fade-in from the onset)
    if (s.attackMs > 0.05f)
    {
        const int a = std::max (1, (int) std::lround (s.attackMs * 48.0));
        for (int i = 0; i < n; ++i)
        {
            const int t = i - on; const float g = t <= 0 ? (i < on ? 1.0f : 0.0f) : t >= a ? 1.0f : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * t / a);
            if (i >= on) { L[(size_t) i] *= g; R[(size_t) i] *= g; }
        }
    }
    // the length (a fade over the last 10 % of what is kept)
    int len = n;
    if (s.lengthPct < 99.95f)
    {
        len = juce::jlimit (on + 480, n, on + (int) std::lround ((n - on) * juce::jlimit (0.02f, 1.0f, s.lengthPct / 100.0f)));
        const int f = std::max (48, (len - on) / 10);
        for (int i = std::max (0, len - f); i < len; ++i) { const float g = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (i - (len - f)) / f); L[(size_t) i] *= g; R[(size_t) i] *= g; }
    }
    // the split: ER = [0, split + 5 ms) fading out over 10 ms, the tail the rest (the two add up to the whole)
    const int sp = juce::jlimit (48, std::max (49, len - 48), on + (int) std::lround (s.splitMs * 48.0 * st)), xf = 480, a0 = std::max (0, sp - xf / 2);
    auto wEr = [&] (int i) { return i < a0 ? 1.0f : i >= a0 + xf ? 0.0f : 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (i - a0) / xf); };
    if (er)
    {
        const int m = std::min (len, a0 + xf);
        out.setSize (2, std::max (1, m)); out.clear();
        for (int i = 0; i < m; ++i) { const float g = wEr (i); out.setSample (0, i, L[(size_t) i] * g); out.setSample (1, i, R[(size_t) i] * g); }
        return;
    }
    out.setSize (2, std::max (1, len)); out.clear();
    for (int i = a0; i < len; ++i) { const float g = 1 - wEr (i); out.setSample (0, i, L[(size_t) i] * g); out.setSample (1, i, R[(size_t) i] * g); }
    if (s.reverse)
    {   // backwards from its start: [a0, len) reversed, with a 10 ms fade at the turn
        for (int c = 0; c < 2; ++c) { auto* d = out.getWritePointer (c); std::reverse (d + a0, d + len); }
        for (int i = 0; i < xf && a0 + i < len; ++i) { const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * i / xf); out.getWritePointer (0)[a0 + i] *= g; out.getWritePointer (1)[a0 + i] *= g; }
        for (int i = 0; i < xf && len - 1 - i > a0; ++i) { const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * i / xf); out.getWritePointer (0)[len - 1 - i] *= g; out.getWritePointer (1)[len - 1 - i] *= g; }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
//  the convolution: one input history shared by every IR (zero latency, non-uniform partitions)
//  taps [0, 64) direct; then partitions of 64 from 64, 512 from 1024, 4096 from 8192 (overlap-save, the input spectra kept).
//  A new IR is computed over the same input history, so it plays its whole tail at once: the old and the new outputs
//  cross-fade (85 ms) with nothing lost - an edit while the reverb rings is heard as the edit, not as a restart.
//  Each IR set holds two parts (ER, tail), stereo, for the mono input.
// ---------------------------------------------------------------------------------------------------------------------
namespace sc
{
constexpr int kFir = 64, kStages = 3;
constexpr int kP[kStages] = { 64, 512, 4096 }, kO[kStages] = { 64, 1024, 8192 }, kOrd[kStages] = { 7, 10, 13 };      // (FFT size 2P)
constexpr int kNmax01[2] = { (1024 - 64) / 64, (8192 - 1024) / 512 };
inline int partitions (int s, int len) { if (len <= kO[s]) return 0; const int n = (len - kO[s] + kP[s] - 1) / kP[s]; return s < 2 ? std::min (n, kNmax01[s]) : n; }
}

struct PartFilter
{
    int len = 0, id = 0;
    std::array<std::array<float, sc::kFir>, 2> fir {};
    struct St { int N = 0; std::array<std::vector<float>, 2> H; };     // N x 2 (P + 1) floats (complex bins 0 .. P)
    std::array<St, sc::kStages> st;
};
using PartPtr = std::shared_ptr<const PartFilter>;
struct FilterSet { std::array<PartPtr, 2> part; };

// a stereo IR at the host rate -> its partitions (worker thread)
inline PartPtr makePartFilter (const juce::AudioBuffer<float>& ir, int id)
{
    auto f = std::make_shared<PartFilter>(); f->len = ir.getNumSamples(); f->id = id;
    for (int c = 0; c < 2; ++c) for (int k = 0; k < sc::kFir && k < f->len; ++k) f->fir[(size_t) c][(size_t) k] = ir.getSample (c, k);
    for (int s = 0; s < sc::kStages; ++s)
    {
        const int P = sc::kP[s], N = sc::partitions (s, f->len); auto& st = f->st[(size_t) s]; st.N = N;
        if (N == 0) continue;
        juce::dsp::FFT fft (sc::kOrd[s]); std::vector<float> buf ((size_t) (4 * P));
        for (int c = 0; c < 2; ++c)
        {
            st.H[(size_t) c].assign ((size_t) N * (size_t) (2 * (P + 1)), 0.0f);
            const float* x = ir.getReadPointer (c);
            for (int j = 0; j < N; ++j)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                const int a = sc::kO[s] + j * P;
                for (int i = 0; i < P && a + i < f->len; ++i) buf[(size_t) i] = x[a + i];
                fft.performRealOnlyForwardTransform (buf.data(), true);
                std::copy (buf.begin(), buf.begin() + 2 * (P + 1), st.H[(size_t) c].begin() + (ptrdiff_t) j * 2 * (P + 1));
            }
        }
    }
    return f;
}

// windowed-sinc resampling (an IR from 48 kHz to the host rate; worker thread)
inline void resampleIR (juce::AudioBuffer<float>& b, double from, double to)
{
    if (std::abs (from - to) < 0.5) return;
    const double r = to / from; const int n = b.getNumSamples(), m = std::max (1, (int) std::lround (n * r));
    const double fc = std::min (1.0, r) * 0.475; const int H = 24;
    juce::AudioBuffer<float> o (2, m);
    for (int c = 0; c < 2; ++c)
    {
        const float* x = b.getReadPointer (c); float* y = o.getWritePointer (c);
        for (int j = 0; j < m; ++j)
        {
            const double p = j / r; const int i0 = (int) std::floor (p); double acc = 0;
            for (int k = i0 - H + 1; k <= i0 + H; ++k)
            {
                if (k < 0 || k >= n) continue;
                const double t = p - k, w = 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * t / H) + 0.08 * std::cos (2 * juce::MathConstants<double>::pi * t / H);
                const double sn = std::abs (t) < 1e-9 ? 2 * fc : std::sin (2 * juce::MathConstants<double>::pi * fc * t) / (juce::MathConstants<double>::pi * t);
                acc += x[k] * sn * (std::abs (t) < H ? w : 0.0);
            }
            y[j] = (float) acc;
        }
    }
    b = std::move (o);
}

class SharedConv
{
public:
    // maxLen: the longest IR (samples at the host rate)
    void prepare (int maxLen)
    {
        for (int s = 0; s < sc::kStages; ++s)
        {
            auto& S = st[(size_t) s]; const int P = sc::kP[s];
            S.fft = std::make_unique<juce::dsp::FFT> (sc::kOrd[s]);
            S.nmax = std::max (1, s < 2 ? sc::kNmax01[s] : sc::partitions (s, maxLen));
            S.fdl.assign ((size_t) S.nmax * (size_t) (2 * (P + 1)), 0.0f);
            S.win.assign ((size_t) (2 * P), 0.0f); S.buf.assign ((size_t) (4 * P), 0.0f);
            for (auto& y : S.Y) y.assign ((size_t) (4 * P), 0.0f);
        }
        for (auto& r : ring) r.assign ((size_t) kRing, 0.0f);
        slots[0].reset(); slots[1].reset(); a = 0; retired.reset(); retiredFull = false;
        reset();
    }
    void reset()
    {
        for (auto& S : st) { std::fill (S.fdl.begin(), S.fdl.end(), 0.0f); std::fill (S.win.begin(), S.win.end(), 0.0f); S.job = false; }
        for (auto& r : ring) std::fill (r.begin(), r.end(), 0.0f);
        hist.fill (0.0f); t = 0; blocks.fill (0); warmAt = { 0, 0 };
        if (slots[a ^ 1])
        {   if (! retiredFull) { retired = std::move (slots[a]); retiredFull = true; a ^= 1; fadeStart = -1; }
            else fadeStart = t; }                                            // (the old one goes once the retired slot is free)
        else fadeStart = -1;
    }
    // (audio thread) a new IR set; false: a cross-fade still runs (try again later). The old one goes to retired (freed by the message thread)
    bool install (std::unique_ptr<FilterSet>& s)
    {
        if (slots[a ^ 1] || retiredFull) return false;
        if (! slots[a]) { slots[a] = std::move (s); warmAt = { t + kLead, t + kLead }; return true; }
        slots[a ^ 1] = std::move (s); fadeStart = t + kLead; return true;
    }
    // (message thread) frees a retired set
    void collect() { if (retiredFull.load()) { retired.reset(); retiredFull = false; } }
    const FilterSet* newest() const { return slots[a ^ 1] ? slots[a ^ 1].get() : slots[a].get(); }
    std::array<bool, 2> enabled { true, true };
    // a part switched on: its long partitions have their whole history after kLead samples
    void enable (int p, bool on) { if (on && ! enabled[(size_t) p]) warmAt[(size_t) p] = t + kLead; enabled[(size_t) p] = on; }
    bool warm (int p) const { return enabled[(size_t) p] && t >= warmAt[(size_t) p]; }
    // mono in -> per part stereo out (er[0..1], tl[0..1], n samples)
    void process (const float* x, float* const* er, float* const* tl, int n)
    {
        float* out[2][2] = { { er[0], er[1] }, { tl[0], tl[1] } };
        for (int i = 0; i < n; ++i)
        {
            const float v = x[i];
            hist[(size_t) (t & (kHist - 1))] = v;
            for (int s = 0; s < sc::kStages; ++s) st[(size_t) s].win[(size_t) (sc::kP[s] + (int) (t % sc::kP[s]))] = v;
            // the gains of the two sets
            float gB = 0.0f; const bool fading = slots[a ^ 1] != nullptr;
            if (fading) { const double u = (double) (t - fadeStart) / kFade; gB = u <= 0 ? 0.0f : u >= 1 ? 1.0f : (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u)); }
            const FilterSet* sets[2] = { slots[0].get(), slots[1].get() }; float g[2]; g[a] = 1.0f - gB; g[a ^ 1] = gB;
            const size_t ri = (size_t) (t & (kRing - 1));
            for (int p = 0; p < 2; ++p)
                for (int c = 0; c < 2; ++c)
                {
                    float y = 0;
                    for (int k = 0; k < 2; ++k)
                    {
                        auto& rg = ring[(size_t) (k * 4 + p * 2 + c)]; const float rv = rg[ri]; rg[ri] = 0.0f;
                        if (sets[k] == nullptr || g[k] == 0.0f) continue;
                        const auto& pf = sets[k]->part[(size_t) p]; if (! pf || ! enabled[(size_t) p]) continue;
                        const auto& h = pf->fir[(size_t) c]; float a = rv;
                        for (int q = 0; q < sc::kFir; ++q) a += h[(size_t) q] * hist[(size_t) ((t - q) & (kHist - 1))];
                        y += g[k] * a;
                    }
                    out[p][c][i] = y;
                }
            ++t;
            // block boundaries
            if ((t & 63) == 0)
            {
                for (int s = 0; s < sc::kStages; ++s) if (t % sc::kP[s] == 0) completeBlock (s);
                for (int s = 1; s < sc::kStages; ++s) work (s, false);
                // the cross-fade done: the old set out (after its last jobs)
                if (slots[a ^ 1] && t >= fadeStart + kFade + kLead && ! retiredFull) { retired = std::move (slots[a]); retiredFull = true; a ^= 1; fadeStart = -1; }
            }
        }
    }
private:
    static constexpr int kRing = 32768, kHist = 128, kLead = 8192 + 64, kFade = 4096;
    struct Stage
    {
        std::unique_ptr<juce::dsp::FFT> fft; int nmax = 1;
        std::vector<float> fdl, win, buf; std::array<std::vector<float>, 8> Y;
        bool job = false; long long b = 0, doneAt = 0; int unit = 0, units = 0; long long started = 0;
        struct U { const PartFilter* f; int set, part, ch; };
        std::array<U, 8> list {}; int nl = 0;
    };
    void completeBlock (int s)
    {
        auto& S = st[(size_t) s]; const int P = sc::kP[s];
        if (S.job) work (s, true);                                        // (never left behind)
        std::fill (S.buf.begin(), S.buf.end(), 0.0f); std::copy (S.win.begin(), S.win.end(), S.buf.begin());
        S.fft->performRealOnlyForwardTransform (S.buf.data(), true);
        const long long b = blocks[(size_t) s]++;
        std::copy (S.buf.begin(), S.buf.begin() + 2 * (P + 1), S.fdl.begin() + (ptrdiff_t) ((b % S.nmax) * 2 * (P + 1)));
        std::copy (S.win.begin() + P, S.win.end(), S.win.begin());
        // the job: every (set, part, channel) that plays
        S.nl = 0; S.b = b;
        const FilterSet* sets[2] = { slots[0].get(), slots[1].get() };
        for (int k = 0; k < 2; ++k)
        {
            if (sets[k] == nullptr) continue;
            if (k == a && slots[a ^ 1] && t >= fadeStart + kFade) continue;          // (the old set: silent from here on)
            for (int p = 0; p < 2; ++p)
            {
                const auto& pf = sets[k]->part[(size_t) p];
                if (! pf || ! enabled[(size_t) p] || pf->st[(size_t) s].N == 0) continue;
                for (int c = 0; c < 2; ++c) S.list[(size_t) S.nl++] = { pf.get(), k, p, c };
            }
        }
        if (S.nl == 0) return;
        for (int u = 0; u < S.nl; ++u) std::fill (S.Y[(size_t) u].begin(), S.Y[(size_t) u].begin() + 2 * (P + 1), 0.0f);
        S.job = true; S.unit = 0; S.started = t; S.units = 0;
        for (int u = 0; u < S.nl; ++u) S.units += std::min (S.list[(size_t) u].f->st[(size_t) s].N, S.nmax);
        if (s == 0) work (0, true);                                        // (64: needed at once)
    }
    // spread a stage's multiply-adds over its slack (P - 64 samples), then the inverse FFTs
    void work (int s, bool all)
    {
        auto& S = st[(size_t) s]; if (! S.job) return;
        const int P = sc::kP[s]; const long long el = t - S.started;
        const bool last = all || el >= P - 64;
        const int target = last ? S.units : (int) ((long long) S.units * el / std::max (1, P - 64));
        const int bins = P + 1;
        while (S.unit < target)
        {
            // unit -> (list entry, partition)
            int u = 0, j = S.unit;
            while (u < S.nl && j >= std::min (S.list[(size_t) u].f->st[(size_t) s].N, S.nmax)) { j -= std::min (S.list[(size_t) u].f->st[(size_t) s].N, S.nmax); ++u; }
            const auto& L = S.list[(size_t) u];
            const float* H = L.f->st[(size_t) s].H[(size_t) L.ch].data() + (ptrdiff_t) j * 2 * bins;
            const float* X = S.fdl.data() + (ptrdiff_t) (((S.b - j) % S.nmax + S.nmax) % S.nmax) * 2 * bins;
            float* Y = S.Y[(size_t) u].data();
            if (S.b - j >= 0)
                for (int q = 0; q < bins; ++q)
                {
                    const float xr = X[2 * q], xi = X[2 * q + 1], hr = H[2 * q], hi = H[2 * q + 1];
                    Y[2 * q] += xr * hr - xi * hi; Y[2 * q + 1] += xr * hi + xi * hr;
                }
            ++S.unit;
        }
        if (! last) return;
        // the outputs: block b plays at (b + d) P
        const int d = sc::kO[s] / P; const long long at = (S.b + d) * (long long) P;
        for (int u = 0; u < S.nl; ++u)
        {
            auto& Y = S.Y[(size_t) u]; const auto& L = S.list[(size_t) u];
            S.fft->performRealOnlyInverseTransform (Y.data());
            auto& rg = ring[(size_t) (L.set * 4 + L.part * 2 + L.ch)];
            for (int i = 0; i < P; ++i) rg[(size_t) ((at + i) & (kRing - 1))] += Y[(size_t) (P + i)];
        }
        S.job = false;
    }
    std::array<Stage, sc::kStages> st;
    std::array<std::vector<float>, 8> ring;                 // [set][part][ch]
    std::array<float, kHist> hist {};
    std::array<long long, sc::kStages> blocks {};
    long long t = 0, fadeStart = -1; std::array<long long, 2> warmAt { 0, 0 };
    std::array<std::unique_ptr<FilterSet>, 2> slots; int a = 0;          // slots[a] plays, slots[a ^ 1] fades in
    std::unique_ptr<FilterSet> retired; std::atomic<bool> retiredFull { false };
};

// ---------------------------------------------------------------------------------------------------------------------
//  real-time: the convolution of the mono input, the pre-delay, level, width and cuts
// ---------------------------------------------------------------------------------------------------------------------
struct LiveSpec
{
    float erG = 0, tailG = 0;              // linear, incl. the blend (0 = the part is off)
    float shiftMs = 0;                     // >= 0: the input delayed (a negative shift is cut from the IR: BuildSpec.startMs)
    float width = 1;                       // M / S
    float lowCut = 20, highCut = 20000;    // Hz (20 / 20000 = off)
};

class HybridDsp
{
public:
    static constexpr double kMaxLenS = 12.5;
    void prepare (double fs, int maxBlock)
    {
        sr = fs; mb = std::max (32, maxBlock);
        conv.prepare ((int) (kMaxLenS * fs) + 8192);
        inBuf.setSize (1, mb); erBuf.setSize (2, mb); tlBuf.setSize (2, mb);
        dl.assign ((size_t) juce::nextPowerOfTwo ((int) (0.6 * fs) + mb + 4), 0.0f); dlMask = (int) dl.size() - 1; dlW = 0;
        reset();
    }
    void reset()
    {
        conv.reset();
        std::fill (dl.begin(), dl.end(), 0.0f);
        for (auto& f : hp) f.reset(); for (auto& f : lp) f.reset();
        hpMix = tgt.lowCut > 20.5f ? 1.0f : 0.0f; lpMix = tgt.highCut < 19950.0f ? 1.0f : 0.0f;
        gEr = tgt.erG; gTl = tgt.tailG; dCur = dNew = (int) std::lround (std::max (0.0f, tgt.shiftMs) * 0.001 * sr); xfPos = -1;
        idleSamples = 0;
    }
    bool install (std::unique_ptr<FilterSet>& s) { return conv.install (s); }
    void collect() { conv.collect(); }
    const FilterSet* newest() const { return conv.newest(); }
    void setLive (const LiveSpec& s) { tgt = s; }
    // adds the IR's wet to L / R; in = the dry input
    void process (const float* inL, const float* inR, float* L, float* R, int n)
    {
        const auto* ns = conv.newest();
        const bool hasEr = ns && ns->part[0], hasTail = ns && ns->part[1];
        float te = hasEr ? tgt.erG : 0.0f, tt = hasTail ? tgt.tailG : 0.0f;
        // a part not heard for 0.5 s stops (its multiply-adds); the input history goes on
        for (int p = 0; p < 2; ++p)
        {
            const bool on = (p == 0 ? te : tt) > 0 || (p == 0 ? gEr : gTl) > 1e-5f;
            if (on) { idleP[(size_t) p] = 0; conv.enable (p, true); }
            else if ((idleP[(size_t) p] += n) > (int) (0.5 * sr)) conv.enable (p, false);
        }
        if (! conv.warm (0)) te = 0.0f;                  // (the gain rises once the part is complete)
        if (! conv.warm (1)) tt = 0.0f;
        // the delayed mono input (a 20 ms cross-fade between two taps when the delay moves)
        const int wantD = juce::jlimit (0, dlMask - mb - 2, (int) std::lround (std::max (0.0f, tgt.shiftMs) * 0.001 * sr));
        if (xfPos < 0 && wantD != dCur) { dNew = wantD; xfPos = 0; }
        const int xfLen = std::max (1, (int) (0.02 * sr));
        auto* a = inBuf.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            dl[(size_t) dlW] = 0.5f * (inL[i] + inR[i]);
            float y = dl[(size_t) ((dlW - dCur) & dlMask)];
            if (xfPos >= 0)
            {
                const float u = (float) xfPos / xfLen, y2 = dl[(size_t) ((dlW - dNew) & dlMask)];
                y = y * std::cos (u * juce::MathConstants<float>::halfPi) + y2 * std::sin (u * juce::MathConstants<float>::halfPi);
                if (++xfPos >= xfLen) { dCur = dNew; xfPos = -1; }
            }
            a[i] = y; dlW = (dlW + 1) & dlMask;
        }
        if (! ns) { gEr = te; gTl = tt; return; }
        conv.process (a, erBuf.getArrayOfWritePointers(), tlBuf.getArrayOfWritePointers(), n);
        if (! conv.enabled[0] && ! conv.enabled[1]) { gEr = te; gTl = tt; return; }
        const float* eL = erBuf.getReadPointer (0); const float* eR = erBuf.getReadPointer (1);
        const float* tL = tlBuf.getReadPointer (0); const float* tR = tlBuf.getReadPointer (1);
        const float k = 1.0f - std::exp (-1.0f / (float) (0.01 * sr));
        for (int i = 0; i < n; ++i)
        {
            if ((i & 31) == 0) updateCuts (std::min (32, n - i));
            gEr += k * (te - gEr); gTl += k * (tt - gTl); wCur += k * (tgt.width - wCur);
            float l = eL[i] * gEr + tL[i] * gTl, r = eR[i] * gEr + tR[i] * gTl;
            {   // (the cuts always run; "off" fades them out of the path, so that off = the IR as it is, and on never clicks)
                hpMix += k * ((tgt.lowCut > 20.5f ? 1.0f : 0.0f) - hpMix); lpMix += k * ((tgt.highCut < 19950.0f ? 1.0f : 0.0f) - lpMix);
                if (hpMix < 1e-6f) hpMix = 0; if (lpMix < 1e-6f) lpMix = 0;
                const float hl = hp[0].run (l), hr = hp[1].run (r); l += hpMix * (hl - l); r += hpMix * (hr - r);
                const float ll = lp[0].run (l), lr = lp[1].run (r); l += lpMix * (ll - l); r += lpMix * (lr - r);
            }
            const float m = 0.5f * (l + r), s = 0.5f * (l - r) * wCur;
            L[i] += m + s; R[i] += m - s;
        }
    }
    double sr = 48000;
private:
    // a state-variable filter (Cytomic TPT, Butterworth Q): it can sweep while the reverb rings without clicks
    struct Bq { bool high = false; float g = 0.1f, k2 = 1.41421356f, a1 = 1, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
                float run (float x) { const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                                      ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; return high ? x - k2 * v1 - v2 : v2; }
                void reset() { ic1 = ic2 = 0; }
                void set (bool hi, double fc, double fs) { high = hi; g = (float) std::tan (juce::MathConstants<double>::pi * std::min (fc, fs * 0.45) / fs);
                                                            a1 = 1 / (1 + g * (g + k2)); a2 = g * a1; a3 = g * a2; } };
    static void copyQ (Bq& d, const Bq& s) { d.high = s.high; d.g = s.g; d.k2 = s.k2; d.a1 = s.a1; d.a2 = s.a2; d.a3 = s.a3; }
    void updateCuts (int n)
    {
        const float k = 1.0f - std::exp (-(float) n / (float) (0.03 * sr));
        const float lcT = std::log (std::max (20.0f, tgt.lowCut)), hcT = std::log (tgt.highCut >= 19950.0f ? 21000.0f : tgt.highCut);
        if (lcCur < 0) lcCur = lcT; if (hcCur < 0) hcCur = hcT;
        lcCur = std::abs (lcT - lcCur) < 1e-3f ? lcT : lcCur + k * (lcT - lcCur);
        hcCur = std::abs (hcT - hcCur) < 1e-3f ? hcT : hcCur + k * (hcT - hcCur);
        // (always running: switching a filter in while the reverb rings would click; "off" = 20 Hz / 21 kHz)
        if (! hpOn || lcCur != lcSet) { hp[0].set (true, std::exp (lcCur), sr); copyQ (hp[1], hp[0]); lcSet = lcCur; }
        if (! lpOn || hcCur != hcSet) { lp[0].set (false, std::exp (hcCur), sr); copyQ (lp[1], lp[0]); hcSet = hcCur; }
        hpOn = lpOn = true;
    }
    SharedConv conv;
    std::array<int, 2> idleP { 0, 0 }; int idleSamples = 0;
    juce::AudioBuffer<float> inBuf, erBuf, tlBuf; int mb = 512;
    std::vector<float> dl; int dlMask = 0, dlW = 0, dCur = 0, dNew = 0, xfPos = -1;
    LiveSpec tgt; float gEr = 0, gTl = 0, wCur = 1;
    std::array<Bq, 2> hp, lp; bool hpOn = false, lpOn = false; float hpMix = 0, lpMix = 0; float lcCur = -1, hcCur = -1, lcSet = -1, hcSet = -1;
};
}
