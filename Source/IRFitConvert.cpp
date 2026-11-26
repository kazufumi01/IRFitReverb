#include "IRFitConvert.h"
#include "IRFitEqJac.h"
#include <map>
#include <iostream>

namespace irfit
{
juce::String partsText (const fit::Parts& p)
{
    juce::String s;
    s << "T30 " << juce::String (p.t30 * 5, 1) << " %  EDT " << juce::String (p.edt * 8, 1) << " %  C80 " << juce::String (p.c80 * 1.5, 2) << " dB  env " << juce::String (p.env * 2, 2)
      << " dB  specL " << juce::String (p.specL, 2) << "  specE " << juce::String (p.specE, 2) << "  IACC " << juce::String (p.iacc, 2) << "  NED " << juce::String (p.ned, 2);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
static bool readAny (const juce::File& f, juce::AudioBuffer<float>& b, double& sr)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f)); if (rd == nullptr) return false;
    sr = rd->sampleRate > 0 ? rd->sampleRate : 48000.0;
    const int n = (int) std::min<juce::int64> (rd->lengthInSamples, (juce::int64) (sr * 20));
    if (n < 64) return false;
    b.setSize ((int) std::max (1u, std::min (8u, rd->numChannels)), n); b.clear();
    rd->read (&b, 0, n, 0, true, true);
    return true;
}

