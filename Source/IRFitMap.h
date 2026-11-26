#pragma once
// IR Fit Reverb: the macros -> the engine's parameters (no JUCE, no allocation: safe on the audio thread).
//
//  1. The macros give a target point in the models' feature space (plug-in measure of each fitted model):
//        lt   = log T30 (mid, 500 Hz - 2 kHz)            <- Decay
//        tilt = log (T30 4 kHz / T30 250 Hz)              <- typical tilt at that decay + Tone
//        c80  = C80 (dB, from the arrival)               <- typical C80 at that decay + Early
//        edtr = log (EDT 1 kHz / T30 mid)                 <- typical ratio at that decay + Build-up
//     ("typical" = the linear fit over the pool against log T30: Decay alone moves along how real rooms behave.)
//  2. Interpolation: the 4 nearest models of the chosen character (distance in (lt, tilt / C80 / EDT offsets), each in
//     standard deviations), modified Shepard weights w = 1/d - 1/d5 (continuous when the neighbours change, exact at a model).
//     Parameters are blended linearly or in log (times / ratios), the ER / tail timings relative to each model's own arrival.
//  3. Feed-forward correction: the gap between the target and the weighted features of the neighbours (zero at a model) is
//     closed with the first-order rules of the study's correction loop v1: reverb time per band (Decay, Tone), tail level /
//     ER energy (Early), initial decay (Build-up; past a single slope, the build-up spread).
//  4. The direct controls: Pre-Delay (= the arrival time), Size, Density, Width, Low / High Cut, Mix.
#include "IRFitTable.h"
#include <cmath>
#include <algorithm>

