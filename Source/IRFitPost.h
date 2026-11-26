#pragma once
// IR Fit Reverb (0.2.0): the product's own stage after the engine's wet output ("Advanced").
//  - Band width: the side signal (L - R) / 2 through a small EQ (low shelf, 3 peaks, high shelf) = the stereo width per
//    octave 250 Hz .. 4 kHz (the bands the late IACC is measured in).
//  - Fine EQ: 25 peaking filters on the 1/3-octave centres 50 Hz .. 16 kHz (the measure's own bands), 2/3-octave wide.
// Both are linear and time-invariant, so the converter fits them on a rendered IR (no engine renders needed) and the same
// code runs on the audio thread. Gains glide (no clicks when a model is loaded).
#include "TailDsp.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace irfit
{
constexpr int kFineBands = 25, kSideBands = 5;
inline double fineFc (int k) { return 50.0 * std::pow (2.0, k / 3.0); }
constexpr double kFineQ = 2.145;                  // 2/3 octave
// the side EQ: low shelf / peak / peak / peak / high shelf, aimed at the IACC octaves 250 / 500 / 1k / 2k / 4k Hz
inline double sideFc (int k) { static const double f[kSideBands] = { 354.0, 500.0, 1000.0, 2000.0, 2828.0 }; return f[k]; }
inline double sideBandFc (int k) { static const double f[kSideBands] = { 250.0, 500.0, 1000.0, 2000.0, 4000.0 }; return f[k]; }
constexpr double kSideQ = 1.41;

// (0.2.1) early taps: the IR's own strongest reflections of its first ~80 ms as discrete taps of the input (mono), added to the
// wet before the EQs. A fitted ALGO ER spreads its energy; a real room often has one or two reflections that stand out
// (the attack). Times in ms from the input, gains per side (linear, signed), at the model's own Pre-Delay / Size / ER level.
constexpr int kMaxTaps = 64;
struct PostSettings
{
    std::array<float, kFineBands> fine {};       // dB
    std::array<float, kSideBands> side {};       // dB on the side signal
    int nTaps = 0; std::array<float, kMaxTaps> tapMs {}, tapL {}, tapR {};
    bool isNeutral() const
    {
        if (nTaps > 0) return false;
        for (auto g : fine) if (std::abs (g) > 0.01f) return false;
        for (auto g : side) if (std::abs (g) > 0.01f) return false;
        return true;
    }
};

// one biquad's magnitude (dB) at f
inline double bqDb (const rv::BQ& b, double f, double sr)
{
    const double w = 2 * rv::kPi * f / sr, c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
    const double nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = -(b.b1 * s1 + b.b2 * s2), dr = 1 + b.a1 * c1 + b.a2 * c2, di = -(b.a1 * s1 + b.a2 * s2);
    return 10 * std::log10 ((nr * nr + ni * ni) / (dr * dr + di * di + 1e-30) + 1e-30);
}
inline void setFine (rv::BQ& b, int k, double gdB, double sr) { b.peak (std::min (fineFc (k), sr * 0.45), kFineQ, gdB, sr); }
inline void setSide (rv::BQ& b, int k, double gdB, double sr)
{
    if (k == 0) b.shelf (false, sideFc (k), gdB, sr);
    else if (k == kSideBands - 1) b.shelf (true, sideFc (k), gdB, sr);
    else b.peak (sideFc (k), kSideQ, gdB, sr);
}

// the gains (dB) of a bank whose summed response (dB) at the centres comes closest to want (damped Gauss-Newton on the
// exact responses; the Jacobian of a 1 dB step per band). lim: the largest gain.
template <int N>
inline std::array<float, N> solveBank (const std::array<double, N>& centres, const std::array<double, N>& want, void (*setter) (rv::BQ&, int, double, double),
                                       double lim, double ridge = 0.02, int iters = 6)
{
    const double sr = 48000;
    std::array<float, N> g {};
    auto resp = [&] (const std::array<float, N>& gg, std::array<double, N>& out)
    {
        out.fill (0.0); rv::BQ b;
        for (int j = 0; j < N; ++j) { if (gg[(size_t) j] == 0) continue; setter (b, j, gg[(size_t) j], sr); for (int k = 0; k < N; ++k) out[(size_t) k] += bqDb (b, centres[(size_t) k], sr); }
    };
    // Jacobian at 0 (dB at centre k per dB of band j): fixed, good enough for the damped iterations
    static thread_local std::vector<double> J; J.assign ((size_t) (N * N), 0.0);
    { rv::BQ b; for (int j = 0; j < N; ++j) { setter (b, j, 1.0, sr); for (int k = 0; k < N; ++k) J[(size_t) (k * N + j)] = bqDb (b, centres[(size_t) k], sr); } }
    std::array<double, N> h {};
    for (int it = 0; it < iters; ++it)
    {
        resp (g, h);
        std::vector<double> A ((size_t) (N * N), 0.0), r ((size_t) N, 0.0);
        for (int k = 0; k < N; ++k) { const double e = want[(size_t) k] - h[(size_t) k]; for (int j = 0; j < N; ++j) r[(size_t) j] += J[(size_t) (k * N + j)] * e; }
        for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { double s = 0; for (int k = 0; k < N; ++k) s += J[(size_t) (k * N + i)] * J[(size_t) (k * N + j)]; A[(size_t) (i * N + j)] = s; }
        for (int i = 0; i < N; ++i) A[(size_t) (i * N + i)] += ridge;
        // Gaussian elimination (N <= 25)
        std::vector<double> x = r;
        for (int c = 0; c < N; ++c)
        {
            int pv = c; for (int i = c + 1; i < N; ++i) if (std::abs (A[(size_t) (i * N + c)]) > std::abs (A[(size_t) (pv * N + c)])) pv = i;
            if (pv != c) { for (int j = 0; j < N; ++j) std::swap (A[(size_t) (c * N + j)], A[(size_t) (pv * N + j)]); std::swap (x[(size_t) c], x[(size_t) pv]); }
            const double d = A[(size_t) (c * N + c)]; if (std::abs (d) < 1e-12) continue;
            for (int i = c + 1; i < N; ++i) { const double f = A[(size_t) (i * N + c)] / d; if (f == 0) continue; for (int j = c; j < N; ++j) A[(size_t) (i * N + j)] -= f * A[(size_t) (c * N + j)]; x[(size_t) i] -= f * x[(size_t) c]; }
        }
        for (int c = N - 1; c >= 0; --c) { double s = x[(size_t) c]; for (int j = c + 1; j < N; ++j) s -= A[(size_t) (c * N + j)] * x[(size_t) j]; x[(size_t) c] = std::abs (A[(size_t) (c * N + c)]) > 1e-12 ? s / A[(size_t) (c * N + c)] : 0.0; }
        for (int j = 0; j < N; ++j) g[(size_t) j] = (float) std::clamp ((double) g[(size_t) j] + 0.8 * x[(size_t) j], -lim, lim);
    }
    return g;
}

// ---------------------------------------------------------------------------------------------------------------------
// the stage itself (audio thread: process; any thread before that: prepare). setTarget may be called from the audio thread.
class Post
{
public:
    void prepare (double fs)
    {
        sr = fs; { int sz = 1; while (sz < (int) (0.6 * fs) + 64) sz <<= 1; tapBuf.assign ((size_t) sz, 0.0f); tapMask = sz - 1; tapW = 0; } for (auto& b : fineL) b.reset(); for (auto& b : fineR) b.reset(); for (auto& b : sideB) b.reset();
        cur = tgt; for (int k = 0; k < kFineBands; ++k) { setFine (fineL[(size_t) k], k, cur.fine[(size_t) k], sr); fineR[(size_t) k] = fineL[(size_t) k]; fineR[(size_t) k].reset(); }
        for (int k = 0; k < kSideBands; ++k) setSide (sideB[(size_t) k], k, cur.side[(size_t) k], sr);
        updateActive();
    }
    void setTarget (const PostSettings& s, bool instant = false)
    {
        tgt = s;
        if (instant) { cur = s; for (int k = 0; k < kFineBands; ++k) { setFine (fineL[(size_t) k], k, cur.fine[(size_t) k], sr); copyCoef (fineR[(size_t) k], fineL[(size_t) k]); }
                       for (int k = 0; k < kSideBands; ++k) setSide (sideB[(size_t) k], k, cur.side[(size_t) k], sr); }
        updateActive();
    }
    void reset() { for (auto& b : fineL) b.reset(); for (auto& b : fineR) b.reset(); for (auto& b : sideB) b.reset(); std::fill (tapBuf.begin(), tapBuf.end(), 0.0f); }
    // (0.2.1) the taps follow the engine: a Pre-Delay change (ms), Size (time ratio), the ER level (dB) relative to the model's own
    void setTapTracking (double shiftMs, double sizeRatio, double gainDb) { tShift = (float) shiftMs; tSize = (float) sizeRatio; tGain = (float) std::pow (10.0, gainDb / 20.0); }
    // the wet (in place) + the taps of the input (inL / inR: the dry input; nullptr = no taps this block)
    void process (float* L, float* R, int n, const float* inL, const float* inR)
    {
        if (tgt.nTaps == 0 && inL != nullptr && ! tapBuf.empty())
        {   // (0.2.3) no taps now: the input history still runs, so taps that come back (a model after a factory preset)
            // read the recent input, not what was there when they went away (that jump clicked ~80 ms after the switch)
            for (int i = 0; i < n; ++i) { tapBuf[(size_t) tapW] = 0.5f * (inL[i] + inR[i]); tapW = (tapW + 1) & tapMask; }
        }
        else if (tgt.nTaps > 0 && inL != nullptr && ! tapBuf.empty())
        {
            const int nt = tgt.nTaps;
            for (int i = 0; i < n; ++i)
            {
                tapBuf[(size_t) tapW] = 0.5f * (inL[i] + inR[i]);
                curShift += 0.0005f * (tShift - curShift); curSize += 0.0005f * (tSize - curSize); curGain += 0.002f * (tGain - curGain);
                float aL = 0, aR = 0;
                for (int k = 0; k < nt; ++k)
                {
                    const float d = std::max (0.0f, (tgt.tapMs[(size_t) k] * curSize + curShift) * 0.001f * (float) sr);
                    const int di = (int) d; const float fr = d - (float) di;
                    if (di + 1 > tapMask) continue;
                    const float x = tapBuf[(size_t) ((tapW - di) & tapMask)] + fr * (tapBuf[(size_t) ((tapW - di - 1) & tapMask)] - tapBuf[(size_t) ((tapW - di) & tapMask)]);
                    aL += tgt.tapL[(size_t) k] * x; aR += tgt.tapR[(size_t) k] * x;
                }
                L[i] += curGain * aL; R[i] += curGain * aR;
                tapW = (tapW + 1) & tapMask;
            }
        }
        process (L, R, n);
    }
    // the taps of a unit impulse at sample 0 (offline renders), added in place
    static void addTaps (const PostSettings& s, float* L, float* R, int n, double fs = 48000)
    {
        for (int k = 0; k < s.nTaps; ++k)
        {
            const double d = s.tapMs[(size_t) k] * 0.001 * fs; const int di = (int) d; const float fr = (float) (d - di);
            if (di >= 0 && di < n) { L[di] += (1 - fr) * s.tapL[(size_t) k]; R[di] += (1 - fr) * s.tapR[(size_t) k]; }
            if (di + 1 >= 0 && di + 1 < n) { L[di + 1] += fr * s.tapL[(size_t) k]; R[di + 1] += fr * s.tapR[(size_t) k]; }
        }
    }
    // in place, stereo
    void process (float* L, float* R, int n)
    {
        if (! active) return;
        for (int i0 = 0; i0 < n; i0 += 32)
        {
            const int m = std::min (32, n - i0);
            glide (m);
            float* l = L + i0; float* r = R + i0;
            if (sideOn)
                for (int i = 0; i < m; ++i)
                {
                    const float mm = 0.5f * (l[i] + r[i]); float s = 0.5f * (l[i] - r[i]);
                    for (auto& b : sideB) s = b.run (s);
                    l[i] = mm + s; r[i] = mm - s;
                }
            if (fineOn)
                for (int k = 0; k < kFineBands; ++k)
                {
                    if (! bandOn[(size_t) k]) continue;
                    auto& bl = fineL[(size_t) k]; auto& br = fineR[(size_t) k];
                    for (int i = 0; i < m; ++i) { l[i] = bl.run (l[i]); r[i] = br.run (r[i]); }
                }
        }
    }
    // offline on a whole buffer (the converter): a fresh stage at fs with the settings at once
    static void apply (const PostSettings& s, float* L, float* R, int n, double fs = 48000)
    {
        if (s.isNeutral()) return;
        addTaps (s, L, R, n, fs);                                          // (an impulse input: the taps go in before the EQs, as in process)
        Post p; p.sr = fs; p.tgt = s; p.prepare (fs); p.process (L, R, n);
    }
private:
    static void copyCoef (rv::BQ& d, const rv::BQ& s) { d.b0 = s.b0; d.b1 = s.b1; d.b2 = s.b2; d.a1 = s.a1; d.a2 = s.a2; }
    void updateActive()
    {
        fineOn = sideOn = false;
        for (int k = 0; k < kFineBands; ++k) { bandOn[(size_t) k] = std::abs (tgt.fine[(size_t) k]) > 0.005f || std::abs (cur.fine[(size_t) k]) > 0.005f; fineOn = fineOn || bandOn[(size_t) k]; }
        for (int k = 0; k < kSideBands; ++k) sideOn = sideOn || std::abs (tgt.side[(size_t) k]) > 0.005f || std::abs (cur.side[(size_t) k]) > 0.005f;
        active = fineOn || sideOn;
    }
    void glide (int m)
    {   // ~25 ms to the target (one-pole per 32-sample step), the coefficients recomputed only for the bands that move
        const float k = 1.0f - (float) std::exp (-(double) m / (0.025 * sr));
        bool any = false;
        for (int j = 0; j < kFineBands; ++j)
        {
            float& c = cur.fine[(size_t) j]; const float t = tgt.fine[(size_t) j];
            if (c == t) continue;
            c = std::abs (t - c) < 0.003f ? t : c + k * (t - c); any = true;
            setFine (fineL[(size_t) j], j, c, sr); copyCoef (fineR[(size_t) j], fineL[(size_t) j]);
        }
        for (int j = 0; j < kSideBands; ++j)
        {
            float& c = cur.side[(size_t) j]; const float t = tgt.side[(size_t) j];
            if (c == t) continue;
            c = std::abs (t - c) < 0.003f ? t : c + k * (t - c); any = true;
            setSide (sideB[(size_t) j], j, c, sr);
        }
        if (any) updateActive();
    }
    double sr = 48000;
    PostSettings tgt, cur;
    std::vector<float> tapBuf; int tapMask = 0, tapW = 0; float tShift = 0, tSize = 1, tGain = 1, curShift = 0, curSize = 1, curGain = 1;
    std::array<rv::BQ, kFineBands> fineL, fineR; std::array<rv::BQ, kSideBands> sideB;
    std::array<bool, kFineBands> bandOn {}; bool fineOn = false, sideOn = false, active = false;
};
}