juce::String prepareStereoIR (const juce::File& in, const juce::File& outWav, juce::String& displayName, int* chOut, double* lenOut)
{
    juce::AudioBuffer<float> a; double sr = 48000;
    if (! readAny (in, a, sr)) return "Cannot read this file (wav / aif, at least a few ms)";
    juce::AudioBuffer<float> st (2, a.getNumSamples()); st.clear();
    const int ch = a.getNumChannels(); if (chOut) *chOut = ch;
    displayName = in.getFileNameWithoutExtension();
    if (ch >= 4)
    {   // true stereo [LL, LR, RL, RR]: out L = LL + RL, out R = LR + RR
        st.addFrom (0, 0, a, 0, 0, a.getNumSamples()); st.addFrom (0, 0, a, 2, 0, a.getNumSamples());
        st.addFrom (1, 0, a, 1, 0, a.getNumSamples()); st.addFrom (1, 0, a, 3, 0, a.getNumSamples());
    }
    else if (ch >= 2) { st.copyFrom (0, 0, a, 0, 0, a.getNumSamples()); st.copyFrom (1, 0, a, 1, 0, a.getNumSamples()); }
    else { st.copyFrom (0, 0, a, 0, 0, a.getNumSamples()); st.copyFrom (1, 0, a, 0, 0, a.getNumSamples()); }
    // an *_L / *_R pair: two mono files = the two ears, two stereo files = a true-stereo pair (added)
    const auto partner = fit::pairOf (in);
    if (partner.existsAsFile() && ch <= 2)
    {
        juce::AudioBuffer<float> b; double sr2 = 0;
        if (readAny (partner, b, sr2) && std::abs (sr2 - sr) < 1)
        {
            const int n = std::max (st.getNumSamples(), b.getNumSamples()); st.setSize (2, n, true, true);
            if (ch == 1 && b.getNumChannels() == 1) { st.clear (1, 0, n); st.copyFrom (1, 0, b, 0, 0, b.getNumSamples()); }
            else for (int c = 0; c < 2; ++c) st.addFrom (c, 0, b, std::min (c, b.getNumChannels() - 1), 0, b.getNumSamples());
            displayName = fit::pairName (in);
        }
    }
    if (lenOut) *lenOut = st.getNumSamples() / sr;
    outWav.deleteFile();
    std::unique_ptr<juce::OutputStream> os (outWav.createOutputStream());
    if (os == nullptr) return "Cannot write a temporary file";
    juce::WavAudioFormat wav;
    auto o = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto w = wav.createWriterFor (os, o); if (w == nullptr) return "WAV writer failed";
    w->writeFromAudioSampleBuffer (st, 0, st.getNumSamples());
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------
//  the correction loop v1 (study scripts/rvc.py: correct / resid / gn_step) on the hidden engine
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
struct Eval { fit::Metrics m; fit::Parts parts; double score = 1e30, on30 = 0; std::vector<float> L, R; };

double onsetMs (const float* L, const float* R, int n, double db)
{
    double pk = 0; for (int i = 0; i < n; ++i) pk = std::max (pk, (double) L[i] * L[i] + (double) R[i] * R[i]);
    const double th = pk * std::pow (10.0, -db / 10); for (int i = 0; i < n; ++i) if ((double) L[i] * L[i] + (double) R[i] * R[i] >= th) return i / 48.0;
    return 0.0;
}
double ratioC (double t, double c, double lo = 0.5, double hi = 2.0)
{
    return (std::isfinite (t) && std::isfinite (c) && t > 0 && c > 0) ? std::clamp (t / c, lo, hi) : 1.0;
}
const std::vector<int> kRtOct[5] = { { 0 }, { 1 }, { 2, 3, 4 }, { 5 }, { 6, 5 } };       // RT point -> the octaves measured for it
double bandRatio (const double* t, const double* c, const std::vector<int>& idx)
{
    double s = 0; int k = 0;
    for (int i : idx) if (std::isfinite (t[i]) && std::isfinite (c[i])) { s += std::log (ratioC (t[i], c[i])); ++k; }
    return k ? std::exp (s / k) : 1.0;
}

struct Polisher
{
    ERProcessor& eng; const fit::Metrics& tm; double tOn30, lenS; std::atomic<bool>& stop;
    std::map<std::string, juce::NormalisableRange<float>> ranges;
    int renders = 0;
    std::function<void()> onRender;

    Polisher (ERProcessor& e, const fit::Metrics& t, double on, double len, std::atomic<bool>& s) : eng (e), tm (t), tOn30 (on), lenS (len), stop (s)
    {
        for (auto* p : eng.getParameters()) if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p)) ranges[rp->getParameterID().toStdString()] = rp->getNormalisableRange();
    }
    float& at (fit::Values& v, const char* id) { return v[id]; }
    void clampAll (fit::Values& v) const
    {
        for (auto& kv : v) { auto it = ranges.find (kv.first); if (it != ranges.end()) kv.second = it->second.snapToLegalValue (kv.second); }
    }
    void render (const fit::Values& v, std::vector<float>& L, std::vector<float>& R, double maxLen) const
    {
        ERProcessor::tlOverride = &v; ERProcessor::tlMaxLenS = maxLen;
        ERProcessor::ExportOpts o; o.sr = fit::kSR; o.bits = 32; o.tail = true; o.normalize = false;
        juce::AudioBuffer<float> b; double g = 0; eng.renderIR (o, b, g);
        ERProcessor::tlOverride = nullptr; ERProcessor::tlMaxLenS = 0;
        L.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples()); R.assign (b.getReadPointer (1), b.getReadPointer (1) + b.getNumSamples());
    }
    void measureInto (Eval& e, const PostSettings& post, const std::vector<float>& rawL, const std::vector<float>& rawR) const
    {
        std::vector<float> L = rawL, R = rawR;
        Post::apply (post, L.data(), R.data(), (int) L.size());
        e.m = fit::measure (L.data(), R.data(), (int) L.size());
        e.score = fit::compare (tm, e.m, &e.parts);
        e.on30 = onsetMs (L.data(), R.data(), (int) L.size(), 30);
    }
    Eval eval (const fit::Values& v, const PostSettings& post)
    {
        Eval e; render (v, e.L, e.R, lenS); ++renders;
        measureInto (e, post, e.L, e.R);
        if (onRender) onRender();
        return e;
    }

    // ---- rules (one step from the measures: target tm, current m)
    void correct (fit::Values& v, const Eval& c, bool time, bool t30, bool c80, bool edt, bool tone)
    {
        const auto& m = c.m;
        if (time)
        {
            const double dt = tOn30 - c.on30;
            if (std::isfinite (dt) && std::abs (dt) > 0.5)
            {
                double pd = v["predelay"] + dt;
                if (pd < 0) { v["first"] = (float) std::clamp (v["first"] + pd, 1.0, 80.0); v["tPre"] = (float) std::clamp (v["tPre"] + pd, 0.0, 300.0); pd = 0; }
                v["predelay"] = (float) std::clamp (pd, 0.0, 200.0);
            }
        }
        if (t30)
            for (int j = 0; j < 5; ++j) { auto& x = v[("rtM" + std::to_string (j + 1)).c_str()]; x = (float) std::clamp (x * bandRatio (tm.t30, m.t30, kRtOct[j]), 0.05, 20.0); }
        if (c80)
        {
            double dl = tm.c80 - m.c80; dl = std::isfinite (dl) ? std::clamp (dl, -12.0, 12.0) : 0.0;
            v["tLvl"] = (float) std::clamp (v["tLvl"] - 0.8 * dl, -36.0, 12.0); v["energy"] = (float) std::clamp (v["energy"] + 0.2 * dl, -30.0, 12.0);
        }
        if (edt)
        {
            for (int j = 0; j < 5; ++j)
            {
                const double r = std::pow (bandRatio (tm.edt, m.edt, kRtOct[j]), 0.8);
                const std::string id = "tEd" + std::to_string (j + 1);
                const float cur = v.count (id) ? v[id] : 100.0f;
                v[id] = (float) std::clamp (cur * r, 40.0, 130.0);
            }
            float mx = 0; for (int j = 0; j < 5; ++j) mx = std::max (mx, v["tEd" + std::to_string (j + 1)]);
            if (mx > 100 && v["tEdr"] < 100)
            {
                const double k = std::min ((double) mx / 100.0, 100.0 / v["tEdr"]); v["tEdr"] = (float) (v["tEdr"] * k);
                for (int j = 0; j < 5; ++j) { auto& x = v["tEd" + std::to_string (j + 1)]; x = (float) std::clamp (x / k, 40.0, 130.0); }
            }
        }
        if (tone)
        {   // the master EQ from the late (weight 1) and early (0.5) spectral shape, with the fixed EQ Jacobian
            const int n = (int) std::min ({ tm.specL.size(), m.specL.size(), tm.specE.size(), m.specE.size(), (size_t) 25 });
            if (n > 6)
            {
                double mL[6] {}, mE[6] {}, bL = 0, bE = 0;
                for (int k = 0; k < n; ++k) for (int j = 0; j < 6; ++j) { mL[j] += kEqJac[0][k][j] / n; mE[j] += kEqJac[1][k][j] / n; }
                for (int k = 0; k < n; ++k) { bL += (tm.specL[(size_t) k] - m.specL[(size_t) k]) / n; bE += (tm.specE[(size_t) k] - m.specE[(size_t) k]) / n; }
                double A[6][6] {}, r[6] {};
                for (int k = 0; k < 2 * n; ++k)
                {
                    const bool late = k < n; const int q = late ? k : k - n; const double w = late ? 1.0 : 0.5;
                    double row[6]; for (int j = 0; j < 6; ++j) row[j] = w * (kEqJac[late ? 0 : 1][q][j] - (late ? mL[j] : mE[j]));
                    const double b = w * (late ? (tm.specL[(size_t) q] - m.specL[(size_t) q] - bL) : (tm.specE[(size_t) q] - m.specE[(size_t) q] - bE));
                    if (! std::isfinite (b)) continue;
                    for (int i = 0; i < 6; ++i) { r[i] += row[i] * b; for (int j = 0; j < 6; ++j) A[i][j] += row[i] * row[j]; }
                }
                for (int i = 0; i < 6; ++i) A[i][i] += 0.05;
                double d[6]; std::copy (r, r + 6, d);
                for (int c = 0; c < 6; ++c) { const double p = A[c][c]; for (int i = c + 1; i < 6; ++i) { const double f = A[i][c] / p; for (int j = c; j < 6; ++j) A[i][j] -= f * A[c][j]; d[i] -= f * d[c]; } }
                for (int c = 5; c >= 0; --c) { double s = d[c]; for (int j = c + 1; j < 6; ++j) s -= A[c][j] * d[j]; d[c] = s / A[c][c]; }
                for (int j = 0; j < 6; ++j) { auto& x = v["eqG" + std::to_string (j + 1)]; x = (float) std::clamp (x + 0.8 * std::clamp (d[j], -6.0, 6.0), -18.0, 18.0); }
            }
        }
        clampAll (v);
    }

    // ---- the score parts as one residual vector (rvc.resid)
    std::vector<double> resid (const fit::Metrics& m) const
    {
        auto lr = [] (double a, double b) { return (std::isfinite (a) && std::isfinite (b) && a > 0 && b > 0) ? std::log (b / a) : 0.7; };
        std::vector<double> r, t30, edt;
        for (int k = 0; k < 7; ++k) if (std::isfinite (tm.t30[k])) t30.push_back (lr (tm.t30[k], m.t30[k]) / 0.05);
        for (int k = 0; k < 7; ++k)
        {
            if (! std::isfinite (tm.edt[k])) continue;
            const double we = (k == 6 && std::isfinite (tm.t30[k]) && tm.edt[k] < 0.5 * tm.t30[k]) ? 0.2 : 1.0;
            edt.push_back (we * lr (tm.edt[k], m.edt[k]) / 0.08);
        }
        for (auto x : t30) r.push_back (x / std::sqrt (std::max<size_t> (1, t30.size())));
        for (auto x : edt) r.push_back (x / std::sqrt (std::max<size_t> (1, edt.size())));
        const size_t n = std::min (tm.env.size(), m.env.size());
        for (size_t i = 0; i < n; ++i) r.push_back ((juce::jlimit (-60.0, 0.0, tm.env[i]) - juce::jlimit (-60.0, 0.0, m.env[i])) / 2.0 / std::sqrt ((double) std::max<size_t> (1, n)));
        r.push_back ((tm.c80 - m.c80) / 1.5);
        for (int k = 0; k < 4; ++k) r.push_back ((tm.ned[k] - m.ned[k]) / 0.10 / 2.0);
        auto spec = [&] (const std::vector<double>& a, const std::vector<double>& b, double w)
        {
            const size_t q = std::min (a.size(), b.size()); if (q == 0) return;
            double mean = 0; for (size_t i = 0; i < q; ++i) mean += a[i] - b[i]; mean /= q;
            for (size_t i = 0; i < q; ++i) r.push_back (w * (a[i] - b[i] - mean) / 1.5 / std::sqrt ((double) q));
        };
        spec (tm.specL, m.specL, 1.0); spec (tm.specE, m.specE, 0.5);
        for (int k = 0; k < fit::kIacc; ++k) r.push_back ((tm.iacc[k] - m.iacc[k]) / 0.08 / std::sqrt (5.0));
        return r;
    }

    struct GnP { const char* id; double h, lo, hi; };
    // one damped Gauss-Newton step from (v, cur); returns true and replaces them when the score improved
    bool gnStep (fit::Values& v, Eval& cur, const std::vector<GnP>& ps, const PostSettings& post, double lam = 0.3, double maxSteps = 3.0)
    {
        const auto e0 = resid (cur.m); const size_t R = e0.size(), P = ps.size();
        std::vector<std::vector<double>> J (P);
        for (size_t j = 0; j < P; ++j)
        {
            if (stop.load()) return false;
            const auto& p = ps[j]; fit::Values w = v; const double x0 = v[p.id];
            double x = x0 + p.h <= p.hi ? x0 + p.h : x0 - p.h; w[p.id] = (float) x; clampAll (w);
            const double hh = w[p.id] - x0; if (std::abs (hh) < 1e-9) { J[j].assign (R, 0.0); continue; }
            const auto e = eval (w, post); const auto r = resid (e.m);
            J[j].assign (R, 0.0); for (size_t i = 0; i < R && i < r.size(); ++i) J[j][i] = (r[i] - e0[i]) / (hh / p.h);
        }
        std::vector<double> A (P * P, 0.0), g (P, 0.0);
        for (size_t a = 0; a < P; ++a) { for (size_t i = 0; i < R; ++i) g[a] += J[a][i] * e0[i]; for (size_t b = 0; b < P; ++b) { double s = 0; for (size_t i = 0; i < R; ++i) s += J[a][i] * J[b][i]; A[a * P + b] = s; } }
        for (size_t a = 0; a < P; ++a) A[a * P + a] += lam * (A[a * P + a] + 1e-6);
        std::vector<double> d (P); for (size_t a = 0; a < P; ++a) d[a] = -g[a];
        for (size_t c = 0; c < P; ++c)
        {
            size_t pv = c; for (size_t i = c + 1; i < P; ++i) if (std::abs (A[i * P + c]) > std::abs (A[pv * P + c])) pv = i;
            if (pv != c) { for (size_t j = 0; j < P; ++j) std::swap (A[c * P + j], A[pv * P + j]); std::swap (d[c], d[pv]); }
            const double piv = A[c * P + c]; if (std::abs (piv) < 1e-15) continue;
            for (size_t i = c + 1; i < P; ++i) { const double f = A[i * P + c] / piv; for (size_t j = c; j < P; ++j) A[i * P + j] -= f * A[c * P + j]; d[i] -= f * d[c]; }
        }
        for (size_t c = P; c-- > 0;) { double s = d[c]; for (size_t j = c + 1; j < P; ++j) s -= A[c * P + j] * d[j]; d[c] = std::abs (A[c * P + c]) > 1e-15 ? s / A[c * P + c] : 0.0; }
        for (auto& x : d) x = std::clamp (x, -maxSteps, maxSteps);
        for (double s : { 1.0, 0.5, 0.25 })
        {
            if (stop.load()) return false;
            fit::Values w = v;
            for (size_t j = 0; j < P; ++j) { const auto& p = ps[j]; w[p.id] = (float) std::clamp (v[p.id] + s * d[j] * p.h, p.lo, p.hi); }
            clampAll (w);
            auto e = eval (w, post);
            if (e.score < cur.score) { v = w; cur = std::move (e); return true; }
        }
        return false;
    }

    // ---- the post stage, fitted on the current render (no new engine renders)
    // the signed zero-lag correlation of L / R per IACC octave over the late part (80 .. 880 ms), after the post stage
    static void bandCorr (const std::vector<float>& L, const std::vector<float>& R, double* out)
    {
        const int n = (int) L.size(), i0 = (int) (0.08 * fit::kSR), i1 = std::min (n, i0 + (int) (0.8 * fit::kSR));
        for (int k = 0; k < kSideBands; ++k)
        {
            fit::Band bl, br; bl.set (sideBandFc (k), fit::kSR); br.set (sideBandFc (k), fit::kSR);
            double ll = 0, rr = 0, lr = 0;
            for (int i = 0; i < i1; ++i) { const double l = bl.run (L[(size_t) i]), r = br.run (R[(size_t) i]); if (i >= i0) { ll += l * l; rr += r * r; lr += l * r; } }
            out[k] = lr / (std::sqrt (ll * rr) + 1e-30);
        }
    }
    void fitWidth (const Eval& base, PostSettings& post)
    {   // the side gain per IACC octave: with M and S uncorrelated, corr = (M - S) / (M + S), so S / M = (1 - corr) / (1 + corr).
        // The target's IACC is a magnitude: the sign nearer the current correlation is aimed at. Three passes on the measure.
        std::array<double, kSideBands> want {};
        { rv::BQ b; for (int k = 0; k < kSideBands; ++k) { double s = 0; for (int j = 0; j < kSideBands; ++j) { setSide (b, j, post.side[(size_t) j], 48000); s += bqDb (b, sideBandFc (k), 48000); } want[(size_t) k] = s; } }
        std::array<double, kSideBands> cen {}; for (int k = 0; k < kSideBands; ++k) cen[(size_t) k] = sideBandFc (k);
        for (int it = 0; it < 3 && ! stop.load(); ++it)
        {
            std::vector<float> L = base.L, R = base.R; Post::apply (post, L.data(), R.data(), (int) L.size());
            const auto m = fit::measure (L.data(), R.data(), (int) L.size());
            double c[kSideBands]; bandCorr (L, R, c);
            for (int k = 0; k < kSideBands; ++k)
            {
                // the measured IACC (max |corr| over +-1 ms) against the zero-lag one: aim the zero-lag correlation at the target scaled alike
                const double Ic = std::clamp (c[k], -0.97, 0.97), mag = std::max (1e-3, std::abs (Ic)), scale = std::clamp (m.iacc[k] / mag, 1.0, 3.0);
                const double It = std::clamp (tm.iacc[k] / scale, 0.0, 0.97);
                auto rr = [] (double I) { return (1 - I) / (1 + I); };
                const double a = std::log (rr (It) / rr (Ic)), b2 = std::log (rr (-It) / rr (Ic));
                const double d = std::abs (a) < std::abs (b2) ? a : b2;                  // ln (S / M power ratio change)
                want[(size_t) k] = std::clamp (want[(size_t) k] + 0.8 * 10 / std::log (10.0) * d, -15.0, 12.0);
            }
            post.side = solveBank<kSideBands> (cen, want, &setSide, 15.0, 0.01);
            if (std::getenv ("IRFIT_DEBUG"))
            {
                Eval e; measureInto (e, post, base.L, base.R);
                juce::String s ("width it " + juce::String (it) + ":");
                for (int k = 0; k < kSideBands; ++k) s << " [" << juce::String (tm.iacc[k], 2) << "/" << juce::String (m.iacc[k], 2) << " c " << juce::String (c[k], 2) << " g " << juce::String (post.side[(size_t) k], 1) << "]";
                std::cerr << s << " iaccPart " << e.parts.iacc << " score " << e.score << std::endl;
            }
        }
    }
    void fitFine (const Eval& base, PostSettings& post)
    {   // the fine EQ: the late (1) and early (0.5) 1/3-octave shapes, the weighted mean taken out; three passes on the measure
        std::array<double, kFineBands> curve {}, cen {};
        for (int k = 0; k < kFineBands; ++k) cen[(size_t) k] = fineFc (k);
        { rv::BQ b; for (int k = 0; k < kFineBands; ++k) { double s = 0; for (int j = 0; j < kFineBands; ++j) { if (post.fine[(size_t) j] == 0) continue; setFine (b, j, post.fine[(size_t) j], 48000); s += bqDb (b, cen[(size_t) k], 48000); } curve[(size_t) k] = s; } }
        Eval e; measureInto (e, post, base.L, base.R);
        for (int it = 0; it < 3 && ! stop.load(); ++it)
        {
            const size_t n = std::min ({ (size_t) kFineBands, tm.specL.size(), e.m.specL.size(), tm.specE.size(), e.m.specE.size() });
            std::array<double, kFineBands> d {}; double mean = 0;
            for (size_t k = 0; k < n; ++k)
            {
                const double dl = tm.specL[k] - e.m.specL[k], de = tm.specE[k] - e.m.specE[k];
                d[k] = std::isfinite (dl) && std::isfinite (de) ? (dl + 0.25 * de) / 1.25 : 0.0; mean += d[k] / n;
            }
            for (size_t k = 0; k < n; ++k) curve[k] = std::clamp (curve[k] + 0.9 * (d[k] - mean), -12.0, 12.0);
            post.fine = solveBank<kFineBands> (cen, curve, &setFine, 12.0, 0.02);
            measureInto (e, post, base.L, base.R);
            if (std::getenv ("IRFIT_DEBUG")) std::cerr << "fine it " << it << ": score " << e.score << " | " << partsText (e.parts) << std::endl;
        }
    }
};
}