namespace irfit
{
enum Character { chAll = 0, chHall, chRoom, chChamber, chPlate, chAmbient, kNumChar };       // (types + 1)
enum Era { eraAny = 0, eraVintage, eraModern, kNumEra };

struct Macros
{
    float preDelay = 10, decay = 2.0f, size = 100, tone = 0, early = 0, buildUp = 0, density = 0, width = 100, lowCut = 20, highCut = 20000, mix = 30;
    int character = chAll, era = eraAny;
    bool operator== (const Macros& o) const
    {
        return preDelay == o.preDelay && decay == o.decay && size == o.size && tone == o.tone && early == o.early && buildUp == o.buildUp && density == o.density
            && width == o.width && lowCut == o.lowCut && highCut == o.highCut && mix == o.mix && character == o.character && era == o.era;
    }
    bool operator!= (const Macros& o) const { return ! (*this == o); }
};
// the knob scales (feature units per knob unit)
constexpr float kToneScale = 0.6f / 100.0f;       // Tone +-100 %  = tilt offset +-0.6 (log ratio; the pool spans -0.54 .. +0.55)
constexpr float kBuildScale = 1.0f / 100.0f;      // Build-up +-100 % = EDT/T30 offset +-1.0 (log; the pool spans -0.98 .. +0.69)
constexpr float kLtWeight = 1.5f;                 // log T30 counts 1.5x in the distance (the study's features had it twice: T30 and length)
constexpr int kK = 4;
constexpr double kEdtGain = 1.2;                  // tail initial-decay share (log) per unit of log EDT ratio (probed: 0.66 .. 1.07)
constexpr double kSpreadPerLog = 400.0;           // build-up spread (ms) per unit of log ratio past one straight slope (probed: 0.2 per 100 ms)
constexpr double kEdrMin = 55.0;                  // below ~55 % the two-slope tail's EDT stops following (probed: not monotonic at 40 - 60 %)
constexpr double kSizeDbPerDecade = 7.5;          // the wet level kept across Size (probed: +1.2 .. +3.4 dB at 50 %, -1.2 .. -3.5 dB at 200 %)
constexpr double kEarlyGain = 0.6;                // ER level up and tail level down, dB each per dB of C80 (probed: C80 moves 0.5 .. 0.9 dB per dB of ER level, 0.1 .. 1 per dB of tail)

struct Feat { double lt = 0, tilt = 0, c80 = 0, edtr = 0; };
inline Feat targetOf (const Macros& m)
{
    Feat f; f.lt = std::log (std::clamp ((double) m.decay, 0.1, 30.0));
    f.tilt = kTiltA + kTiltB * f.lt + kToneScale * m.tone;
    f.c80 = kC80A + kC80B * f.lt + m.early;
    f.edtr = kEdtrA + kEdtrB * f.lt + kBuildScale * m.buildUp;
    return f;
}
// a model's knob positions (the inverse of targetOf): loading a model as a program
inline Macros macrosOf (int e, Macros m)
{
    const auto& E = kEntries[e];
    m.decay = E.t30m; m.tone = (float) ((E.tilt - (kTiltA + kTiltB * E.lt)) / kToneScale);
    m.early = (float) (E.c80 - (kC80A + kC80B * E.lt)); m.buildUp = (float) ((E.edtr - (kEdtrA + kEdtrB * E.lt)) / kBuildScale);
    m.preDelay = E.onset; m.size = 100; m.density = 0; m.character = E.type + 1; m.era = E.era + 1;
    return m;
}
inline bool inPool (int e, int ch, int era)
{
    const auto& E = kEntries[e];
    return ! E.shortIr && (ch == chAll || E.type == ch - 1) && (era == eraAny || E.era == era - 1);
}

constexpr int kMaxOut = kNumP + 8;
struct Result
{
    std::pair<const char*, float> v[kMaxOut]; int n = 0;
    int nn[kK] {}; double w[kK] {}; int used = 0;          // the neighbours and their weights
    Feat tgt, fhat;                                          // the target and the neighbours' weighted features
    void put (const char* id, double x) { if (n < kMaxOut) v[n++] = { id, (float) x }; }
};

// C80 from the arrival (the first sample within 30 dB of the peak power) instead of from time 0, so Pre-Delay does not move it
inline double c80FromOnset (const float* L, const float* R, int n, double sr = 48000.0)
{
    double pk = 0; for (int i = 0; i < n; ++i) pk = std::max (pk, (double) L[i] * L[i] + (double) R[i] * R[i]);
    int i0 = 0; for (int i = 0; i < n; ++i) if ((double) L[i] * L[i] + (double) R[i] * R[i] >= pk * 1e-3) { i0 = i; break; }
    const int n80 = (int) (0.08 * sr); double a = 0, z = 0;
    for (int i = i0; i < n; ++i) (i < i0 + n80 ? a : z) += (double) L[i] * L[i] + (double) R[i] * R[i];
    return 10.0 * std::log10 (a / (z + 1e-30) + 1e-30);
}
// the coordinates of a measurement (fit::measure octaves 125 .. 8k Hz), as the table generator computes them
inline bool featOf (const double* t30, const double* edt, double c80, Feat& f)
{
    auto ok = [] (double x) { return std::isfinite (x) && x > 0; };
    double s = 0; int k = 0; for (int i : { 2, 3, 4 }) if (ok (t30[i])) { s += t30[i]; ++k; }
    if (k == 0) return false;
    const double t30m = s / k, t250 = ok (t30[1]) ? t30[1] : t30m, t4k = ok (t30[5]) ? t30[5] : t30m, e1k = ok (edt[3]) ? edt[3] : t30m;
    f.lt = std::log (t30m); f.tilt = std::log (t4k / t250); f.c80 = std::isfinite (c80) ? c80 : 0.0; f.edtr = std::log (std::clamp (e1k, 0.05, 20.0) / t30m);
    return true;
}

// extra correction offsets (feature units) from a measurement loop (0 = feed-forward only)
struct Trim { double lt = 0, tilt = 0, c80 = 0, edtr = 0; };

// (0.2.0) a single model played directly (an IR converted to ALGO, or a saved model): its own parameters in place of the
// blend of the library's neighbours. At its anchor (the knob positions computed from its measured features) the mapping
// gives exactly its parameters; away from it the same feed-forward corrections act relative to it.
struct ModelCore
{
    bool active = false;
    float p[kNumP] {};                 // the model's values of the mapped parameters (kPid order)
    float predelay = 0;                // its engine predelay (ms) at the anchor
    double eEr = 0, eTail = 0;         // wet energy of its ER alone / tail alone (dB, impulse in)
    float rtOct[5] { -3.3219f, -2.0f, 0.0f, 2.0f, 3.3219f };      // its RT points, octaves re 1 kHz (log2 (rtF / 1000))
    float sizeP = 100, eqHP = 20;      // its ER size and output high pass (the knobs scale / replace them away from the anchor)
    Macros anchor;                     // where it plays exactly
};

// a knob within 1e-5 (relative) of the model's anchor counts as at it (a value through a parameter's normalisation and back,
// or through a saved state, can come back an ulp off: the model must still play exactly)
inline Macros snapToAnchor (const Macros& m, const Macros& a)
{
    Macros r = m;
    auto snap = [] (float& v, float t) { if (std::abs (v - t) <= 1e-5f * std::max (1.0f, std::abs (t))) v = t; };
    snap (r.preDelay, a.preDelay); snap (r.decay, a.decay); snap (r.size, a.size); snap (r.tone, a.tone); snap (r.early, a.early); snap (r.buildUp, a.buildUp);
    snap (r.density, a.density); snap (r.width, a.width); snap (r.lowCut, a.lowCut); snap (r.highCut, a.highCut);
    return r;
}

inline void map (const Macros& mIn, Result& r, const Trim& trim = {}, const ModelCore* mc = nullptr)
{
    const bool model = mc != nullptr && mc->active;
    const Macros m = model ? snapToAnchor (mIn, mc->anchor) : mIn;
    r.n = 0; r.tgt = targetOf (m);
    double p[kNumP] {}; Feat fh; double intr = 0, eEr = 0, eTail = 0;
    if (model)
    {
        for (int i = 0; i < kNumP; ++i) p[i] = (double) mc->p[i];
        fh = targetOf (mc->anchor); eEr = mc->eEr; eTail = mc->eTail; r.used = 0;
        intr = (double) mc->anchor.preDelay - (double) mc->predelay;
    }
    else
    {
    // ---- the pool (a character / era with fewer than kK + 1 models widens: era first, then the type)
    int ch = m.character, era = m.era, cnt = 0;
    for (int pass = 0; pass < 3; ++pass)
    {
        cnt = 0; for (int e = 0; e < kNumEntries; ++e) if (inPool (e, ch, era)) ++cnt;
        if (cnt > kK) break;
        if (era != eraAny) era = eraAny; else ch = chAll;
    }
    // ---- the nearest kK + 1 (distance in the offsets from the typical, so Decay keeps the character's own tone / build-up)
    const Feat& T0 = r.tgt;
    const double tRes[3] = { T0.tilt - (kTiltA + kTiltB * T0.lt), T0.c80 - (kC80A + kC80B * T0.lt), T0.edtr - (kEdtrA + kEdtrB * T0.lt) };
    int best[kK + 1]; double bd[kK + 1]; int nb = 0;
    for (int e = 0; e < kNumEntries; ++e)
    {
        if (! inPool (e, ch, era)) continue;
        const auto& E = kEntries[e];
        const double a = kLtWeight * (E.lt - T0.lt) / kSdLt,
                     b = ((E.tilt - (kTiltA + kTiltB * E.lt)) - tRes[0]) / kSdTilt,
                     c = ((E.c80 - (kC80A + kC80B * E.lt)) - tRes[1]) / kSdC80,
                     d = ((E.edtr - (kEdtrA + kEdtrB * E.lt)) - tRes[2]) / kSdEdtr;
        const double dist = std::sqrt (a * a + b * b + c * c + d * d);
        int k;
        if (nb < kK + 1) k = nb++;
        else { if (dist >= bd[kK]) continue; k = kK; }
        while (k > 0 && bd[k - 1] > dist) { bd[k] = bd[k - 1]; best[k] = best[k - 1]; --k; }
        bd[k] = dist; best[k] = e;
    }
    // ---- modified Shepard weights: exact at a model, continuous as the neighbours change
    const int use = std::min (kK, nb); double ws = 0;
    if (bd[0] < 1e-9) { r.w[0] = 1; for (int j = 1; j < use; ++j) r.w[j] = 0; ws = 1; }
    else
    {
        const double far = nb > kK ? 1.0 / bd[kK] : 0.0;
        for (int j = 0; j < use; ++j) { r.w[j] = std::max (0.0, 1.0 / bd[j] - far); ws += r.w[j]; }
        if (ws <= 0) { r.w[0] = 1; ws = 1; }
    }
    for (int j = 0; j < use; ++j) { r.w[j] /= ws; r.nn[j] = best[j]; }
    r.used = use;
    // ---- the blended parameters and features
    for (int j = 0; j < use; ++j)
    {
        const auto& E = kEntries[best[j]]; const double w = r.w[j];
        for (int i = 0; i < kNumP; ++i) p[i] += w * (kLogP[i] ? std::log (std::max (1e-4f, E.p[i])) : (double) E.p[i]);
        intr += w * E.intr; eEr += w * E.eEr; eTail += w * E.eTail; fh.lt += w * E.lt; fh.tilt += w * E.tilt; fh.c80 += w * E.c80; fh.edtr += w * E.edtr;
    }
    for (int i = 0; i < kNumP; ++i) if (kLogP[i]) p[i] = std::exp (p[i]);
    }
    r.fhat = fh;
    const Feat& T = r.tgt;
    // ---- feed-forward correction: the target minus what the blend is expected to give (+ the measured trim)
    const double dLt = T.lt - fh.lt + trim.lt, dTilt = T.tilt - fh.tilt + trim.tilt, dC80 = std::clamp (T.c80 - fh.c80 + trim.c80, -24.0, 24.0),
                 dEdtr = T.edtr - fh.edtr + trim.edtr;
    const double tDecay0 = p[p_tDecay];
    p[p_tDecay] = std::clamp (p[p_tDecay] * std::exp (dLt), 0.1, 30.0);
    const double rtGain = p[p_tDecay] / tDecay0;
    if (rtGain < 1.0) p[p_tSpread] *= rtGain;          // a shorter decay than the neighbours: the build-up shortens with it
    {   // tilt: the RT points (100 / 250 / 1k / 4k / 10k Hz) by log2 (f / 1 kHz) / 4 (the measured tilt is over 250 Hz - 4 kHz = 4 octaves)
        static const double octLib[5] = { -3.3219, -2.0, 0.0, 2.0, 3.3219 };
        double oct[5]; for (int k = 0; k < 5; ++k) oct[k] = model ? (double) mc->rtOct[k] : octLib[k];
        const int id[5] = { p_rtM1, p_rtM2, p_rtM3, p_rtM4, p_rtM5 };
        for (int k = 0; k < 5; ++k) p[id[k]] = std::clamp (p[id[k]] * std::exp (dTilt * oct[k] / 4.0), 0.1, 10.0);
    }
    // C80: the ER up and the tail down by the same dB; then both by one gain so the wet energy stays the neighbours'
    // (every model is at the same wet energy; the tail's energy grows with its reverb time)
    {
        const double dEr = kEarlyGain * dC80, dTail = -kEarlyGain * dC80 + 10.0 * std::log10 (rtGain);
        auto sum = [] (double a, double b) { return 10.0 * std::log10 (std::pow (10.0, a / 10.0) + std::pow (10.0, b / 10.0)); };
        // (+ Size: a smaller space packs more reflections and shorter lines into the same time: about 7.5 dB louder per decade, probed)
        const double sz = model ? std::clamp ((double) m.size, 50.0, 200.0) / std::clamp ((double) mc->anchor.size, 50.0, 200.0) : std::clamp ((double) m.size, 50.0, 200.0) / 100.0;
        const double comp = sum (eEr, eTail) - sum (eEr + dEr, eTail + dTail) + kSizeDbPerDecade * std::log10 (sz);
        p[p_level] = std::clamp (p[p_level] + kEarlyGain * dC80 + comp, -24.0, 12.0);
        p[p_tLvl] = std::clamp (p[p_tLvl] - kEarlyGain * dC80 + comp, -36.0, 12.0);
    }
    {   // initial decay: the tail's EDT share (55 .. 100 %); past one straight slope a slower build-up, below 55 % a shorter one
        const double lo = std::min (kEdrMin, p[p_tEdr]), want = p[p_tEdr] * std::exp (kEdtGain * dEdtr);     // (a model below 55 % keeps its own)
        p[p_tEdr] = std::clamp (want, lo, 100.0);
        if (want > 100.0) p[p_tSpread] = std::clamp (p[p_tSpread] + kSpreadPerLog * std::log (want / 100.0), 0.0, 300.0);
        if (want < lo) p[p_tSpread] *= want / lo;
    }
    // ---- the direct controls (a model: relative to its anchor, so they are exact there)
    const double s = model ? std::clamp ((double) m.size, 50.0, 200.0) / std::clamp ((double) mc->anchor.size, 50.0, 200.0)
                           : std::clamp ((double) m.size, 50.0, 200.0) / 100.0;
    p[p_tSize] = std::clamp (p[p_tSize] * s, 10.0, 300.0);
    p[p_tSpread] = std::clamp (p[p_tSpread] * s, 0.0, 300.0);
    // Pre-Delay = the arrival time: the engine's predelay is what is left after the models' own delay before they arrive
    // (their first reflection / tail delay; scaled by Size as the engine scales them). Too short: the ER and the tail move earlier.
    double first = p[p_first], tPre = p[p_tPre] * s,
           pd = model ? ((double) m.preDelay - (double) mc->anchor.preDelay) + (double) mc->predelay - (s - 1.0) * intr     // (exact at the anchor)
                      : std::clamp ((double) m.preDelay, 0.0, 250.0) - s * intr;
    if (pd < 0.0) { first = std::max (1.0, first + pd / s); tPre = std::max (0.0, tPre + pd); pd = 0.0; }
    pd = std::min (pd, 200.0);
    p[p_first] = std::clamp (first, 1.0, 80.0); p[p_tPre] = std::clamp (tPre, 0.0, 300.0);
    const double d = std::clamp ((double) m.density, -100.0, 100.0) / 100.0;
    p[p_taps] = std::clamp (std::round (p[p_taps] * std::pow (2.0, d)), 4.0, 120.0);
    p[p_tDiff] = std::clamp (p[p_tDiff] + 30.0 * d, 0.0, 100.0);
    p[p_outW] = std::clamp (p[p_outW] * (model ? (double) m.width / std::max (1.0, (double) mc->anchor.width) : m.width / 100.0), 0.0, 200.0);
    if (! model || m.highCut != mc->anchor.highCut) p[p_eqLP] = std::clamp (std::min (p[p_eqLP], (double) m.highCut), 1000.0, 20000.0);
    p[p_taps] = std::round (p[p_taps]);
    for (int i = 0; i < kNumP; ++i) r.put (kPid[i], p[i]);
    const double hp = model && m.lowCut == mc->anchor.lowCut ? (double) mc->eqHP : std::clamp ((double) m.lowCut, 20.0, 1000.0);
    r.put ("predelay", pd); r.put ("size", model ? (double) mc->sizeP * s : s * 100.0); r.put ("eqHP", model ? std::max (hp, (double) mc->eqHP) : hp); r.put ("mix", std::clamp ((double) m.mix, 0.0, 100.0));
}
}