// ---------------------------------------------------------------------------------------------------------------------
Converter::Converter() {}
Converter::~Converter()
{
    stop = true; if (fitEng) fitEng->cancelBatch();
    joinWorker();
    fitEng.reset();
    if (tmpDir.isDirectory()) tmpDir.deleteRecursively();
}

double Converter::elapsedS() const { return t0 > 0 ? (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 : 0.0; }
double Converter::etaS() const
{
    const double p = progress.load(), e = elapsedS();
    return p > 0.04 && running() ? e / p * (1 - p) : -1;
}

juce::String Converter::start (const juce::File& ir, const Options& o)
{
    if (running()) return "A conversion is already running";
    joinWorker();
    opts = o; error = {}; haveResult = false; stop = false; workerDone = false; progress = 0;
    { const juce::ScopedLock l (lock); log.clear(); }
    if (tmpDir.isDirectory()) tmpDir.deleteRecursively();
    tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("IRFitReverb-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    tmpDir.createDirectory();
    juce::String name; tmpWav = tmpDir.getChildFile ("ir.wav");
    if (auto err = prepareStereoIR (ir, tmpWav, name); err.isNotEmpty()) { error = err; state = failed; return err; }
    irName = name;
    // (the Lab names its report after the file; keep the IR's own name, without an _L / _R ending that would look like a pair)
    { auto nm = juce::File::createLegalFileName (name); if (nm.endsWith ("_L") || nm.endsWith ("_R") || nm.endsWith (" L") || nm.endsWith (" R") || nm.endsWith ("-L") || nm.endsWith ("-R")) nm << "_";
      const auto f2 = tmpDir.getChildFile (nm + ".wav"); if (tmpWav.moveFileTo (f2)) tmpWav = f2; }
    t0 = juce::Time::getMillisecondCounterHiRes(); labSeconds = 0;
    fitEng = std::make_unique<ERProcessor>(); fitEng->flatBuildUp = std::getenv ("IRFIT_SPARSE_BU") == nullptr; fitEng->setRateAndBufferSizeDetails (48000, 256); fitEng->prepareToPlay (48000, 256);
    fitEng->fitParallel = std::getenv ("IRFIT_SERIAL") == nullptr;        // (0.2.3) the Lab fit's three starts side by side
    if (opts.fast) { fitEng->fitStarts = 1; fitEng->fitScale = opts.fastScale; }
    if (! opts.lab) { state = polishing; return {}; }
    state = labFit; setText ("Analysing the IR...");
    fitEng->startBatch ({ tmpWav }, "", true);
    return {};
}

juce::String Converter::startFromValues (const juce::File& ir, const fit::Values& v, const Options& o)
{
    Options oo = o; oo.lab = false;
    if (auto err = start (ir, oo); err.isNotEmpty()) return err;
    launchPolish (v);
    return {};
}

void Converter::cancel()
{
    if (! running()) return;
    stop = true;
    if (fitEng) fitEng->cancelBatch();
    setText ("Cancelling...");
}

void Converter::tick()
{
    const int s = state.load();
    if (s == labFit && fitEng)
    {
        if (opts.drivePoll) fitEng->poll();
        const float fp = fitEng->batchFitProgress.load();
        progress = 0.02f + 0.83f * std::min (1.0f, fp / 0.8f);
        if (fp > 0) setText ("Fitting (Lab closed loop)  " + juce::String (juce::roundToInt (std::min (1.0f, fp / 0.8f) * 100)) + " %");
        if (fitEng->batchRunning.load()) return;
        tLab = elapsedS(); labSeconds = tLab;
        if (stop.load()) { state = cancelled; setText ("Cancelled"); fitEng.reset(); return; }
        if (fitEng->batchOk.load() < 1) { error = "The IR could not be analysed: " + fitEng->batchLog.trim(); state = failed; setText (error); fitEng.reset(); return; }
        fit::Values v;
        for (auto* pr : fitEng->getParameters()) if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (pr)) v[rp->getParameterID().toStdString()] = rp->convertFrom0to1 (rp->getValue());
        state = polishing;
        launchPolish (std::move (v));
        return;
    }
    if (s == polishing && workerDone.load())
    {
        joinWorker();
        fitEng.reset();
        if (tmpDir.isDirectory()) tmpDir.deleteRecursively();
        if (stop.load()) { state = cancelled; setText ("Cancelled"); return; }
        if (! result.valid()) { state = failed; if (error.isEmpty()) error = "The conversion failed"; setText (error); return; }
        haveResult = true; progress = 1; state = done;
        setText ("Done: score " + juce::String (result.info.score, 1) + " in " + juce::String (juce::roundToInt (result.info.seconds)) + " s");
    }
}

bool Converter::takeResult (UserModel& out)
{
    if (! haveResult) return false;
    out = result; haveResult = false; return true;
}

void Converter::launchPolish (fit::Values v)
{
    joinWorker();
    workerDone = false;
    worker = std::make_unique<Worker>();
    worker->fn = [this, v = std::move (v)]() mutable { polishRun (std::move (v)); workerDone = true; };
    worker->startThread (juce::Thread::Priority::low);          // (as the Lab's own fit: below everything the host runs)
}

void Converter::polishRun (fit::Values v)
{
    result = UserModel();
    auto addStage = [this] (const juce::String& nm, const Eval& e, int renders)
    {
        StageScore s; s.stage = nm; s.score = e.score; s.parts = e.parts; s.seconds = elapsedS(); s.renders = renders;
        const juce::ScopedLock l (lock); log.push_back (s);
    };
    std::vector<float> tL, tR;
    if (! fit::loadTarget (tmpWav, {}, tL, tR) || tL.size() < 4800) { error = "The IR is too short"; return; }
    const auto tm = fit::measure (tL.data(), tR.data(), (int) tL.size());
    const double tOn30 = onsetMs (tL.data(), tR.data(), (int) tL.size(), 30);
    const double lenS = juce::jlimit (0.5, 8.0, tL.size() / fit::kSR + 0.2);
    Polisher P (*fitEng, tm, tOn30, lenS, stop);
    const int expect = opts.polish ? 40 : 6;
    P.onRender = [this, &P, expect] { progress = 0.85f + 0.14f * std::min (1.0f, P.renders / (float) expect); };
    v["autoLvl"] = 0; P.clampAll (v);
    PostSettings post;
    setText ("Correction loop (T30, EDT, C80, tone, build-up)");
    Eval best = P.eval (v, post); fit::Values bestV = v;
    addStage ("lab", best, P.renders);
    const double labScore = best.score;
    if (opts.polish && ! stop.load() && std::getenv ("IRFIT_NO_EDW") == nullptr)
    {   // (0.2.2) the energy-correct two-slope split reaches a shorter low-band EDT; re-aim the per-band
        // initial decay under it (EDT + T30 rules) and keep it only if the whole score is better
        fit::Values w = v; w["tEdW"] = 1; Eval cw = P.eval (w, post); fit::Values bw = w; Eval bwe = cw;
        for (int it = 0; it < 4 && ! stop.load(); ++it)
        {
            P.correct (w, cw, false, true, false, true, false);
            cw = P.eval (w, post);
            if (cw.score < bwe.score) { bwe = cw; bw = w; }
        }
        if (std::getenv ("IRFIT_DEBUG")) std::fprintf (stderr, "edt balance: %.2f -> %.2f\n", best.score, bwe.score);
        if (bwe.score < best.score) { best = bwe; v = bw; bestV = bw; }
        addStage ("edt balance", best, P.renders);
    }
    if (opts.polish && ! stop.load())
    {
        // two rule passes (arrival, T30, C80, EDT), each kept only if better (from the Lab fit the rules can overshoot)
        Eval cur = best; fit::Values cv = v;
        for (int it = 0; it < 2 && ! stop.load(); ++it)
        {
            P.correct (cv, cur, true, true, true, true, false);
            cur = P.eval (cv, post);
            if (cur.score < best.score) { best = cur; bestV = cv; }
        }
        addStage ("rules", best, P.renders);
        // the build-up Gauss-Newton (rvc GN_P), + (0.2.0) diffusion / reflection count for the echo density
        std::vector<Polisher::GnP> ps = { { "tSpread", 15, 0, 300 }, { "tShape", 15, -100, 100 }, { "tPre", 3, 0, 200 }, { "energy", 1.5, -30, 12 },
                                          { "tLvl", 1.5, -36, 12 }, { "decay", 3, -40, 24 }, { "tSpreadHi", 15, 20, 200 }, { "tFill", 10, 0, 100 },
                                          { "first", 3, 1, 80 }, { "predelay", 3, 0, 200 } };
        if (opts.nedParams) { ps.push_back ({ "tDiff", 8, 0, 100 }); ps.push_back ({ "taps", 8, 4, 120 }); }
        for (int g = 0; g < opts.gnIters && ! stop.load(); ++g)
        {
            setText ("Correction loop: build-up (Gauss-Newton " + juce::String (g + 1) + ")");
            if (! P.gnStep (bestV, best, ps, post)) break;
        }
        addStage ("gn", best, P.renders);
        // the last rule pass: T30, C80, EDT and the tone (master EQ)
        { fit::Values w = bestV; P.correct (w, best, false, true, true, true, true); auto e = P.eval (w, post); if (e.score < best.score) { best = std::move (e); bestV = w; } }
        addStage ("post-rules", best, P.renders);
    }
    if (opts.post && ! stop.load())
    {   // (0.2.0) the product's post stage, fitted on the render: the band width and the fine EQ, each at the amount (0 .. 100 %)
        // that gives the best whole score (the fine EQ also moves the echo density and the envelope), then one Gauss-Newton step
        // of the build-up / density parameters with the post stage in.
        setText ("Band width and fine EQ");
        auto tryPost = [&] (const PostSettings& ps) { Eval e; P.measureInto (e, ps, best.L, best.R); e.L = best.L; e.R = best.R; return e; };
        auto scaled = [] (PostSettings ps, double fa, double wa) { for (auto& g : ps.fine) g = (float) (g * fa); for (auto& g : ps.side) g = (float) (g * wa); return ps; };
        PostSettings pw; P.fitWidth (best, pw);
        double wa = 0; Eval bw = best;
        for (double a : { 1.0, 0.7, 0.4 }) { auto e = tryPost (scaled (pw, 0, a)); if (e.score < bw.score) { bw = std::move (e); wa = a; } }
        addStage ("width", bw, P.renders);
        PostSettings pf = scaled (pw, 0, wa); P.fitFine (best, pf);
        double fa = 0; Eval bf = bw;
        for (double a : { 1.0, 0.8, 0.6, 0.4, 0.25 }) { auto e = tryPost (scaled (pf, a, 1.0)); if (e.score < bf.score) { bf = std::move (e); fa = a; } }
        if (bf.score < best.score) { best = std::move (bf); post = scaled (pf, fa, 1.0); }
        addStage ("fine-eq", best, P.renders);
        if (std::getenv ("IRFIT_DEBUG")) std::cerr << "post amounts: width " << wa << " fine " << fa << std::endl;
        if (opts.polish && ! post.isNeutral() && ! stop.load())
        {
            std::vector<Polisher::GnP> ps = { { "tSpread", 15, 0, 300 }, { "tShape", 15, -100, 100 }, { "energy", 1.5, -30, 12 }, { "tLvl", 1.5, -36, 12 },
                                              { "decay", 3, -40, 24 }, { "tFill", 10, 0, 100 }, { "tDiff", 8, 0, 100 }, { "taps", 8, 4, 120 } };
            setText ("Correction loop with the post stage");
            P.gnStep (bestV, best, ps, post);
            addStage ("gn-post", best, P.renders);
        }
        // T30 / C80 once more with the post stage in
        if (! stop.load()) { fit::Values w = bestV; P.correct (w, best, false, true, true, false, false); auto e = P.eval (w, post); if (e.score < best.score) { best = std::move (e); bestV = w; } }
        addStage ("final", best, P.renders);
        // (0.2.1) early taps: the IR's strongest reflections of its first 100 ms as discrete taps (the attack a fitted ER spreads
        // out). The ER and the tail are rendered apart (the wet is their sum), so the ER's share (its level), the taps' count and
        // their share are searched on two renders; then the ER / tail levels and the build-up once more around them.
        if (! stop.load() && std::getenv ("IRFIT_NO_TAPS") == nullptr)
        {
            setText ("Early reflections");
            std::vector<float> eL, eR, qL, qR;
            { fit::Values w = bestV; w["tOn"] = 0; P.render (w, eL, eR, lenS); }
            { fit::Values w = bestV; w["erOn"] = 0; P.render (w, qL, qR, lenS); }
            const size_t nn = best.L.size();                       // (the ER alone renders shorter: pad with silence)
            eL.resize (nn, 0.0f); eR.resize (nn, 0.0f); qL.resize (nn, 0.0f); qR.resize (nn, 0.0f);
            if (std::getenv ("IRFIT_DEBUG"))
            {
                double d = 0, t = 0; for (size_t i = 0; i < nn; ++i) { const double x = best.L[i] - eL[i] - qL[i]; d += x * x; t += (double) best.L[i] * best.L[i]; }
                std::cerr << "ER + tail vs the whole: rel. error " << std::sqrt (d / std::max (1e-30, t)) << std::endl;
            }
            double eT = 0, eM = 0;
            for (size_t i = 0; i < tL.size(); ++i) eT += 0.5 * ((double) tL[i] * tL[i] + (double) tR[i] * tR[i]);
            for (size_t i = 0; i < best.L.size(); ++i) eM += 0.5 * ((double) best.L[i] * best.L[i] + (double) best.R[i] * best.R[i]);
            const double g0 = std::sqrt (eM / std::max (1e-30, eT));
            const int n = (int) tL.size(), on = (int) (tOn30 * 48.0), i0 = std::max (0, on - 48), i1 = std::min (n, on + (int) (0.100 * fit::kSR));
            std::vector<int> idx; for (int i = i0; i < i1; ++i) idx.push_back (i);
            std::sort (idx.begin(), idx.end(), [&] (int x, int y) { return (double) tL[x] * tL[x] + (double) tR[x] * tR[x] > (double) tL[y] * tL[y] + (double) tR[y] * tR[y]; });
            std::vector<int> pick;
            for (int i : idx) { bool ok = true; for (int j : pick) if (std::abs (i - j) < 15) { ok = false; break; } if (ok) pick.push_back (i); if ((int) pick.size() >= kMaxTaps) break; }
            auto withTaps = [&] (int K, double a) { PostSettings ps = post; ps.nTaps = std::min (K, (int) pick.size());
                for (int k = 0; k < ps.nTaps; ++k) { const int i = pick[(size_t) k]; ps.tapMs[(size_t) k] = (float) (i * 1000.0 / fit::kSR); ps.tapL[(size_t) k] = (float) (a * g0 * tL[(size_t) i]); ps.tapR[(size_t) k] = (float) (a * g0 * tR[(size_t) i]); }
                return ps; };
            std::vector<float> mL (nn), mR (nn);
            // the attack: 5 ms bins over the first 100 ms (dB re the whole energy), RMS difference in dB. The score's envelope is
            // 10 ms bins over 400 ms and hardly sees it; the ear does (a strong reflection at 20 - 40 ms is the attack's "body")
            auto early5 = [&] (const float* a, const float* b2, int len, double* out)
            {
                double tot = 0; for (int i = 0; i < len; ++i) tot += (double) a[i] * a[i] + (double) b2[i] * b2[i];
                for (int k = 0; k < 20; ++k) { double e = 0; for (int i = on + k * 240; i < on + (k + 1) * 240 && i < len; ++i) e += (double) a[i] * a[i] + (double) b2[i] * b2[i]; out[k] = std::max (-60.0, 10 * std::log10 (e / std::max (1e-30, tot) + 1e-12)); }
            };
            double tE[20]; early5 (tL.data(), tR.data(), (int) tL.size(), tE);
            auto earlyErr = [&] (const std::vector<float>& a, const std::vector<float>& b2, const PostSettings& ps)
            {
                std::vector<float> x = a, y = b2; Post::apply (ps, x.data(), y.data(), (int) x.size());
                double mE[20]; early5 (x.data(), y.data(), (int) x.size(), mE);
                double sq = 0; for (int k = 0; k < 20; ++k) { const double d = mE[k] - tE[k]; sq += d * d; } return std::sqrt (sq / 20);
            };
            const double lam = std::getenv ("IRFIT_EARLY_LAMBDA") ? std::atof (std::getenv ("IRFIT_EARLY_LAMBDA")) 
                                                                         : 1.0;
            const double err0 = earlyErr (best.L, best.R, post);
            double bObj = best.score + lam * err0, bErr = err0;
            Eval bt = best; PostSettings bp = post; int bK = 0; double bA = 0, bB = 1;
            for (double be : { 1.0, 0.85, 0.7, 0.5 })
            {
                for (size_t i = 0; i < nn; ++i) { mL[i] = (float) (be * eL[i]) + qL[i]; mR[i] = (float) (be * eR[i]) + qR[i]; }
                for (int K : { 16, 32, 64 }) for (double a : { 0.5, 0.7, 0.85, 1.0, 1.2, 1.4, 1.6 })
                {
                    if (stop.load()) break;
                    Eval e; const auto ps = withTaps (K, a); P.measureInto (e, ps, mL, mR);
                    const double ee = earlyErr (mL, mR, ps), obj = e.score + lam * ee;
                    if (std::getenv ("IRFIT_DEBUG")) std::cerr << "  ER x" << be << " K " << K << " a " << a << ": score " << e.score << " early " << ee << std::endl;
                    if (obj < bObj) { bObj = obj; bErr = ee; bt = std::move (e); bt.L = mL; bt.R = mR; bp = ps; bK = K; bA = a; bB = be; }
                }
            }
            if (std::getenv ("IRFIT_DEBUG")) std::cerr << "early taps: " << bK << " at " << bA << ", ER x" << bB << " (score " << best.score << " -> " << bt.score << ", attack " << err0 << " -> " << bErr << " dB)" << std::endl;
            if (bK > 0)
            {
                bestV["level"] = juce::jlimit (-24.0f, 12.0f, bestV["level"] + (float) (20 * std::log10 (bB)));
                post = bp;
                { const Eval chk = P.eval (bestV, post); if (std::getenv ("IRFIT_DEBUG")) std::cerr << "  (rendered with the ER level: " << chk.score << ")" << std::endl;
                  best = chk; }
                if (opts.polish && ! stop.load())
                {
                    std::vector<Polisher::GnP> ps = { { "energy", 1.5, -30, 12 }, { "tLvl", 1.5, -36, 12 }, { "decay", 3, -40, 24 }, { "tSpread", 15, 0, 300 }, { "tShape", 15, -100, 100 }, { "first", 3, 1, 80 } };
                    P.gnStep (bestV, best, ps, post);
                }
                if (std::getenv ("IRFIT_DEBUG")) std::cerr << "  after the GN step: score " << best.score << " attack " << earlyErr (best.L, best.R, post) << " dB" << std::endl;
            }
            addStage ("early-taps", best, P.renders);
        }
    }
    if (stop.load()) return;
    // the level: the shape is matched, the loudness goes to the plug-in's reference (as the Lab's fit does)
    {
        std::vector<float> L = best.L, R = best.R; Post::apply (post, L.data(), R.data(), (int) L.size());
        double e = 0; for (size_t i = 0; i < L.size(); ++i) e += 0.5 * ((double) L[i] * L[i] + (double) R[i] * R[i]);
        const float d = (float) juce::jlimit (-30.0, 30.0, ERProcessor::kRefWetDb - 10 * std::log10 (e + 1e-20));
        bestV["level"] = juce::jlimit (-24.0f, 12.0f, bestV["level"] + d); bestV["tLvl"] = juce::jlimit (-36.0f, 12.0f, bestV["tLvl"] + d);
        { const float g = std::pow (10.0f, d / 20.0f); for (int k = 0; k < post.nTaps; ++k) { post.tapL[(size_t) k] *= g; post.tapR[(size_t) k] *= g; } }     // (the taps with the wet)
        P.clampAll (bestV);
    }
    setText ("Measuring the model");
    const Eval fin = P.eval (bestV, post);
    addStage ("result", fin, P.renders);
    if (std::getenv ("IRFIT_DEBUG"))
    {
        for (size_t i = 0; i < 30 && i < tm.env.size(); ++i) std::cerr << "env " << i * 10 << " ms IR " << tm.env[i] << " model " << fin.m.env[i] << std::endl;
        std::cerr << "edt IR"; for (auto x : tm.edt) std::cerr << " " << x; std::cerr << " | model"; for (auto x : fin.m.edt) std::cerr << " " << x; std::cerr << std::endl;
        std::cerr << "t30 IR"; for (auto x : tm.t30) std::cerr << " " << x; std::cerr << " | model"; for (auto x : fin.m.t30) std::cerr << " " << x; std::cerr << std::endl;
    }
    // ---- the model: every parameter, its features (product convention, 8 s render), the ER / tail energies, the anchor
    UserModel m; m.name = irName; m.post = post;
    {   // (0.3.0) the IR itself with it (the hybrid), at the same reference energy as the model
        auto d = std::make_shared<IRData>(); d->L = tL; d->R = tR; d->name = irName;
        normaliseIR (*d, ERProcessor::kRefWetDb); analyseIR (*d); m.ir = std::move (d);
    }
    for (auto& kv : bestV) m.eng.push_back ({ juce::String (kv.first), kv.second });
    std::sort (m.eng.begin(), m.eng.end(), [] (auto& a, auto& b) { return a.first < b.first; });
    m.fillCoreFromEng();
    auto energy = [&] (const fit::Values& vv, double* c80 = nullptr, double* on = nullptr, fit::Metrics* mm = nullptr, bool taps = true)
    {
        std::vector<float> L, R; P.render (vv, L, R, 8.0); { PostSettings ps = post; if (! taps) ps.nTaps = 0; Post::apply (ps, L.data(), R.data(), (int) L.size()); }
        double e = 0; for (size_t i = 0; i < L.size(); ++i) e += 0.5 * ((double) L[i] * L[i] + (double) R[i] * R[i]);
        if (c80) *c80 = c80FromOnset (L.data(), R.data(), (int) L.size());
        if (on) *on = onsetMs (L.data(), R.data(), (int) L.size(), 30);
        if (mm) *mm = fit::measure (L.data(), R.data(), (int) L.size());
        return 10 * std::log10 (e + 1e-30);
    };
    double c80 = 0, on = 0; fit::Metrics mm;
    energy (bestV, &c80, &on, &mm);
    { fit::Values w = bestV; w["tOn"] = 0; m.core.eEr = energy (w); }
    { fit::Values w = bestV; w["erOn"] = 0; m.core.eTail = energy (w, nullptr, nullptr, nullptr, false); }      // (the taps are early: not the tail's)
    if (! featOf (mm.t30, mm.edt, c80, m.feat)) { error = "The model could not be measured"; return; }
    m.onset = (float) on;
    {
        Macros A; const auto& F = m.feat;
        A.decay = (float) juce::jlimit (0.2, 15.0, std::exp (F.lt));
        A.tone = (float) juce::jlimit (-100.0, 100.0, (F.tilt - (kTiltA + kTiltB * F.lt)) / kToneScale);
        A.early = (float) juce::jlimit (-24.0, 24.0, F.c80 - (kC80A + kC80B * F.lt));
        A.buildUp = (float) juce::jlimit (-100.0, 100.0, (F.edtr - (kEdtrA + kEdtrB * F.lt)) / kBuildScale);
        A.preDelay = (float) juce::jlimit (0.0, 200.0, (double) m.onset);
        A.size = 100; A.density = 0; A.width = 100; A.lowCut = 20; A.highCut = 20000;
        m.core.anchor = A;
    }
    auto& I = m.info; I.score = fin.score; I.labScore = labScore; I.t30 = fin.parts.t30 * 5; I.edt = fin.parts.edt * 8; I.c80 = fin.parts.c80 * 1.5; I.env = fin.parts.env * 2;
    I.specL = fin.parts.specL; I.specE = fin.parts.specE; I.iacc = fin.parts.iacc; I.ned = fin.parts.ned; I.seconds = elapsedS(); I.renders = P.renders; I.source = irName;
    result = std::move (m);
}
}
