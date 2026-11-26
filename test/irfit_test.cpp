// IR Fit Reverb: checks of the product processor (offline renders measured with the plug-in's own fit::measure)
//   IRFitTest repro <params.csv>   every library model: the product at that model's knob positions vs the model itself
//   IRFitTest sweep                each macro swept from a few programs: T30 / EDT / C80 / tilt / onset / NED / level
//   IRFitTest rt                   silence, full scale, 44.1 / 48 / 96 kHz: NaN / Inf, clicks while knobs move, CPU
//   IRFitTest map                  the mapping alone (neighbours, weights) for the knob defaults
#include "IRFitProcessor.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <tuple>
#include <thread>
using namespace juce;

static fit::Metrics renderMeasure (ERProcessor& e, double* energyDb = nullptr, double* onsetMs = nullptr)
{
    ERProcessor::ExportOpts o; o.sr = 48000; o.bits = 32; o.tail = true; o.normalize = false;
    AudioBuffer<float> b; double g = 0;
    ERProcessor::tlMaxLenS = 8.0; e.renderIR (o, b, g); ERProcessor::tlMaxLenS = 0;
    const int n = b.getNumSamples(); const float* L = b.getReadPointer (0); const float* R = b.getReadPointer (1);
    if (energyDb) { double s = 0; for (int i = 0; i < n; ++i) s += 0.5 * ((double) L[i] * L[i] + (double) R[i] * R[i]); *energyDb = 10 * std::log10 (s + 1e-30); }
    if (onsetMs)
    {   // the first sample within 30 dB of the peak power (as ETest's metrics)
        double pk = 0; for (int i = 0; i < n; ++i) pk = std::max (pk, (double) L[i] * L[i] + (double) R[i] * R[i]);
        *onsetMs = 0; for (int i = 0; i < n; ++i) if ((double) L[i] * L[i] + (double) R[i] * R[i] >= pk * 1e-3) { *onsetMs = i / 48.0; break; }
    }
    auto m = fit::measure (L, R, n);
    m.c80 = irfit::c80FromOnset (L, R, n);            // (the product's C80: from the arrival)
    return m;
}
static double t30m (const fit::Metrics& m) { double s = 0; int k = 0; for (int i : { 2, 3, 4 }) if (std::isfinite (m.t30[i]) && m.t30[i] > 0) { s += m.t30[i]; ++k; } return k ? s / k : NAN; }
static double tiltOf (const fit::Metrics& m) { return std::log (m.t30[5] / m.t30[1]); }
static double edtrOf (const fit::Metrics& m) { return std::log (m.edt[3] / t30m (m)); }
static void setM (IRFitProcessor& p, const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); }

static int repro (const char* csvPath)
{
    StringArray lines; lines.addLines (File (csvPath).loadFileAsString()); lines.removeEmptyStrings();
    StringArray head; head.addTokens (lines[0], ",", "\"");
    IRFitProcessor p; ERProcessor ref;
    std::cout << "entry,name,inPool,score,dT30pct,dEDTpct,dC80,dOnset,dEnergy,maxParamDiff,worstParam\n";
    std::vector<double> scores, dT, dE, dC;
    for (int r = 1; r < lines.size(); ++r)
    {
        const int e = r - 1; if (e >= irfit::kNumEntries) break;
        // the model itself, as the study rendered it: Init - Algo + every column
        StringArray v; v.addTokens (lines[r], ",", "\"");
        ref.applyPreset (presetIndex ("Init — Algo"));
        for (int c = 6; c < head.size(); ++c) ref.setParam (head[c].toRawUTF8(), v[c].getFloatValue());
        double eR = 0, oR = 0; const auto mR = renderMeasure (ref, &eR, &oR);
        // the product at the model's knob positions
        p.loadModel (e); p.pushNow();
        double eP = 0, oP = 0; const auto mP = renderMeasure (p.engine(), &eP, &oP);
        // the engine parameters that differ
        double worst = 0; String wid;
        for (int c = 6; c < head.size(); ++c)
        {
            const auto id = head[c]; if (id == "mix" || id == "autoLvl") continue;
            auto* q = p.engine().apvts.getParameter (id); if (q == nullptr) continue;
            const double a = p.engine().value (id.toRawUTF8()), b = ref.value (id.toRawUTF8()), d = std::abs (a - b) / (1e-3 + std::abs (b));
            if (d > worst) { worst = d; wid = id + "=" + String (a, 4) + "/" + String (b, 4); }
        }
        fit::Parts pp; const double sc = fit::compare (mR, mP, &pp);
        const bool pool = ! irfit::kEntries[e].shortIr;
        std::cout << e << ",\"" << irfit::kEntries[e].name << "\"," << pool << "," << sc << "," << pp.t30 * 5 << "," << pp.edt * 8 << "," << (mP.c80 - mR.c80) << "," << (oP - oR) << "," << (eP - eR) << "," << worst << "," << wid << "\n";
        if (pool) { scores.push_back (sc); dT.push_back (pp.t30 * 5); dE.push_back (pp.edt * 8); dC.push_back (std::abs (mP.c80 - mR.c80)); }
    }
    auto mx = [] (std::vector<double> a) { return a.empty() ? 0.0 : *std::max_element (a.begin(), a.end()); };
    std::cout << "SUMMARY pool " << scores.size() << " max score " << mx (scores) << " max T30 " << mx (dT) << " % max EDT " << mx (dE) << " % max |dC80| " << mx (dC) << " dB\n";
    return 0;
}

static int sweep (bool withRefine)
{
    IRFitProcessor p;
    struct Prog { const char* name; } progs[] = { { "Hall 2.2 s Neutral '10s" }, { "Plate 1.9 s Neutral '80s" }, { "Chamber 1.2 s Neutral '10s" }, { "Concert Hall '10s" }, { "Room 1.0 s Neutral '80s" } };
    struct Sw { const char* id; std::vector<float> v; } sws[] = {
        { "decay", { 0.3f, 0.5f, 0.8f, 1.2f, 1.8f, 2.5f, 3.5f, 5.0f, 7.0f, 10.0f } },
        { "tone", { -100, -60, -30, 0, 30, 60, 100 } },
        { "early", { -18, -12, -6, 0, 6, 12, 18 } },
        { "buildup", { -100, -60, -30, 0, 30, 60, 100 } },
        { "size", { 50, 70, 100, 140, 200 } },
        { "density", { -100, -50, 0, 50, 100 } },
        { "predelay", { 0, 5, 10, 20, 40, 80, 150 } },
        { "width", { 0, 50, 100, 150 } } };
    std::cout << "prog,macro,value,T30m,EDT1k,C80,tilt,edtr,onset,ned50,ned100,energy,iacc1k,n1,w1,n2,w2,tLt,tTilt,tC80,tEdtr\n";
    for (auto& pr : progs)
    {
        int e = -1; for (int i = 0; i < irfit::kNumEntries; ++i) if (String (irfit::kEntries[i].name) == pr.name) e = i;
        if (e < 0) { std::cout << "missing " << pr.name << "\n"; continue; }
        for (auto& s : sws)
        {
            for (float v : s.v)
            {
                p.loadModel (e); setM (p, "width", 100); setM (p, "size", 100); setM (p, "density", 0); setM (p, s.id, v); p.pushNow();
                if (withRefine) { const auto t = p.refine (p.macros(), p.trim(), 4); p.setTrim (t); p.pushNow(); }
                double en = 0, on = 0; const auto m = renderMeasure (p.engine(), &en, &on);
                irfit::Result r; irfit::map (p.macros(), r);
                std::cout << "\"" << pr.name << "\"," << s.id << "," << v << "," << t30m (m) << "," << m.edt[3] << "," << m.c80 << "," << tiltOf (m) << "," << edtrOf (m) << "," << on << ","
                          << m.ned[1] << "," << m.ned[2] << "," << en << "," << m.iacc[2] << ",\"" << irfit::kEntries[r.nn[0]].name << "\"," << r.w[0] << ",\"" << irfit::kEntries[r.nn[1]].name << "\"," << r.w[1] << "," << r.tgt.lt << "," << r.tgt.tilt << "," << r.tgt.c80 << "," << r.tgt.edtr << "\n";
            }
        }
    }
    return 0;
}

static int rt()
{
    int fails = 0;
    auto check = [&] (bool ok, const String& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl; if (! ok) ++fails; };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const int bs = 256;
        std::cout << "== " << sr << " Hz, block " << bs << std::endl;
        IRFitProcessor p; p.setRateAndBufferSizeDetails (sr, bs); p.prepareToPlay (sr, bs);
        AudioBuffer<float> b (2, bs); MidiBuffer midi; Random rnd (7);
        long t = 0; float y1 = 0, y2 = 0; int worstAt = 0;          // (continuous across runs: the input and the 2nd difference never restart)
        auto run = [&] (int blocks, std::function<float (int)> src, std::function<void (int)> each, float* peak = nullptr, bool* finite = nullptr, double* worstD2 = nullptr, int skip = 0)
        {
            float pk = 0; bool fin = true; double w = 0;
            for (int k = 0; k < blocks; ++k)
            {
                if (each) each (k);
                for (int i = 0; i < bs; ++i) { const float x = src ((int) (t + i)); b.setSample (0, i, x); b.setSample (1, i, x); }
                p.processBlock (b, midi);
                for (int i = 0; i < bs; ++i)
                {
                    const float y = b.getSample (0, i), yr = b.getSample (1, i);
                    if (! std::isfinite (y) || ! std::isfinite (yr)) fin = false;
                    pk = std::max (pk, std::max (std::abs (y), std::abs (yr)));
                    const double d2 = std::abs (y - 2 * y1 + y2); y2 = y1; y1 = y; if (k >= skip && d2 > w) { w = d2; worstAt = k; }
                }
                t += bs;
            }
            if (peak) *peak = pk; if (finite) *finite = fin; if (worstD2) *worstD2 = w;
        };
        const int sec = (int) (sr / bs);
        {   // silence in: silence out (after the tail of nothing)
            float pk = 1; bool fin = false; run (2 * sec, [] (int) { return 0.0f; }, nullptr, &pk, &fin);
            check (fin && pk < 1e-6f, "silence: output peak " + String (pk, 8));
        }
        {   // full scale noise and a full-scale impulse train at the longest settings
            setM (p, "decay", 15.0f); setM (p, "size", 200); setM (p, "tone", 100); setM (p, "buildup", 100); setM (p, "density", 100); setM (p, "mix", 100);
            float pk = 0; bool fin = false; run (6 * sec, [&] (int) { return rnd.nextFloat() * 2 - 1; }, nullptr, &pk, &fin);
            check (fin && pk < 30.0f, "full-scale noise, decay 15 s / size 200 / tone +100 / build-up +100: finite, peak " + String (pk, 2));
            run (4 * sec, [&] (int i) { return (i % (int) (sr / 4)) == 0 ? 1.0f : 0.0f; }, nullptr, &pk, &fin);
            check (fin && pk < 30.0f, "full-scale impulses: finite, peak " + String (pk, 2));
            setM (p, "decay", 0.2f); setM (p, "size", 50); setM (p, "tone", -100); setM (p, "buildup", -100); setM (p, "density", -100); setM (p, "early", 24);
            run (3 * sec, [&] (int) { return rnd.nextFloat() * 2 - 1; }, nullptr, &pk, &fin);
            check (fin && pk < 30.0f, "full-scale noise at the other extremes: finite, peak " + String (pk, 2));
            check (! p.nonFinite.load(), "the NaN guard never fired");
        }
        {   // clicks: a sine under knob moves vs the same sine with the knobs still (max 2nd difference)
            auto sine = [&] (int i) { return 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 220.0 * i / sr); };
            p.loadModel (40); setM (p, "mix", 50); setM (p, "early", 0);
            double still = 0; run (3 * sec, sine, nullptr, nullptr, nullptr, &still, sec);
            struct Move { const char* name; std::function<void (int)> f; };
            const int N = 3 * sec;
            Move moves[] = {
                { "decay 0.4 -> 10 s", [&] (int k) { setM (p, "decay", 0.4f * std::pow (25.0f, (float) k / N)); } },
                { "size 50 -> 200 %", [&] (int k) { setM (p, "size", 50 + 150.0f * k / N); } },
                { "tone -100 -> +100", [&] (int k) { setM (p, "tone", -100 + 200.0f * k / N); } },
                { "early -18 -> +18 dB", [&] (int k) { setM (p, "early", -18 + 36.0f * k / N); } },
                { "build-up -100 -> +100", [&] (int k) { setM (p, "buildup", -100 + 200.0f * k / N); } },
                { "density -100 -> +100", [&] (int k) { setM (p, "density", -100 + 200.0f * k / N); } },
                { "pre-delay 0 -> 150 ms", [&] (int k) { setM (p, "predelay", 150.0f * k / N); } },
                { "width / cuts", [&] (int k) { setM (p, "width", 150.0f * k / N); setM (p, "lowcut", 20 + 500.0f * k / N); setM (p, "highcut", 20000 - 17000.0f * k / N); } },
                { "character steps (every 0.5 s)", [&] (int k) { setM (p, "character", (float) ((k / (sec / 2)) % 6)); } },
                { "program loads (every 0.5 s)", [&] (int k) { if (k % (sec / 2) == 0) p.loadModel ((k / (sec / 2) * 17) % irfit::kNumEntries); } },
            };
            std::cout << "  sine alone (knobs still): max 2nd diff " << still << std::endl;
            for (auto& mv : moves)
            {
                p.loadModel (40); setM (p, "mix", 50); run (sec, sine, nullptr);
                double w = 0; run (N, sine, mv.f, nullptr, nullptr, &w);
                check (w < 4 * still, String ("click test, ") + mv.name + ": max 2nd diff " + String (w, 5) + " (x" + String (w / still, 2) + " of still, block " + String (worstAt) + ")");
            }
        }
        {   // CPU: 10 s of noise, knobs still, then the same with Decay moving (a mapping push every 10 ms)
            p.loadModel (40); setM (p, "mix", 50);
            AudioBuffer<float> nb (2, bs); for (int c = 0; c < 2; ++c) for (int i = 0; i < bs; ++i) nb.setSample (c, i, rnd.nextFloat() - 0.5f);
            auto bench = [&] (bool moving) {
                const int blocks = (int) (10 * sr / bs);
                const auto t0 = std::chrono::steady_clock::now();
                for (int k = 0; k < blocks; ++k) { if (moving) setM (p, "decay", 1.0f + 3.0f * (float) (k % 400) / 400.0f); b.makeCopyOf (nb, true); p.processBlock (b, midi); }
                return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 10.0 * 100.0; };
            const double still = bench (false), moving = bench (true);
            std::cout << "  CPU (one core, % of real time): knobs still " << String (still, 2) << " %, Decay moving " << String (moving, 2) << " %" << std::endl;
        }
    }
    std::cout << (fails ? "FAILED " : "ALL OK ") << fails << std::endl;
    return fails;
}

// probing: load <entry> | m <macro> <value> | e <engine id> <value> (after the mapping) | push | measure | quit
static int serve()
{
    IRFitProcessor p; std::string line;
    while (std::getline (std::cin, line))
    {
        StringArray t; t.addTokens (String (line), " ", "\""); t.removeEmptyStrings(); if (t.isEmpty()) continue;
        if (t[0] == "quit") break;
        else if (t[0] == "load") { p.loadModel (t[1].getIntValue()); p.pushNow(); std::cout << "ok" << std::endl; }
        else if (t[0] == "m") { setM (p, t[1].toRawUTF8(), t[2].getFloatValue()); p.pushNow(); std::cout << "ok" << std::endl; }
        else if (t[0] == "e") { p.engine().setParam (t[1].toRawUTF8(), t[2].getFloatValue()); std::cout << "ok" << std::endl; }
        else if (t[0] == "refine") { double err = 0; const auto tr = p.refine (p.macros(), p.trim(), t.size() > 1 ? t[1].getIntValue() : 3, &err); p.setTrim (tr); p.pushNow();
                                     std::cout << "ok err=" << err << " trim " << tr.lt << " " << tr.tilt << " " << tr.c80 << " " << tr.edtr << std::endl; }
        else if (t[0] == "get") { std::cout << p.engine().value (t[1].toRawUTF8()) << std::endl; }
        else if (t[0] == "measure")
        {
            double en = 0, on = 0; const auto m = renderMeasure (p.engine(), &en, &on);
            std::cout << "measure T30m=" << t30m (m) << " EDT1k=" << m.edt[3] << " C80=" << m.c80 << " tilt=" << tiltOf (m) << " edtr=" << edtrOf (m) << " onset=" << on
                      << " ned20=" << m.ned[0] << " ned50=" << m.ned[1] << " ned100=" << m.ned[2] << " energy=" << en << " iacc1k=" << m.iacc[2] << std::endl;
        }
        else std::cout << "?" << std::endl;
    }
    return 0;
}

// which engine parameter clicks? a knob move rendered with the mapping applied by hand, one parameter held at a time
static int diag (const char* macro, float from, float to)
{
    const double sr = 48000; const int bs = 256, N = 3 * (int) (sr / bs);
    auto runOne = [&] (const String& hold) -> double
    {
        IRFitProcessor p; p.loadModel (40); setM (p, "mix", 50); p.setRateAndBufferSizeDetails (sr, bs); p.prepareToPlay (sr, bs);
        auto& e = p.engine(); AudioBuffer<float> b (2, bs); MidiBuffer midi; long t = 0; float y1 = 0, y2 = 0; double w = 0;
        irfit::Result r0; irfit::map (p.macros(), r0);
        for (int k = 0; k < N + 200; ++k)
        {
            if (k >= 100 && k < 100 + N) setM (p, macro, from + (to - from) * (float) (k - 100) / N);
            irfit::Result r; irfit::map (p.macros(), r);
            for (int i = 0; i < r.n; ++i)
            {
                const bool only = hold.startsWith ("+"), isIt = String (r.v[i].first) == hold.trimCharactersAtStart ("+");
                e.setParam (r.v[i].first, (only ? ! isIt : isIt) ? r0.v[i].second : r.v[i].second);
            }
            for (int i = 0; i < bs; ++i) { const float x = 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 220.0 * (double) (t + i) / sr); b.setSample (0, i, x); b.setSample (1, i, x); }
            t += bs; e.processBlock (b, midi);
            for (int i = 0; i < bs; ++i) { const float y = b.getSample (0, i); const double d2 = std::abs (y - 2 * y1 + y2); y2 = y1; y1 = y; if (k > 60 && d2 > w) w = d2; }
        }
        return w;
    };
    std::cout << "all moving: " << runOne ("") << std::endl;
    {   // the product's own push (rate-limited build-up)
        IRFitProcessor p; p.loadModel (40); setM (p, "mix", 50); p.setRateAndBufferSizeDetails (sr, bs); p.prepareToPlay (sr, bs);
        AudioBuffer<float> b (2, bs); MidiBuffer midi; long t = 0; float y1 = 0, y2 = 0; double w = 0;
        for (int k = 0; k < N + 200; ++k)
        {
            if (k >= 100 && k < 100 + N) setM (p, macro, from + (to - from) * (float) (k - 100) / N);
            for (int i = 0; i < bs; ++i) { const float x = 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 220.0 * (double) (t + i) / sr); b.setSample (0, i, x); b.setSample (1, i, x); }
            t += bs; p.processBlock (b, midi);
            for (int i = 0; i < bs; ++i) { const float y = b.getSample (0, i); const double d2 = std::abs (y - 2 * y1 + y2); y2 = y1; y1 = y; if (k > 60 && d2 > w) w = d2; }
        }
        std::cout << "product: " << w << std::endl;
        if (getenv ("PRODUCT_ONLY")) return 0;
    }
    irfit::Result r; irfit::map (irfit::Macros(), r);
    for (int i = 0; i < r.n; ++i) std::cout << "hold " << r.v[i].first << ": " << runOne (r.v[i].first) << "   only it moving: " << runOne (String ("+") + r.v[i].first) << std::endl;
    return 0;
}


// ---------------------------------------------------------------------------------------------------------------------
// (0.2.0) IR -> ALGO, presets, model playback
// ---------------------------------------------------------------------------------------------------------------------
static fit::Metrics targetOf (const File& f, double& lenS)
{
    std::vector<float> tL, tR; fit::loadTarget (f, fit::pairOf (f), tL, tR);
    lenS = jlimit (0.5, 8.0, tL.size() / fit::kSR + 0.2);
    return fit::measure (tL.data(), tR.data(), (int) tL.size());
}
// the product's own playback (engine after the mapping + post stage) scored against an IR
static double productScore (IRFitProcessor& p, const fit::Metrics& tm, double lenS, fit::Parts* pp = nullptr)
{
    AudioBuffer<float> b; p.renderWetIR (b, lenS);
    const auto m = fit::measure (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
    return fit::compare (tm, m, pp);
}
static bool runConversion (IRFitProcessor& p, const File& ir, double& secs)
{
    const auto t0 = Time::getMillisecondCounterHiRes();
    const auto err = p.convertIR (ir);
    if (err.isNotEmpty()) { std::cout << "convert failed: " << err << std::endl; return false; }
    String lastText;
    while (p.converter() != nullptr && p.converter()->running())
    {
        Thread::sleep (25); p.pollForTests();
        const auto t = p.converter()->stageText();
        if (t != lastText && ! t.contains ("%")) { std::cout << "   [" << String ((Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) << " s] " << t << std::endl; lastText = t; }
    }
    p.pollForTests();
    secs = (Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    return p.converter() != nullptr && p.converter()->state.load() == irfit::Converter::done;
}
static void applyFlags (irfit::Converter::Options& o, const StringArray& flags)
{
    o.drivePoll = true;
    if (flags.contains ("nopost")) o.post = false;
    if (flags.contains ("nopolish")) o.polish = false;
    if (flags.contains ("noned")) o.nedParams = false;
    if (flags.contains ("fast")) o.fast = true;
    for (auto& f : flags) if (f.startsWith ("fk=")) { o.fast = true; o.fastScale = f.fromFirstOccurrenceOf ("=", false, false).getDoubleValue(); }
    for (auto& f : flags) if (f.startsWith ("gn=")) o.gnIters = f.fromFirstOccurrenceOf ("=", false, false).getIntValue();
}
// convert <ir> [flags...]: the whole conversion as the plug-in runs it, the score per stage, then the product's playback of the model
static int convertCmd (const File& ir, const StringArray& flags, std::ostream* csv = nullptr, const String& tag = {})
{
    IRFitProcessor p; applyFlags (p.convOptions, flags);
    double secs = 0; const bool ok = runConversion (p, ir, secs);
    if (! ok) { std::cout << "FAILED " << ir.getFileName() << " " << (p.converter() ? p.converter()->error : String()) << std::endl; if (csv) *csv << tag << ",\"" << ir.getFileName() << "\",FAILED\n"; return 1; }
    const auto st = p.converter()->stages();
    for (auto& s : st) std::cout << "   " << s.stage.paddedRight (' ', 11) << " score " << String (s.score, 2).paddedLeft (' ', 7) << "  | " << irfit::partsText (s.parts) << "  (" << s.renders << " renders, " << String (s.seconds, 1) << " s)" << std::endl;
    const auto& M = p.currentModel();
    double lenS = 0; const auto tm = targetOf (ir, lenS);
    p.pushNow(); fit::Parts pp; const double ps = productScore (p, tm, lenS, &pp);
    // the engine at the anchor = the model's own values?
    double worst = 0; String wid;
    for (auto& kv : M.eng)
    {
        if (kv.first == "mix" || kv.first == "out" || kv.first == "autoLvl") continue;
        const float a = p.engine().value (kv.first.toRawUTF8()); auto* q = p.engine().apvts.getParameter (kv.first);
        const float b = q ? q->convertFrom0to1 (q->convertTo0to1 (kv.second)) : kv.second;
        const double d = std::abs (a - b); if (d > worst) { worst = d; wid = kv.first + "=" + String (a, 5) + "/" + String (b, 5); }
    }
    { auto& B = rv::flatbu::Designer::stats(); std::cout << "   build-up designs " << B.n.load() << " (" << String (B.us.load() / 1e6, 1) << " s), cache hits " << B.hits.load() << std::endl; }
    std::cout << "RESULT " << ir.getFileName() << ": lab " << String (M.info.labScore, 2) << " -> model " << String (M.info.score, 2) << " | product playback " << String (ps, 2)
              << " | " << irfit::partsText (pp) << " | " << String (secs, 1) << " s (Lab stage " << String (p.converter()->labSeconds, 1) << " s) | anchor max |param diff| " << worst << " " << wid << std::endl;
    std::cout << "   anchor: decay " << M.core.anchor.decay << " tone " << M.core.anchor.tone << " early " << M.core.anchor.early << " build-up " << M.core.anchor.buildUp << " pre-delay " << M.core.anchor.preDelay
              << " | preset " << p.current.file.getFullPathName() << std::endl;
    if (csv)
    {
        auto partsCsv = [] (const fit::Parts& q) { return String (q.t30 * 5, 3) + "," + String (q.edt * 8, 3) + "," + String (q.c80 * 1.5, 3) + "," + String (q.env * 2, 3) + "," + String (q.specL, 3) + "," + String (q.specE, 3) + "," + String (q.iacc, 3) + "," + String (q.ned, 3); };
        for (auto& s : st) *csv << tag << ",\"" << ir.getFileName() << "\"," << s.stage << "," << s.score << "," << partsCsv (s.parts) << "," << s.seconds << "," << s.renders << "\n";
        *csv << tag << ",\"" << ir.getFileName() << "\",product," << ps << "," << partsCsv (pp) << "," << secs << ",0\n";
        csv->flush();
    }
    return 0;
}
// (0.3.1) batch <folder> [fast|std] [par=N] [nolab]: the batch as the plug-in runs it, then
//  every preset loads and plays its model at the anchor; every Lab file loads into a plain Lab engine with the same engine values;
//  a second run skips everything
static int batchCmd (const File& dir, const StringArray& flags)
{
    IRFitProcessor p; p.setRateAndBufferSizeDetails (48000, 256); p.prepareToPlay (48000, 256); applyFlags (p.convOptions, flags);
    irfit::Batch::Settings s; s.fast = ! flags.contains ("std"); s.toLab = ! flags.contains ("nolab");
    for (auto& f : flags) if (f.startsWith ("par=")) s.parallel = f.fromFirstOccurrenceOf ("=", false, false).getIntValue();
    const auto t0 = Time::getMillisecondCounterHiRes();
    if (auto err = p.startBatch (dir, s); err.isNotEmpty()) { std::cout << "batch: " << err << std::endl; return 1; }
    double lastPrint = 0;
    while (p.batchState().running())
    {
        Thread::sleep (25); p.pollForTests();
        const double t = (Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        if (t - lastPrint > 30) { lastPrint = t; const auto& b = p.batchState();
            std::cout << "   [" << String (t, 0) << " s] " << String (b.progress() * 100, 1) << " %  running " << b.count (irfit::Batch::jRunning) << "  done " << b.count (irfit::Batch::jDone)
                      << "/" << b.total() << "  eta " << String (b.etaS(), 0) << " s" << std::endl; }
    }
    const double secs = (Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    std::cout << p.lastBatchReport << "  (" << String (secs, 1) << " s)" << std::endl;
    bool ok = p.batchState().count (irfit::Batch::jFailed) == 0;
    for (auto& j : p.batchState().list())
    {
        std::cout << "   " << j.name.paddedRight (' ', 34) << " " << (j.state == irfit::Batch::jDone ? "done" : j.state == irfit::Batch::jSkipped ? "skipped" : j.state == irfit::Batch::jFailed ? "FAILED" : "other")
                  << "  score " << String (j.score, 2) << "  " << String (j.seconds, 0) << " s  " << j.note << std::endl;
        if (j.state != irfit::Batch::jDone && j.state != irfit::Batch::jSkipped) continue;
        const auto pf = irfit::PresetBank::root().getChildFile (irfit::PresetBank::legalName (j.folder)).getChildFile (irfit::PresetBank::legalName (j.name) + irfit::PresetBank::kExt);
        auto px = XmlDocument::parse (pf);
        IRFitProcessor q; q.setRateAndBufferSizeDetails (48000, 256); q.prepareToPlay (48000, 256);
        if (px == nullptr || ! q.applyStateXml (*px, true) || ! q.currentModel().valid()) { std::cout << "      FAIL preset does not load: " << pf.getFullPathName() << std::endl; ok = false; continue; }
        double worst = 0; String wid;
        for (auto& kv : q.currentModel().eng)
        {
            if (kv.first == "mix" || kv.first == "out" || kv.first == "autoLvl") continue;
            auto* qq = q.engine().apvts.getParameter (kv.first); if (qq == nullptr) continue;
            const double d = std::abs (q.engine().value (kv.first.toRawUTF8()) - qq->convertFrom0to1 (qq->convertTo0to1 (kv.second)));
            if (d > worst) { worst = d; wid = kv.first; }
        }
        const bool atAnchor = worst < 1e-3;
        std::cout << "      preset loads, engine at the model " << (atAnchor ? "OK" : "FAIL") << " (max diff " << worst << " " << wid << ")" << std::endl; ok = ok && atAnchor;
        if (! s.toLab) continue;
        const auto lf = irfit::labIrFitDir (j.folder).getChildFile (irfit::PresetBank::legalName (j.name) + ".rvpreset");
        ERProcessor lab; lab.setRateAndBufferSizeDetails (48000, 512); lab.prepareToPlay (48000, 512);
        if (! lab.loadUserPreset (lf)) { std::cout << "      FAIL Lab preset does not load: " << lf.getFullPathName() << std::endl; ok = false; continue; }
        double lw = 0; String lid; int n = 0;
        for (auto* pr : lab.getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (pr))
            {
                const auto id = rp->getParameterID(); if (id == "mix" || id == "out" || id == "outByp" || id == "outWet" || id == "autoLvl") continue;
                const double d = std::abs (lab.value (id.toRawUTF8()) - q.engine().value (id.toRawUTF8())); ++n;
                if (d > lw) { lw = d; lid = id; }
            }
        auto lx = XmlDocument::parse (lf); auto* post = lx ? lx->getChildByName ("IRFITPOST") : nullptr;
        StringArray taps; if (post) taps.addTokens (post->getStringAttribute ("taps"), ",", "");
        const bool labOk = lw < 1e-3 && (post != nullptr) == ! q.currentModel().post.isNeutral() && (post == nullptr || taps.size() == q.currentModel().post.nTaps);
        std::cout << "      Lab preset: " << n << " parameters, max diff " << lw << " " << lid << ", post " << (post ? "yes" : "no") << ", taps " << taps.size() << "  " << (labOk ? "OK" : "FAIL") << std::endl;
        ok = ok && labOk;
    }
    {   // the same folder again: everything is there
        IRFitProcessor r; r.setRateAndBufferSizeDetails (48000, 256); r.prepareToPlay (48000, 256); applyFlags (r.convOptions, flags);
        const auto t1 = Time::getMillisecondCounterHiRes();
        r.startBatch (dir, s); while (r.batchState().running()) { Thread::sleep (5); r.pollForTests(); }
        const double t = (Time::getMillisecondCounterHiRes() - t1) / 1000.0;
        const bool rOk = r.batchState().count (irfit::Batch::jSkipped) == p.batchState().count (irfit::Batch::jDone) + p.batchState().count (irfit::Batch::jSkipped) && t < 5;
        std::cout << "   again: " << r.lastBatchReport << " in " << String (t, 2) << " s  " << (rOk ? "OK" : "FAIL") << std::endl; ok = ok && rOk;
    }
    std::cout << (ok ? "BATCH ALL OK" : "BATCH FAIL") << std::endl;
    return ok ? 0 : 1;
}
// reso <preset.xml...>: narrow resonances of the ALGO playback vs the model's own IR. Welch spectra (8192 Hann, hop 2048, L + R) of the
// tail [onset + t0, + len], the excess over its 1/3-octave smoothing, 80 Hz - 10 kHz: p99, the largest peaks, the count above 6 dB
struct Reso { double p99 = 0, mx = 0, fmx = 0; int n6 = 0; std::vector<std::pair<double, double>> top; std::vector<double> ex, frq; };
static Reso resoOf (const float* L, const float* R, int n, double t0, double len)
{
    const double sr = 48000; int on = 0; float pk = 0;
    for (int i = 0; i < n; ++i) pk = std::max (pk, std::max (std::abs (L[i]), std::abs (R[i])));
    for (int i = 0; i < n; ++i) if (std::max (std::abs (L[i]), std::abs (R[i])) > pk * 0.0316f) { on = i; break; }
    const int a = on + (int) (t0 * sr), b = std::min (n, a + (int) (len * sr));
    const int N = 8192, H = 2048; juce::dsp::FFT fft (13);
    std::vector<double> P (N / 2 + 1, 0.0); int cnt = 0; std::vector<float> w (2 * N);
    for (int s = a; s + N <= b; s += H)
        for (const float* x : { L, R })
        {
            std::fill (w.begin(), w.end(), 0.0f);
            for (int k = 0; k < N; ++k) w[(size_t) k] = x[s + k] * (float) (0.5 - 0.5 * std::cos (2 * M_PI * k / N));
            fft.performFrequencyOnlyForwardTransform (w.data());
            for (int k = 0; k <= N / 2; ++k) P[(size_t) k] += (double) w[(size_t) k] * w[(size_t) k];
            ++cnt;
        }
    Reso r; if (cnt == 0) return r;
    const double df = sr / N;
    std::vector<double> c (P.size() + 1, 0.0); for (size_t k = 0; k < P.size(); ++k) c[k + 1] = c[k] + P[k];
    for (int k = (int) (80 / df); k <= (int) (10000 / df); ++k)
    {
        const double f = k * df; const int lo = std::max (1, (int) (f / std::pow (2.0, 1.0 / 6) / df)), hi = std::min ((int) P.size() - 1, (int) (f * std::pow (2.0, 1.0 / 6) / df));
        const double sm = (c[(size_t) hi + 1] - c[(size_t) lo]) / (hi - lo + 1);
        r.ex.push_back (10 * std::log10 ((P[(size_t) k] + 1e-30) / (sm + 1e-30))); r.frq.push_back (f);
    }
    auto ex = r.ex; std::sort (ex.begin(), ex.end()); r.p99 = ex[(size_t) (ex.size() * 0.99)];
    // peaks: local maxima above 6 dB, at least 1/12 octave apart
    std::vector<std::pair<double, double>> pk2;
    for (size_t i = 1; i + 1 < r.ex.size(); ++i) if (r.ex[i] > 6 && r.ex[i] >= r.ex[i - 1] && r.ex[i] >= r.ex[i + 1]) pk2.push_back ({ r.ex[i], r.frq[i] });
    std::sort (pk2.rbegin(), pk2.rend());
    for (auto& q : pk2) { bool far = true; for (auto& t : r.top) if (std::abs (std::log2 (q.second / t.second)) < 1.0 / 12) far = false; if (far) r.top.push_back (q); }
    r.n6 = (int) r.top.size(); if (! r.top.empty()) { r.mx = r.top[0].first; r.fmx = r.top[0].second; }
    return r;
}
static int resoCmd (const StringArray& files)
{
    const double t0 = std::getenv ("RESO_T0") ? atof (std::getenv ("RESO_T0")) : 0.08, len = std::getenv ("RESO_LEN") ? atof (std::getenv ("RESO_LEN")) : 1.0;
    std::cout << "preset                          |   IR p99  max@Hz      n6 |  ALGO p99  max@Hz      n6 | no post p99 n6 | top ALGO peaks (dB@Hz)" << std::endl;
    for (auto& fn : files)
    {
        auto x = XmlDocument::parse (File (fn));
        IRFitProcessor q; q.setRateAndBufferSizeDetails (48000, 256); q.prepareToPlay (48000, 256);
        if (x == nullptr || ! q.applyStateXml (*x, true) || ! q.currentModel().valid()) { std::cout << File (fn).getFileName() << ": no model" << std::endl; continue; }
        static const irfit::IRData noIr; const auto& ir = q.currentModel().ir ? *q.currentModel().ir : noIr;
        q.pushNow(); AudioBuffer<float> a; q.renderWetIR (a, 8.0);
        auto ad = q.adv(); ad.on = false; q.setAdv (ad); q.pushNow(); AudioBuffer<float> a0; q.renderWetIR (a0, 8.0);
        const auto ri = ir.L.empty() ? Reso() : resoOf (ir.L.data(), ir.R.data(), (int) ir.L.size(), t0, len);
        const auto ra = resoOf (a.getReadPointer (0), a.getReadPointer (1), a.getNumSamples(), t0, len);
        const auto r0 = resoOf (a0.getReadPointer (0), a0.getReadPointer (1), a0.getNumSamples(), t0, len);
        ad.on = true; q.setAdv (ad); q.pushNow();
        double sc = -1;
        if (! ir.L.empty()) { const auto tm = fit::measure (ir.L.data(), ir.R.data(), (int) ir.L.size()); fit::Parts pp; sc = productScore (q, tm, jlimit (0.5, 8.0, ir.L.size() / fit::kSR + 0.2), &pp); if (std::getenv ("RESO_PARTS")) std::cout << "   parts: " << irfit::partsText (pp) << std::endl; }
        String tops; tops << "score " << String (sc, 2) << " (model " << String (q.currentModel().info.score, 2) << ") | "; for (size_t k = 0; k < std::min ((size_t) 4, ra.top.size()); ++k) tops << String (ra.top[k].first, 1) << "@" << juce::roundToInt (ra.top[k].second) << " ";
        std::cout << File (fn).getFileNameWithoutExtension().substring (0, 30).paddedRight (' ', 31) << " | " << String (ri.p99, 1).paddedLeft (' ', 6) << " " << String (ri.mx, 1).paddedLeft (' ', 5) << "@" << String (juce::roundToInt (ri.fmx)).paddedRight (' ', 6) << String (ri.n6).paddedLeft (' ', 3)
                  << " | " << String (ra.p99, 1).paddedLeft (' ', 6) << " " << String (ra.mx, 1).paddedLeft (' ', 5) << "@" << String (juce::roundToInt (ra.fmx)).paddedRight (' ', 6) << String (ra.n6).paddedLeft (' ', 3)
                  << " | " << String (r0.p99, 1).paddedLeft (' ', 6) << String (r0.n6).paddedLeft (' ', 4) << "    | " << tops << std::endl;
    }
    return 0;
}
// convset <list.txt> <out.csv> [flags]: one IR path per line (tag<TAB>path)
static int convSet (const File& list, const File& out, const StringArray& flags)
{
    StringArray lines; lines.addLines (list.loadFileAsString()); lines.removeEmptyStrings();
    std::ofstream csv (out.getFullPathName().toStdString(), std::ios::app);
    if (out.getSize() == 0) csv << "tag,file,stage,score,T30,EDT,C80,env,specL,specE,iacc,ned,seconds,renders\n";
    for (auto& l : lines)
    {
        const auto tag = l.upToFirstOccurrenceOf ("\t", false, false), path = l.fromFirstOccurrenceOf ("\t", false, false);
        std::cout << "== " << tag << " " << path << std::endl;
        convertCmd (File (path), flags, &csv, tag);
    }
    return 0;
}
// polish <ir> <params.csv> <row>: the correction loop + post stage from a fitted row (no Lab fit), for quick comparisons
static int polishCmd (const File& ir, const File& csvPath, int row, const StringArray& flags, std::ostream* csv = nullptr)
{
    StringArray lines; lines.addLines (csvPath.loadFileAsString()); lines.removeEmptyStrings();
    StringArray head; head.addTokens (lines[0], ",", "\""); StringArray v; v.addTokens (lines[row + 1], ",", "\"");
    ERProcessor ref; ref.applyPreset (presetIndex ("Init — Algo"));
    for (int c = 6; c < head.size(); ++c) ref.setParam (head[c].toRawUTF8(), v[c].getFloatValue());
    fit::Values vals; for (auto* pr : ref.getParameters()) if (auto* rp = dynamic_cast<RangedAudioParameter*> (pr)) vals[rp->getParameterID().toStdString()] = rp->convertFrom0to1 (rp->getValue());
    irfit::Converter c; irfit::Converter::Options o; applyFlags (o, flags);
    const auto t0 = Time::getMillisecondCounterHiRes();
    if (auto err = c.startFromValues (ir, vals, o); err.isNotEmpty()) { std::cout << "fail " << err << std::endl; return 1; }
    while (c.running()) { Thread::sleep (20); c.tick(); }
    const auto st = c.stages();
    std::cout << row << " " << ir.getFileName() << ":";
    for (auto& s : st) std::cout << " " << s.stage << " " << String (s.score, 2);
    std::cout << "  (" << String ((Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) << " s)" << std::endl;
    if (csv)
    {
        for (auto& s : st) *csv << row << ",\"" << ir.getFileName() << "\"," << s.stage << "," << s.score << "," << s.parts.t30 * 5 << "," << s.parts.edt * 8 << "," << s.parts.c80 * 1.5 << "," << s.parts.env * 2
                                << "," << s.parts.specL << "," << s.parts.specE << "," << s.parts.iacc << "," << s.parts.ned << "," << s.seconds << "," << s.renders << "\n";
        csv->flush();
    }
    return 0;
}

// presets: save -> load -> state -> restore (bit-exact renders), the arrows' order, folders, delete
static int presetsCmd (const File& modelXml)
{
    int fails = 0; auto check = [&] (bool ok, const String& w) { std::cout << (ok ? "  ok   " : "  FAIL ") << w << std::endl; if (! ok) ++fails; };
    auto renderOf = [] (IRFitProcessor& p) { p.pushNow(); AudioBuffer<float> b; p.renderWetIR (b, 6.0); return b; };
    auto same = [] (const AudioBuffer<float>& a, const AudioBuffer<float>& b, double* maxDiff)
    {
        double d = 0; if (a.getNumSamples() != b.getNumSamples()) { *maxDiff = 1e9; return false; }
        for (int c = 0; c < 2; ++c) for (int i = 0; i < a.getNumSamples(); ++i) d = std::max (d, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
        *maxDiff = d; return d == 0;
    };
    const auto root = irfit::PresetBank::root(); root.deleteRecursively(); root.createDirectory();
    std::cout << "preset root: " << root.getFullPathName() << std::endl;
    irfit::UserModel M;
    { auto x = parseXML (modelXml); if (x == nullptr || ! (x->hasTagName ("MODEL") ? irfit::modelFromXml (*x, M) : (x->getChildByName ("MODEL") && irfit::modelFromXml (*x->getChildByName ("MODEL"), M)))) { std::cout << "no model in " << modelXml.getFullPathName() << std::endl; return 1; } }
    {   // 1. a model: activate -> its render; save as a preset; load it into a fresh instance; its state into a third one
        IRFitProcessor a; a.activateModel (M, true); setM (a, "decay", a.macros().decay * 1.3f); setM (a, "tone", 20); a.pushNow();
        AudioBuffer<float> ra = renderOf (a);
        const auto f = a.savePresetAs ("Converted", M.name);
        check (f.existsAsFile() && f.getParentDirectory().getFileName() == "Converted", "saved to Converted/" + f.getFileName());
        IRFitProcessor b; b.rescanPresets();
        irfit::PresetRef r; r.folder = "Converted"; r.name = f.getFileNameWithoutExtension(); r.file = f;
        check (b.loadPreset (r) && b.modelActive(), "loaded into a fresh instance (model mode)");
        double d = 0; AudioBuffer<float> rb = renderOf (b); { const bool ok = same (ra, rb, &d); check (ok, "preset render == original (max diff " + String (d, 12) + ")"); }
        MemoryBlock mb; b.getStateInformation (mb);
        IRFitProcessor c; c.setStateInformation (mb.getData(), (int) mb.getSize());
        AudioBuffer<float> rc = renderOf (c); { const bool ok = same (ra, rc, &d); check (ok, "state restore render == original (max diff " + String (d, 12) + ")"); }
        check (c.current.file == f, "the state remembers the preset");
        // the preset file gone: the session still has the whole model
        f.copyFileTo (f.getSiblingFile ("backup.tmp")); f.deleteFile();
        IRFitProcessor e; e.setStateInformation (mb.getData(), (int) mb.getSize()); AudioBuffer<float> re = renderOf (e);
        { const bool ok = same (ra, re, &d) && e.modelActive(); check (ok, "state restore without the preset file == original (max diff " + String (d, 12) + ")"); }
        f.getSiblingFile ("backup.tmp").moveFileTo (f);
        std::cout << "  state size " << mb.getSize() << " bytes" << std::endl;
        // at the anchor the product plays the model's own values
        IRFitProcessor g; g.activateModel (M, true); g.pushNow(); double worst = 0; String wid;
        for (auto& kv : M.eng) { if (kv.first == "mix" || kv.first == "out" || kv.first == "autoLvl") continue; auto* q = g.engine().apvts.getParameter (kv.first);
                                 const float want = q->convertFrom0to1 (q->convertTo0to1 (kv.second)), got = g.engine().value (kv.first.toRawUTF8()); if (std::abs (want - got) > worst) { worst = std::abs (want - got); wid = kv.first; } }
        check (worst == 0, "at the anchor every engine parameter = the model's (max diff " + String (worst, 9) + " " + wid + ")");
    }
    {   // 2. a library preset (knobs only + snapshot): save / load / state
        IRFitProcessor a; a.loadModel (12); setM (a, "size", 130); a.pushNow(); AudioBuffer<float> ra = renderOf (a);
        const auto f = a.savePresetAs ("My Rooms", "Lib test");
        IRFitProcessor b; irfit::PresetRef r; r.folder = "My Rooms"; r.name = "Lib test"; r.file = f;
        check (b.loadPreset (r) && ! b.modelActive(), "library preset loads in library mode");
        double d = 0; AudioBuffer<float> rb = renderOf (b); { const bool ok = same (ra, rb, &d); check (ok, "library preset render == original (max diff " + String (d, 12) + ")"); }
        MemoryBlock mb; b.getStateInformation (mb); IRFitProcessor c; c.setStateInformation (mb.getData(), (int) mb.getSize());
        AudioBuffer<float> rc = renderOf (c); { const bool ok = same (ra, rc, &d); check (ok, "library state restore == original (max diff " + String (d, 12) + ")"); }
        // model -> library -> model in one instance
        IRFitProcessor s; s.activateModel (M, true); s.pushNow(); AudioBuffer<float> m1 = renderOf (s);
        s.loadModel (12); setM (s, "size", 130); AudioBuffer<float> l1 = renderOf (s); { same (ra, l1, &d); check (d < 1e-4, "model -> factory in one instance vs a fresh factory load: max diff " + String (d, 9) + " (an ulp of parameter history; bit-exact for loads into fresh instances)"); }
        for (auto* pr : a.engine().getParameters()) if (auto* rp = dynamic_cast<RangedAudioParameter*> (pr)) { const auto id = rp->getParameterID(); const float x = a.engine().value (id.toRawUTF8()), y = s.engine().value (id.toRawUTF8()); if (x != y) std::cout << "     differs: " << id << " " << String (x, 9) << " / " << String (y, 9) << " " << irfit::floatBits (x) << " " << irfit::floatBits (y) << std::endl; }
        s.activateModel (M, true); AudioBuffer<float> m2 = renderOf (s); { same (m1, m2, &d); check (d < 1e-4, "factory -> model again in one instance: max diff " + String (d, 9)); }
    }
    {   // 3. folders and the arrows
        IRFitProcessor p; p.newFolder ("Alpha"); p.activateModel (M, true);
        p.savePresetAs ("Alpha", "A1"); p.savePresetAs ("Alpha", "A2"); p.savePresetAs ("Alpha", "A2");     // (the third gets "A2 2")
        p.rescanPresets();
        StringArray order; irfit::PresetRef r; r.factory = true; r.entry = p.bank.folders[4].items.back().entry; r.folder = p.bank.folders[4].name;     // the last factory Ambient
        p.loadPreset (r); order.add (p.presetLabel());
        for (int k = 0; k < 8; ++k) { p.stepPreset (+1); order.add (p.presetLabel()); }
        std::cout << "  arrows from the last factory preset: " << order.joinIntoString ("  >  ") << std::endl;
        check (order[1].startsWith ("Converted /") || order[1].startsWith ("Alpha /"), "next after the factory folders is the first user folder");
        StringArray back; for (int k = 0; k < 4; ++k) { p.stepPreset (-1); back.add (p.presetLabel()); }
        std::cout << "  back: " << back.joinIntoString ("  <  ") << std::endl;
        // wrap: the first factory preset, back once = the last preset of the last folder
        irfit::PresetRef f0 = p.bank.folders[0].items.front(); p.loadPreset (f0); p.stepPreset (-1);
        std::cout << "  before the first factory preset: " << p.presetLabel() << std::endl;
        check (! p.current.factory, "back from the first preset wraps to the last user folder");
        // delete
        p.bank.scan(); irfit::PresetRef del; for (auto& fo : p.bank.folders) if (fo.name == "Alpha") del = fo.items.front();
        check (p.deletePreset (del) && ! del.file.existsAsFile(), "delete " + del.name);
        int n = 0; for (auto& fo : p.bank.folders) n += (int) fo.items.size();
        std::cout << "  folders: "; for (auto& fo : p.bank.folders) std::cout << fo.name << " (" << fo.items.size() << ")  "; std::cout << std::endl;
        check (p.bank.folders.size() >= 7 && p.bank.folders[0].items.size() > 10, "factory folders + user folders listed (" + String (n) + " presets)");
    }
    std::cout << (fails ? "FAILED " : "ALL OK ") << fails << std::endl;
    return fails;
}

// rtconv <ir>: the audio thread while a conversion runs (processBlock times), at 44.1 / 48 / 96 kHz; NaN / Inf; clicks on the switch
static int rtConv (const File& ir)
{
    int fails = 0; auto check = [&] (bool ok, const String& w) { std::cout << (ok ? "  ok   " : "  FAIL ") << w << std::endl; if (! ok) ++fails; };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const int bs = 256; IRFitProcessor p; p.convOptions.drivePoll = true; p.setRateAndBufferSizeDetails (sr, bs); p.prepareToPlay (sr, bs);
        p.loadModel (40); setM (p, "mix", 50);
        AudioBuffer<float> b (2, bs); MidiBuffer midi; Random rnd (3); long t = 0; float y1 = 0, y2 = 0;
        auto blockOnce = [&] (double& ms, double& d2max, bool& fin)
        {
            for (int i = 0; i < bs; ++i) { const float x = 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 220.0 * (double) (t + i) / sr); b.setSample (0, i, x); b.setSample (1, i, x); }
            const auto a = std::chrono::steady_clock::now(); p.processBlock (b, midi); ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - a).count();
            for (int i = 0; i < bs; ++i) { const float y = b.getSample (0, i); if (! std::isfinite (y)) fin = false; d2max = std::max (d2max, (double) std::abs (y - 2 * y1 + y2)); y2 = y1; y1 = y; }
            t += bs;
        };
        // reference: 10 s of blocks without a conversion
        std::vector<double> idle, busy; double d2 = 0; bool fin = true;
        for (int k = 0; k < (int) (10 * sr / bs); ++k) { double ms; blockOnce (ms, d2, fin); idle.push_back (ms); }
        const double still = d2;
        // the conversion: blocks run as fast as they can on this thread (the message-thread work is polled every ~100 ms of audio)
        // the audio runs in real time (each block waits for its deadline, as a host's audio thread would); the message thread's
        // work (the hidden engine's steps, the hand-over) every ~100 ms
        const int f0 = p.convFinished.load(); p.convertIR (ir); const int blocksPer100ms = std::max (1, (int) (0.1 * sr / bs)); int k = 0; double d2c = 0; int switchBlock = -1;
        const auto t0 = Time::getMillisecondCounterHiRes(); auto next = std::chrono::steady_clock::now(); const auto per = std::chrono::duration<double> (bs / sr);
        while (p.convFinished.load() == f0)
        {
            double ms; blockOnce (ms, d2c, fin); busy.push_back (ms);
            if (++k % blocksPer100ms == 0) p.pollForTests();
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration> (per);
            std::this_thread::sleep_until (next);
        }
        switchBlock = k;
        double d2s = 0; for (int j = 0; j < (int) (3 * sr / bs); ++j) { double ms; blockOnce (ms, d2s, fin); }        // the switch to the model and 3 s after
        auto stats = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); double s = 0; for (auto x : v) s += x; return std::make_tuple (s / std::max<size_t> (1, v.size()), v[v.size() * 99 / 100], v.back()); };
        const auto [im, i99, imax] = stats (idle); const auto [bm, b99, bmax] = stats (busy);
        const double budget = 1000.0 * bs / sr;
        std::cout << "== " << sr << " Hz: block budget " << String (budget, 2) << " ms | idle mean " << String (im, 3) << " p99 " << String (i99, 3) << " max " << String (imax, 3)
                  << " ms | during conversion (" << String ((Time::getMillisecondCounterHiRes() - t0) / 1000.0, 0) << " s, " << busy.size() << " blocks) mean " << String (bm, 3) << " p99 " << String (b99, 3) << " max " << String (bmax, 3) << " ms" << std::endl;
        std::cout << "   " << p.lastConvReport << std::endl;
        {   // where the slow blocks are (index from the end = after the model was switched in)
            std::vector<std::pair<double, int>> top; for (int i = 0; i < (int) busy.size(); ++i) top.push_back ({ busy[(size_t) i], i });
            std::sort (top.rbegin(), top.rend()); String w ("   slowest blocks (ms @ s into the conversion): ");
            for (int j = 0; j < 6 && j < (int) top.size(); ++j) w << String (top[(size_t) j].first, 1) << " @ " << String (top[(size_t) j].second * bs / sr, 1) << "  ";
            int over = 0; for (auto x : busy) if (x > budget) ++over;
            std::cout << w << "| blocks over budget: " << over << " of " << busy.size() << std::endl;
        }
        check (fin && ! p.nonFinite.load(), "finite output throughout");
        check (p.modelActive(), "the converted model plays");
        check (d2s < 4 * still, "the switch to the model: max 2nd diff " + String (d2s, 5) + " (x" + String (d2s / still, 2) + " of still)");
        if (sr != 96000.0) continue;
    }
    std::cout << (fails ? "FAILED " : "ALL OK ") << fails << std::endl;
    return fails;
}

// modelrt <model.xml>: a model's knobs moved (clicks, CPU), switching models <-> factory presets (clicks)
static int modelRt (const File& modelXml)
{
    int fails = 0; auto check = [&] (bool ok, const String& w) { std::cout << (ok ? "  ok   " : "  FAIL ") << w << std::endl; if (! ok) ++fails; };
    irfit::UserModel M; { auto x = parseXML (modelXml); auto* e = x ? (x->hasTagName ("MODEL") ? x.get() : x->getChildByName ("MODEL")) : nullptr; if (! e || ! irfit::modelFromXml (*e, M)) { std::cout << "no model" << std::endl; return 1; } }
    const double sr = 48000; const int bs = 256, sec = (int) (sr / bs);
    IRFitProcessor p; p.setRateAndBufferSizeDetails (sr, bs); p.prepareToPlay (sr, bs);
    AudioBuffer<float> b (2, bs); MidiBuffer midi; long t = 0; float y1 = 0, y2 = 0;
    auto run = [&] (int blocks, std::function<void (int)> each) {
        double w = 0;
        for (int k = 0; k < blocks; ++k)
        {
            if (each) each (k);
            for (int i = 0; i < bs; ++i) { const float x = 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 220.0 * (double) (t + i) / sr); b.setSample (0, i, x); b.setSample (1, i, x); }
            p.processBlock (b, midi); t += bs;
            for (int i = 0; i < bs; ++i) { const float y = b.getSample (0, i); w = std::max (w, (double) std::abs (y - 2 * y1 + y2)); y2 = y1; y1 = y; }
        }
        return w; };
    p.activateModel (M, true); setM (p, "mix", 50); run (2 * sec, nullptr);
    const double still = run (3 * sec, nullptr);
    std::cout << "  still: " << still << std::endl;
    const int N = 3 * sec; const auto A = M.core.anchor;
    struct Mv { const char* n; std::function<void (int)> f; } mv[] = {
        { "decay x0.3 -> x3", [&] (int k) { setM (p, "decay", A.decay * 0.3f * std::pow (10.0f, (float) k / N)); } },
        { "tone -100 -> +100", [&] (int k) { setM (p, "tone", -100 + 200.0f * k / N); } },
        { "early -12 -> +12 dB", [&] (int k) { setM (p, "early", A.early - 12 + 24.0f * k / N); } },
        { "build-up -100 -> +100", [&] (int k) { setM (p, "buildup", -100 + 200.0f * k / N); } },
        { "size 50 -> 200", [&] (int k) { setM (p, "size", 50 + 150.0f * k / N); } },
        { "pre-delay 0 -> 150", [&] (int k) { setM (p, "predelay", 150.0f * k / N); } },
        { "advanced width / fine EQ", [&] (int k) { auto a = p.adv(); a.fineAmt = 100.0f * (float) (k % sec) / sec; a.width[2] = 50.0f + 100.0f * (float) k / N; p.setAdv (a); } },
    };
    for (auto& m : mv)
    {
        p.activateModel (M, true); setM (p, "mix", 50); run (sec, nullptr);
        const double w = run (N, m.f);
        check (w < 4 * still, String ("model mode, ") + m.n + ": max 2nd diff " + String (w, 5) + " (x" + String (w / still, 2) + ")");
    }
    {   // switches as a host sees them: the audio on its own thread in real time, the switches from this (message) thread
        auto runThreaded = [&] (const char* what, std::function<void (int)> doSwitch)
        {   // the max 2nd difference around each switch (from 2 blocks before to 120 ms after) against the steady parts between them
            p.activateModel (M, true); setM (p, "mix", 50); run (sec, nullptr);
            std::atomic<bool> stopA { false }; std::vector<double> perBlock; perBlock.reserve (4000); std::atomic<long> blk { 0 };
            std::thread au ([&] {
                auto next = std::chrono::steady_clock::now(); const auto per = std::chrono::duration<double> (bs / sr);
                while (! stopA.load()) { perBlock.push_back (run (1, nullptr)); ++blk; next += std::chrono::duration_cast<std::chrono::steady_clock::duration> (per); std::this_thread::sleep_until (next); }
            });
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            std::vector<long> at;
            for (int k = 0; k < 8; ++k) { at.push_back (blk.load()); doSwitch (k); std::this_thread::sleep_for (std::chrono::milliseconds (500)); }
            stopA = true; au.join();
            double atSw = 0, steady = 0; const long win = (long) (0.12 * sr / bs) + 2;
            for (long i = 4; i < (long) perBlock.size(); ++i)
            {
                bool near = false; for (auto s0 : at) if (i >= s0 - 2 && i <= s0 + win) near = true;
                (near ? atSw : steady) = std::max (near ? atSw : steady, perBlock[(size_t) i]);
            }
            if (std::getenv ("SWDBG")) for (size_t j = 0; j < at.size(); ++j) { long bi = -1; double bm = 0; for (long i = at[j] - 2; i <= at[j] + win && i < (long) perBlock.size(); ++i) if (i >= 0 && perBlock[(size_t) i] > bm) { bm = perBlock[(size_t) i]; bi = i - at[j]; } std::cout << "    switch " << j << ": max " << bm << " at block +" << bi << std::endl; }
            const double ref = std::max (still, steady);
            check (atSw < 4 * ref, String ("model mode, ") + what + ": around the switches max 2nd diff " + String (atSw, 5) + " (x" + String (atSw / ref, 2) + " of the steady sound "
                                   + String (ref, 5) + ")");
        };
        irfit::PresetRef fac = p.bank.folders[0].items[5];
        runThreaded ("model <-> factory preset (every 0.5 s, audio thread)", [&] (int k) { if (k % 2) p.loadPreset (fac); else p.activateModel (M, true); });
        irfit::UserModel M2 = M; M2.name = "other"; for (auto& kv : M2.eng) if (kv.first == "tDecay") kv.second *= 1.6f; M2.fillCoreFromEng();
        runThreaded ("model <-> another model (every 0.5 s, audio thread)", [&] (int k) { p.activateModel (k % 2 ? M2 : M, true); });
        runThreaded ("the arrows through factory presets (every 0.5 s, audio thread)", [&] (int) { p.stepPreset (+1); });
        runThreaded ("the arrows back and forth across folders (every 0.5 s, audio thread)", [&] (int k) { p.stepPreset (k % 3 == 2 ? -1 : +1); });
    }
    {   // CPU: the model with its post stage, knobs still / Decay moving
        p.activateModel (M, true); setM (p, "mix", 50); Random rnd (5);
        AudioBuffer<float> nb (2, bs); for (int c = 0; c < 2; ++c) for (int i = 0; i < bs; ++i) nb.setSample (c, i, rnd.nextFloat() - 0.5f);
        auto bench = [&] (bool moving) { const int blocks = (int) (10 * sr / bs); const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < blocks; ++k) { if (moving) setM (p, "decay", A.decay * (1.0f + (float) (k % 400) / 400.0f)); b.makeCopyOf (nb, true); p.processBlock (b, midi); }
            return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 10.0 * 100.0; };
        const double s1 = bench (false), s2 = bench (true);
        auto a = p.adv(); a.on = false; p.setAdv (a); const double s0 = bench (false); a.on = true; p.setAdv (a);
        std::cout << "  CPU (one core, % of real time): model still " << String (s1, 2) << " %, Decay moving " << String (s2, 2) << " %, post stage off " << String (s0, 2) << " %" << std::endl;
    }
    std::cout << (fails ? "FAILED " : "ALL OK ") << fails << std::endl;
    return fails;
}

// prep <files...>: what the converter reads (channels, length) and the measure of the target it makes
static int prepCmd (const StringArray& files)
{
    fit::Metrics ref; bool haveRef = false;
    for (auto& fn : files)
    {
        const File f (fn), out = File::getSpecialLocation (File::tempDirectory).getChildFile ("irfit_prep.wav");
        String name; int ch = 0; double len = 0;
        const auto err = irfit::prepareStereoIR (f, out, name, &ch, &len);
        if (err.isNotEmpty()) { std::cout << f.getFileName() << ": " << err << std::endl; continue; }
        std::vector<float> L, R; fit::loadTarget (out, {}, L, R); const auto m = fit::measure (L.data(), R.data(), (int) L.size());
        double sc = 0; if (haveRef) sc = fit::compare (ref, m); else { ref = m; haveRef = true; }
        std::cout << f.getFileName() << ": " << ch << " ch, " << String (len, 2) << " s -> \"" << name << "\" T30 500/1k/2k " << String (m.t30[2], 3) << "/" << String (m.t30[3], 3) << "/" << String (m.t30[4], 3)
                  << " C80 " << String (m.c80, 2) << " IACC1k " << String (m.iacc[2], 3) << (sc > 0 || &fn != &files[0] ? " | score vs the first " + String (sc, 3) : String()) << std::endl;
    }
    return 0;
}

// shot <out.png> [model <xml>] [adv] [conv <ir>]: the editor as an image (the message loop runs: timers, the hidden engine)
static int shotCmd (const StringArray& a)
{
    IRFitProcessor p; p.setRateAndBufferSizeDetails (48000, 256); p.prepareToPlay (48000, 256);
    if (const int i = a.indexOf ("model"); i >= 0)
    {
        irfit::UserModel M; auto x = parseXML (File (a[i + 1])); auto* e = x ? (x->hasTagName ("MODEL") ? x.get() : x->getChildByName ("MODEL")) : nullptr;
        if (e && irfit::modelFromXml (*e, M))
        {
            if (const int j = a.indexOf ("ir"); j >= 0) { auto d = std::make_shared<irfit::IRData>(); if (irfit::loadIRFile (File (a[j + 1]), *d, ERProcessor::kRefWetDb).isEmpty()) M.ir = d; }
            p.activateModel (M, true); p.savePresetAs ("Converted", M.name, true);
            if (a.contains ("hyb")) { setM (p, "erBlend", 100); setM (p, "tailBlend", 50); }
        }
    }
    else p.loadPreset (p.bank.folders[0].items[3]);
    std::unique_ptr<AudioProcessorEditor> ed (static_cast<AudioProcessor&> (p).createEditorIfNeeded());
    ed->setVisible (true);
    if (a.contains ("adv")) { std::function<void (Component*)> walk = [&] (Component* c) { if (auto* b = dynamic_cast<Button*> (c)) if (b->getButtonText() == "Advanced") b->triggerClick(); for (auto* k : c->getChildren()) walk (k); }; walk (ed.get()); }
    if (const int i = a.indexOf ("conv"); i >= 0) { p.convertIR (File (a[i + 1])); MessageManager::getInstance()->runDispatchLoopUntil (6000); }
    else MessageManager::getInstance()->runDispatchLoopUntil (1500);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds());
    File (a[0]).deleteFile(); FileOutputStream os { File (a[0]) }; PNGImageFormat png; png.writeImageToStream (img, os);
    std::cout << "wrote " << a[0] << " " << img.getWidth() << "x" << img.getHeight() << std::endl;
    p.cancelConversion(); for (int k = 0; k < 50 && p.converter() && p.converter()->running(); ++k) MessageManager::getInstance()->runDispatchLoopUntil (100);
    ed.reset();
    return 0;
}

// cancel <ir>: Cancel in the Lab stage and in the correction stage, and the plug-in closed while converting (how long each takes)
static int cancelCmd (const File& ir)
{
    int fails = 0; auto check = [&] (bool ok, const String& w) { std::cout << (ok ? "  ok   " : "  FAIL ") << w << std::endl; if (! ok) ++fails; };
    for (double at : { 5.0, -1.0 })
    {
        IRFitProcessor p; p.convOptions.drivePoll = true; p.convertIR (ir);
        const auto t0 = Time::getMillisecondCounterHiRes();
        while (p.converter()->running())
        {
            Thread::sleep (25); p.pollForTests();
            const double el = (Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            if ((at > 0 && el > at) || (at < 0 && p.converter()->state.load() == irfit::Converter::polishing)) break;
        }
        const int st = p.converter()->state.load();
        const auto t1 = Time::getMillisecondCounterHiRes(); p.cancelConversion();
        while (p.converter()->running() && Time::getMillisecondCounterHiRes() - t1 < 30000) { Thread::sleep (10); p.pollForTests(); }
        const double took = (Time::getMillisecondCounterHiRes() - t1) / 1000.0;
        check (p.converter()->state.load() == irfit::Converter::cancelled && ! p.modelActive(), String ("cancel during ") + (st == irfit::Converter::labFit ? "the Lab fit" : "the correction loop") + ": cancelled in " + String (took, 2) + " s, the sound unchanged (library mode)");
    }
    for (double at : { 5.0, -1.0 })
    {
        auto p = std::make_unique<IRFitProcessor>(); p->convOptions.drivePoll = true; p->convertIR (ir);
        const auto t0 = Time::getMillisecondCounterHiRes();
        while (p->converter()->running())
        {
            Thread::sleep (25); p->pollForTests();
            if ((at > 0 && (Time::getMillisecondCounterHiRes() - t0) / 1000.0 > at) || (at < 0 && p->converter()->state.load() == irfit::Converter::polishing)) break;
        }
        const auto t1 = Time::getMillisecondCounterHiRes(); p.reset();
        const double took = (Time::getMillisecondCounterHiRes() - t1) / 1000.0;
        check (took < 5.0, String ("the plug-in closed during ") + (at > 0 ? "the Lab fit" : "the correction loop") + ": destroyed in " + String (took, 2) + " s");
    }
    std::cout << (fails ? "FAILED " : "ALL OK ") << fails << std::endl;
    return fails;
}

static int mapOnly()
{
    irfit::Macros m; irfit::Result r; irfit::map (m, r);
    for (int j = 0; j < r.used; ++j) std::cout << irfit::kEntries[r.nn[j]].name << " " << r.w[j] << "\n";
    for (int i = 0; i < r.n; ++i) std::cout << r.v[i].first << "=" << r.v[i].second << " ";
    std::cout << "\n";
    return 0;
}


// edtprobe <model.xml>: the model's EDT per octave as it is and with its initial-decay parameters moved (no post stage)
static int edtProbe (const File& f)
{
    auto x = parseXML (f); irfit::UserModel M;
    if (x == nullptr || ! (x->hasTagName ("MODEL") ? irfit::modelFromXml (*x, M) : (x->getChildByName ("MODEL") && irfit::modelFromXml (*x->getChildByName ("MODEL"), M)))) { std::cout << "no model" << std::endl; return 1; }
    ERProcessor e; e.setRateAndBufferSizeDetails (48000, 512); e.prepareToPlay (48000, 512);
    fit::Values base; for (auto& kv : M.eng) base[kv.first.toStdString()] = kv.second;
    auto run = [&] (const char* what, std::function<void (fit::Values&)> f2)
    {
        fit::Values v = base; f2 (v); ERProcessor::tlOverride = &v;
        const auto m = renderMeasure (e); ERProcessor::tlOverride = nullptr;
        std::cout << String (what).paddedRight (' ', 28) << " EDT"; for (auto q : m.edt) std::cout << " " << String (q, 2); std::cout << " | T30"; for (auto q : m.t30) std::cout << " " << String (q, 2); std::cout << " | c80 " << String (m.c80, 2) << std::endl;
    };
    run ("as is", [] (fit::Values&) {});
    run ("tail only", [] (fit::Values& v) { v["erOn"] = 0; });
    run ("ER only", [] (fit::Values& v) { v["tOn"] = 0; });
    run ("tEd all 40, tEdr 100", [] (fit::Values& v) { for (int j = 1; j <= 5; ++j) v["tEd" + std::to_string (j)] = 40; v["tEdr"] = 100; });
    run ("tEd all 100 (no EDT path)", [] (fit::Values& v) { for (int j = 1; j <= 5; ++j) v["tEd" + std::to_string (j)] = 100; v["tEdr"] = 100; });
    run ("tail only, tEd 40", [] (fit::Values& v) { v["erOn"] = 0; for (int j = 1; j <= 5; ++j) v["tEd" + std::to_string (j)] = 40; v["tEdr"] = 100; });
    run ("ER +6 dB", [] (fit::Values& v) { v["level"] += 6; });
    run ("tSpread 0 tShape 0", [] (fit::Values& v) { v["tSpread"] = 0; v["tShape"] = 0; });
    return 0;
}

// tailcache <model.xml>: does the tail-only render move with the ER parameters? and what an ER-only render costs
static int tailCache (const File& f)
{
    auto x = parseXML (f); irfit::UserModel M;
    if (x == nullptr || ! (x->hasTagName ("MODEL") ? irfit::modelFromXml (*x, M) : (x->getChildByName ("MODEL") && irfit::modelFromXml (*x->getChildByName ("MODEL"), M)))) { std::cout << "no model" << std::endl; return 1; }
    ERProcessor e; e.setRateAndBufferSizeDetails (48000, 512); e.prepareToPlay (48000, 512);
    fit::Values base; for (auto& kv : M.eng) base[kv.first.toStdString()] = kv.second;
    auto render = [&] (fit::Values v, AudioBuffer<float>& b, double& ms)
    {
        ERProcessor::tlOverride = &v; ERProcessor::tlMaxLenS = 8;
        ERProcessor::ExportOpts o; o.sr = 48000; o.bits = 32; o.tail = true; o.normalize = false; double g = 0;
        const double t0 = Time::getMillisecondCounterHiRes(); e.renderIR (o, b, g); ms = Time::getMillisecondCounterHiRes() - t0;
        ERProcessor::tlOverride = nullptr; ERProcessor::tlMaxLenS = 0;
    };
    auto diff = [] (const AudioBuffer<float>& a, const AudioBuffer<float>& b)
    {
        double pk = 1e-20, d = 0; const int n = std::min (a.getNumSamples(), b.getNumSamples());
        for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) { pk = std::max (pk, (double) std::abs (a.getSample (c, i))); d = std::max (d, (double) std::abs (a.getSample (c, i) - b.getSample (c, i))); }
        return String (d / pk, 8) + (a.getNumSamples() != b.getNumSamples() ? " (len " + String (a.getNumSamples()) + " vs " + String (b.getNumSamples()) + ")" : String());
    };
    AudioBuffer<float> full, tail, er; double tf, tt, te;
    auto v = base; render (v, full, tf);
    v = base; v["erOn"] = 0; render (v, tail, tt);
    v = base; v["tOn"] = 0; render (v, er, te);
    std::cout << "ms: full " << tf << " tail " << tt << " ER " << te << std::endl;
    AudioBuffer<float> sum (2, full.getNumSamples()); sum.clear();
    for (int c = 0; c < 2; ++c) { for (int i = 0; i < std::min (tail.getNumSamples(), sum.getNumSamples()); ++i) sum.addSample (c, i, tail.getSample (c, i)); for (int i = 0; i < std::min (er.getNumSamples(), sum.getNumSamples()); ++i) sum.addSample (c, i, er.getSample (c, i)); }
    std::cout << "ER + tail vs full: " << diff (full, sum) << std::endl;
    { AudioBuffer<float> fp; double ms; e.fitParallel = true; render (base, fp, ms); e.fitParallel = false; std::cout << "threaded render (" << ms << " ms) vs serial: " << diff (full, fp) << std::endl; }
    for (auto [id, dv] : std::vector<std::pair<const char*, float>> { { "first", 7 }, { "length", 30 }, { "growth", 0.3f }, { "energy", 3 }, { "decay", 4 }, { "width", 20 }, { "outW", 20 }, { "eqG3", 3 }, { "tModD", 10 } })
    {
        AudioBuffer<float> t2; double ms; auto w = base; w["erOn"] = 0; w[id] += dv; render (w, t2, ms);
        std::cout << "tail-only with " << id << " moved: " << diff (tail, t2) << std::endl;
    }
    return 0;
}


// hybrid <model.xml> <ir.wav>: the IR next to its model (0.3.0) - identity at 0 %, the IR itself at 100 %, the edits, clicks, CPU, state
static int hybridCmd (const File& modelXml, const File& irFile)
{
    int fails = 0; auto check = [&] (bool ok, const String& w) { std::cout << (ok ? "  ok   " : "  FAIL ") << w << std::endl; if (! ok) ++fails; };
    irfit::UserModel M; { auto x = parseXML (modelXml); auto* e = x ? (x->hasTagName ("MODEL") ? x.get() : x->getChildByName ("MODEL")) : nullptr; if (! e || ! irfit::modelFromXml (*e, M)) { std::cout << "no model" << std::endl; return 1; } }
    irfit::UserModel M0 = M; M0.ir = nullptr;
    if (! M.ir) { auto d = std::make_shared<irfit::IRData>(); auto err = irfit::loadIRFile (irFile, *d, ERProcessor::kRefWetDb); if (err.isNotEmpty()) { std::cout << err << std::endl; return 1; } M.ir = d; }
    std::cout << "IR " << M.ir->name << ": " << M.ir->L.size() / 48000.0 << " s, onset " << M.ir->onsetMs << " ms (model " << M.onset << ", anchor pre-delay " << M.core.anchor.preDelay
              << "), T60 low / mid / high " << M.ir->t60[0] << " / " << M.ir->t60[1] << " / " << M.ir->t60[2] << " s" << std::endl;
    const double sr = 48000; const int bs = 256;
    MidiBuffer midi;
    auto settle = [&] (IRFitProcessor& p) {
        AudioBuffer<float> z (2, bs);
        for (int k = 0; k < 400; ++k)
        {
            p.pollHybridForTests(); z.clear(); p.processBlock (z, midi);
            if (p.hybridIdle() && k > 20) break;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (400));         // (JUCE's convolution prepares the IR on its own thread)
        const double st = std::getenv ("HYB_SETTLE") ? String (std::getenv ("HYB_SETTLE")).getDoubleValue() : 1.5;
        for (int k = 0; k < (int) (st * sr / bs); ++k) { z.clear(); p.processBlock (z, midi); } };
    auto impulse = [&] (IRFitProcessor& p, double secs, AudioBuffer<float>& out) {
        settle (p);
        const int n = (int) (secs * sr); out.setSize (2, n); out.clear(); AudioBuffer<float> b (2, bs);
        for (int i0 = 0; i0 < n; i0 += bs) { b.clear(); if (i0 == 0) { b.setSample (0, 0, 1); b.setSample (1, 0, 1); } p.processBlock (b, midi); for (int c = 0; c < 2; ++c) out.copyFrom (c, i0, b, c, 0, std::min (bs, n - i0)); } };
    auto mk = [&] (bool withIR) { auto p = std::make_unique<IRFitProcessor>(); p->setRateAndBufferSizeDetails (sr, bs); p->prepareToPlay (sr, bs); p->activateModel (withIR ? M : M0, true); setM (*p, "mix", 100); return p; };
    auto meas = [] (const AudioBuffer<float>& b) { return fit::measure (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples()); };
    auto eDb = [] (const AudioBuffer<float>& b) { double e = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) e += 0.5 * (double) b.getSample (c, i) * b.getSample (c, i); return 10 * std::log10 (e + 1e-30); };
    auto eSeg = [] (const AudioBuffer<float>& b, double t0, double t1) { double e = 0; for (int c = 0; c < 2; ++c) for (int i = std::max (0, (int) (t0 * 48000)); i < std::min (b.getNumSamples(), (int) (t1 * 48000)); ++i) e += (double) b.getSample (c, i) * b.getSample (c, i); return 10 * std::log10 (e + 1e-30); };
    auto onset = [] (const AudioBuffer<float>& b) { std::vector<float> L (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples()), R (b.getReadPointer (1), b.getReadPointer (1) + b.getNumSamples()); return irfit::hyb::onsetIdx (L, R) / 48.0; };
    // 1. at 0 % the 0.2.x plug-in, sample for sample
    AudioBuffer<float> a0, a1;
    { auto p = mk (false); impulse (*p, 4, a0);
      AudioBuffer<float> w; p->renderWetIR (w, 8.0);
      if (std::getenv ("HYB_LVL"))
      {
          AudioBuffer<float> r8; impulse (*p, 8, r8);
          for (int k = 0; k < 12; ++k) { auto seg = [&] (const AudioBuffer<float>& b) { double e = 0; for (int c = 0; c < 2; ++c) for (int i = k * 2400; i < (k + 1) * 2400 && i < b.getNumSamples(); ++i) e += (double) b.getSample (c, i) * b.getSample (c, i); return 10 * std::log10 (e + 1e-30); };
                                         std::cout << "   " << k * 50 << " ms: rt " << String (seg (r8), 2) << "  offline " << String (seg (w), 2) << std::endl; }
          fit::Parts pp; const double sc = fit::compare (meas (w), meas (r8), &pp); std::cout << "   rt vs offline score " << sc << " " << irfit::partsText (pp) << "  rt8 energy " << eDb (r8) << std::endl;
          AudioBuffer<float> rl (2, 8 * 48000); rl.clear(); { AudioBuffer<float> b (2, bs); settle (*p); for (int i0 = 0; i0 < rl.getNumSamples(); i0 += bs) { b.clear(); if (i0 == 0) b.setSample (0, 0, 1); p->processBlock (b, midi); for (int c = 0; c < 2; ++c) rl.copyFrom (c, i0, b, c, 0, bs); } }
          {   std::vector<float> L, R; fit::loadTarget (irFile, {}, L, R); const auto tm = fit::measure (L.data(), R.data(), (int) L.size());
              fit::Parts q1, q2; const double s1 = fit::compare (tm, meas (r8), &q1), s2 = fit::compare (tm, meas (w), &q2);
              std::cout << "   vs the IR: real time " << s1 << " (" << irfit::partsText (q1) << ")  offline " << s2 << " (" << irfit::partsText (q2) << ")" << std::endl; }
          std::cout << "   L-only impulse energy " << eDb (rl) << std::endl;
          {   // the engine alone, in real time
              auto& e = p->engine(); AudioBuffer<float> b (2, bs), eo (2, 8 * 48000); eo.clear();
              for (int k = 0; k < 400; ++k) { b.clear(); e.processBlock (b, midi); }
              for (int i0 = 0; i0 < eo.getNumSamples(); i0 += bs) { b.clear(); if (i0 == 0) { b.setSample (0, 0, 1); b.setSample (1, 0, 1); } e.processBlock (b, midi); for (int c = 0; c < 2; ++c) eo.copyFrom (c, i0, b, c, 0, bs); }
              std::cout << "   engine alone rt energy " << eDb (eo) << "  mix " << e.value ("mix") << " dk " << e.value ("dkMode") << " gate " << e.value ("gtOn") << std::endl;
              for (int k = 0; k < 6; ++k) { double en = 0; for (int c = 0; c < 2; ++c) for (int i = k * 2400; i < (k + 1) * 2400; ++i) en += (double) eo.getSample (c, i) * eo.getSample (c, i); std::cout << "    " << k * 50 << " ms " << 10 * std::log10 (en + 1e-30) << std::endl; }
          }
      }
      std::cout << "  levels: real time " << String (eDb (a0), 2) << " dB, renderWetIR " << String (eDb (w), 2) << " dB, model eEr / eTail " << String (M.core.eEr, 2) << " / " << String (M.core.eTail, 2) << " dB" << std::endl; }
    { auto p = mk (true); impulse (*p, 4, a1); }
    { double d = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < a0.getNumSamples(); ++i) d = std::max (d, (double) std::abs (a0.getSample (c, i) - a1.getSample (c, i)));
      check (d == 0, "ER / tail IR mix 0 %: the same output as without the IR (max diff " + String (d) + ")"); }
    // 2. at 100 % / 100 %: the IR itself (with the ALGO at -inf)
    const double len = std::min (17.0, M.ir->L.size() / sr * 2.0 + 1.0);
    AudioBuffer<float> ir100; auto P = mk (true); setM (*P, "erBlend", 100); setM (*P, "tailBlend", 100); impulse (*P, len, ir100);
    {
        AudioBuffer<float> ref (2, ir100.getNumSamples()); ref.clear();
        for (int i = 0; i < ref.getNumSamples() && i < (int) M.ir->L.size(); ++i) { ref.setSample (0, i, M.ir->L[(size_t) i] * ERProcessor::kWetCal); ref.setSample (1, i, M.ir->R[(size_t) i] * ERProcessor::kWetCal); }
        double num = 0, den = 0, xx = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < ref.getNumSamples(); ++i) { const double x = ref.getSample (c, i), y = ir100.getSample (c, i); num += x * y; den += y * y; xx += x * x; }
        const double corr = num / std::sqrt (den * xx + 1e-30), dDb = eDb (ir100) - eDb (ref);
        check (corr > 0.99 && std::abs (dDb) < 0.5, "100 % / 100 %: the IR itself: correlation " + String (corr, 5) + ", level " + String (dDb, 2) + " dB");
        const auto mi = meas (ref), mp = meas (ir100);
        std::cout << "         T30 mid IR " << t30m (mi) << " / plug-in " << t30m (mp) << "   energy ALGO " << String (eDb (a0), 2) << " dB, IR " << String (eDb (ir100), 2) << " dB" << std::endl;
    }
    if (std::getenv ("HYB_QUICK")) return fails;
    // 3. the edits and the linked knobs (100 % / 100 %)
    const auto base = meas (ir100); const double bT = t30m (base), bOn = onset (ir100);
    // the time the broadband decay curve takes to fall 20 dB (from the onset) / EDT at 1 kHz, relative to the plain IR's
    auto t20 = [] (const AudioBuffer<float>& b) { const int n = b.getNumSamples(); std::vector<double> e ((size_t) n); for (int i = 0; i < n; ++i) e[(size_t) i] = (double) b.getSample (0, i) * b.getSample (0, i) + (double) b.getSample (1, i) * b.getSample (1, i);
        const auto d = fit::edcDb (e, 48000); for (size_t i = 0; i < d.size(); ++i) if (d[i] <= -20) return i / 48000.0; return (double) n / 48000; };
    const double bT20 = t20 (ir100);
    auto kOk = [&] (const AudioBuffer<float>& b, double k, String& w) { const double e = meas (b).edt[3] / base.edt[3], d = t20 (b) / bT20;
        w = "EDT 1k x" + String (e, 2) + ", -20 dB time x" + String (d, 2) + " (want x" + String (k, 2) + ")"; return std::abs (e / k - 1) < 0.12 && std::abs (d / k - 1) < 0.15; };
    struct Ed { const char* n; std::function<void (IRFitProcessor&)> set; std::function<bool (const AudioBuffer<float>&, String&)> ok; };
    const auto A = M.core.anchor;
    std::vector<Ed> eds = {
        { "IR Decay Mid 200 %", [] (IRFitProcessor& p) { setM (p, "irDecM", 200); }, [&] (const AudioBuffer<float>& b, String& w) { kOk (b, 2.0, w); return std::abs (meas (b).edt[3] / base.edt[3] / 2 - 1) < 0.12; } },
        { "all IR Decay 200 %", [] (IRFitProcessor& p) { setM (p, "irDecL", 200); setM (p, "irDecM", 200); setM (p, "irDecH", 200); }, [&] (const AudioBuffer<float>& b, String& w) { return kOk (b, 2.0, w); } },
        { "all IR Decay 50 %", [] (IRFitProcessor& p) { setM (p, "irDecL", 50); setM (p, "irDecM", 50); setM (p, "irDecH", 50); }, [&] (const AudioBuffer<float>& b, String& w) { return kOk (b, 0.5, w); } },
        { "IR Decay Mid 50 %", [] (IRFitProcessor& p) { setM (p, "irDecM", 50); }, [&] (const AudioBuffer<float>& b, String& w) { kOk (b, 0.5, w); return std::abs (meas (b).edt[3] / base.edt[3] / 0.5 - 1) < 0.12; } },
        { "Decay knob x0.5 (linked)", [&] (IRFitProcessor& p) { setM (p, "decay", A.decay * 0.5f); }, [&] (const AudioBuffer<float>& b, String& w) { return kOk (b, 0.5, w); } },
        { "Decay knob x0.5, link off", [&] (IRFitProcessor& p) { setM (p, "decay", A.decay * 0.5f); setM (p, "irLink", 0); }, [&] (const AudioBuffer<float>& b, String& w) { const double r = t30m (meas (b)) / bT; w = "T30 mid x" + String (r, 2) + ", EDT 1k x" + String (meas (b).edt[3] / base.edt[3], 2); return std::abs (r - 1) < 0.05; } },
        { "Tone +100 (linked)", [] (IRFitProcessor& p) { setM (p, "tone", 100); }, [&] (const AudioBuffer<float>& b, String& w) { const auto m = meas (b); const double d = tiltOf (m) - tiltOf (base); w = "tilt " + String (d, 3); return d > 0.1; } },
        { "IR Size 150 %", [] (IRFitProcessor& p) { setM (p, "irSize", 150); }, [&] (const AudioBuffer<float>& b, String& w) { return kOk (b, 1.5, w); } },
        { "Size knob 150 (linked: time, not decay)", [&] (IRFitProcessor& p) { setM (p, "size", A.size * 1.5f); }, [&] (const AudioBuffer<float>& b, String& w) { const double r = t30m (meas (b)) / bT; w = "T30 mid x" + String (r, 2) + ", EDT 1k x" + String (meas (b).edt[3] / base.edt[3], 2); return std::abs (r - 1) < 0.1; } },
        { "Pre-Delay +50 ms (linked)", [&] (IRFitProcessor& p) { setM (p, "predelay", A.preDelay + 50); }, [&] (const AudioBuffer<float>& b, String& w) { const double d = onset (b) - bOn; w = "onset " + String (d, 1) + " ms"; return std::abs (d - 50) < 2; } },
        { "Pre-Delay to 0 (linked)", [&] (IRFitProcessor& p) { setM (p, "predelay", 0); }, [&] (const AudioBuffer<float>& b, String& w) { const double want = -std::min ((double) A.preDelay, bOn), d = onset (b) - bOn; w = "onset " + String (d, 1) + " ms (want " + String (want, 1) + ")"; return std::abs (d - want) < 1.5; } },
        { "IR Start 30 ms", [] (IRFitProcessor& p) { setM (p, "irStart", 30); }, [&] (const AudioBuffer<float>& b, String& w) { const double d = eDb (b) - eDb (ir100); w = "level " + String (d, 2) + " dB"; return d < -0.2; } },
        { "IR Attack 100 ms", [] (IRFitProcessor& p) { setM (p, "irAttack", 100); }, [&] (const AudioBuffer<float>& b, String& w) { const double d = eSeg (b, 0, 0.05) - eSeg (ir100, 0, 0.05); w = "first 50 ms " + String (d, 1) + " dB"; return d < -6; } },
        { "IR Length 30 %", [] (IRFitProcessor& p) { setM (p, "irLength", 30); }, [&] (const AudioBuffer<float>& b, String& w) { const double L0 = M.ir->L.size() / sr, d = eSeg (b, 0.32 * L0, L0) - eSeg (ir100, 0.32 * L0, L0); w = "after 32 % of the IR " + String (d, 1) + " dB"; return d < -40; } },
        { "IR Reverse", [] (IRFitProcessor& p) { setM (p, "irReverse", 1); }, [&] (const AudioBuffer<float>& b, String& w) { const double L0 = M.ir->L.size() / sr; const double d = eSeg (b, L0 - 1.2, L0 - 0.2) - eSeg (b, 0.2, 1.2); w = "the tail's last second vs its first " + String (d, 1) + " dB"; return d > 10; } },
        { "IR Width 0 % (mono)", [] (IRFitProcessor& p) { setM (p, "irWidth", 0); }, [&] (const AudioBuffer<float>& b, String& w) { double d = 0; for (int i = 0; i < b.getNumSamples(); ++i) d = std::max (d, (double) std::abs (b.getSample (0, i) - b.getSample (1, i))); w = "max |L - R| " + String (d); return d < 1e-6; } },
        { "Tail IR level -6 dB", [] (IRFitProcessor& p) { setM (p, "irTailLvl", -6); }, [&] (const AudioBuffer<float>& b, String& w) { const double d = eSeg (b, 0.2, 3) - eSeg (ir100, 0.2, 3); w = "after 200 ms " + String (d, 2) + " dB"; return std::abs (d + 6) < 0.3; } },
        { "Low Cut 400 Hz (linked)", [] (IRFitProcessor& p) { setM (p, "lowcut", 400); }, [&] (const AudioBuffer<float>& b, String& w) { const double d = eDb (b) - eDb (ir100); w = "level " + String (d, 2) + " dB"; return d < -0.2; } },
        { "ER = IR, tail = ALGO (50 ms split)", [] (IRFitProcessor& p) { setM (p, "tailBlend", 0); setM (p, "hSplit", 50); }, [&] (const AudioBuffer<float>& b, String& w) { const double d = eDb (b) - eDb (ir100); const auto m = meas (b); w = "level " + String (d, 2) + " dB, T30 mid " + String (t30m (m), 2) + " s"; return std::abs (d) < 3; } },
    };
    for (auto& e : eds)
    {
        auto p = mk (true); setM (*p, "erBlend", 100); setM (*p, "tailBlend", 100); e.set (*p);
        AudioBuffer<float> o; impulse (*p, len, o); String w; const bool ok = e.ok (o, w);
        check (ok, String (e.n) + ": " + w);
    }
    if (std::getenv ("HYB_EDITS")) return fails;
    // 4. clicks: a 220 Hz sine through, the blends and the knobs moving (real-time blocks, the IRs rendered on the worker as they come)
    {
        auto p = mk (true); setM (*p, "mix", 50); settle (*p);
        AudioBuffer<float> b (2, bs); long t = 0; float y1 = 0, y2 = 0; const int sec = (int) (sr / bs);
        auto run = [&] (int blocks, std::function<void (int)> each) { double w = 0;
            for (int k = 0; k < blocks; ++k)
            {
                if (each) each (k);
                if (k % 19 == 0) p->pollHybridForTests();
                for (int i = 0; i < bs; ++i) { const float x = 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 220.0 * (double) (t + i) / sr); b.setSample (0, i, x); b.setSample (1, i, x); }
                p->processBlock (b, midi); t += bs;
                for (int i = 0; i < bs; ++i) { const float y = b.getSample (0, i); const double v = std::abs (y - 2 * y1 + y2); if (v > w) { w = v; if (std::getenv ("HYB_WHERE")) std::cout << "      max at block " << k << " sample " << i << ": " << v << std::endl; } y2 = y1; y1 = y; }
                std::this_thread::sleep_for (std::chrono::microseconds (200));
            }
            return w; };
        run (2 * sec, nullptr); const double still = run (2 * sec, nullptr);
        std::cout << "  still: " << still << std::endl;
        const int N = 3 * sec;
        struct Mv { const char* n; std::function<void (int)> f; } mv[] = {
            { "ER IR mix 0 -> 100 %", [&] (int k) { setM (*p, "erBlend", 100.0f * k / N); } },
            { "tail IR mix 0 -> 100 %", [&] (int k) { setM (*p, "tailBlend", 100.0f * k / N); } },
            { "Decay x0.5 -> x2 (IR re-rendered)", [&] (int k) { setM (*p, "decay", A.decay * 0.5f * std::pow (4.0f, (float) k / N)); } },
            { "Pre-Delay 0 -> 150", [&] (int k) { setM (*p, "predelay", 150.0f * k / N); } },
            { "IR Size 50 -> 200 %", [&] (int k) { setM (*p, "irSize", 50 + 150.0f * k / N); } },
            { "IR Width 0 -> 200 %", [&] (int k) { setM (*p, "irWidth", 200.0f * k / N); } },
            { "Low Cut 20 -> 800", [&] (int k) { setM (*p, "lowcut", 20 + 780.0f * k / N); } },
        };
        bool first = true;
        for (auto& m : mv)
        {
            if (! first) { const float b0 = std::getenv ("HYB_ALGO") ? 0.0f : 100.0f; setM (*p, "erBlend", b0); setM (*p, "tailBlend", b0); }
            // (each move from the model's own settings)
            setM (*p, "decay", A.decay); setM (*p, "predelay", A.preDelay); setM (*p, "irSize", 100); setM (*p, "irWidth", 100); setM (*p, "lowcut", 20);
            if (! first) run (2 * sec, nullptr);
            const double w = run (N, m.f); const double after = run (sec, nullptr), ref = std::max (still, after);
            check (w < 4 * ref, String ("hybrid, ") + m.n + ": max 2nd diff " + String (w, 5) + " (x" + String (w / ref, 2) + " of the steady sound before / after)");
            if (std::string (m.n).rfind ("tail IR", 0) == 0) first = false;
        }
    }
    // 5. CPU (one core): ALGO only / ER IR / both IR / IR only
    {
        Random rnd (5); AudioBuffer<float> nb (2, bs), b (2, bs); for (int c = 0; c < 2; ++c) for (int i = 0; i < bs; ++i) nb.setSample (c, i, rnd.nextFloat() - 0.5f);
        auto bench = [&] (float e, float tl) { auto p = mk (true); setM (*p, "erBlend", e); setM (*p, "tailBlend", tl); settle (*p);
            const int blocks = (int) (10 * sr / bs); const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < blocks; ++k) { b.makeCopyOf (nb, true); p->processBlock (b, midi); }
            return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 10.0 * 100.0; };
        std::cout << "  CPU (% of real time, 256-sample blocks): ALGO " << String (bench (0, 0), 2) << " %, ER IR " << String (bench (100, 0), 2) << " %, ER + tail 50 % " << String (bench (50, 50), 2)
                  << " %, IR only " << String (bench (100, 100), 2) << " %" << std::endl;
    }
    // 6. a session with an IR in the tail slot (another IR) comes back the same
    {
        auto p = mk (true); setM (*p, "erBlend", 100); setM (*p, "tailBlend", 100); setM (*p, "irDecH", 60);      // (the ALGO's modulation runs on: compare the IR parts)
        const auto err = p->loadSlotIR (false, irFile); check (err.isEmpty(), "tail slot: " + (err.isEmpty() ? p->slotName (false) : err));
        AudioBuffer<float> o1, o2; impulse (*p, 3, o1);
        MemoryBlock st; p->getStateInformation (st);
        auto q = std::make_unique<IRFitProcessor>(); q->setRateAndBufferSizeDetails (sr, bs); q->prepareToPlay (sr, bs); q->setStateInformation (st.getData(), (int) st.getSize());
        impulse (*q, 3, o2);
        double d = 0, pk = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < o1.getNumSamples(); ++i) { d = std::max (d, (double) std::abs (o1.getSample (c, i) - o2.getSample (c, i))); pk = std::max (pk, (double) std::abs (o1.getSample (c, i))); }
        if (std::getenv ("HYB_RT"))
            for (auto [e, tl] : { std::pair<float, float> { 0, 0 }, { 100, 0 }, { 0, 100 } })
            {
                setM (*p, "erBlend", e); setM (*p, "tailBlend", tl); setM (*q, "erBlend", e); setM (*q, "tailBlend", tl);
                AudioBuffer<float> x1, x2; impulse (*p, 3, x1); impulse (*q, 3, x2); double dd = 0, pp = 0; int at = 0;
                for (int c = 0; c < 2; ++c) for (int i = 0; i < x1.getNumSamples(); ++i) { const double v = std::abs (x1.getSample (c, i) - x2.getSample (c, i)); if (v > dd) { dd = v; at = i; } pp = std::max (pp, (double) std::abs (x1.getSample (c, i))); }
                std::cout << "    blends " << e << " / " << tl << ": max diff " << dd / pp * 100 << " % at " << at / 48.0 << " ms   slots " << p->slotName (true) << " | " << p->slotName (false) << " vs " << q->slotName (true) << " | " << q->slotName (false) << std::endl;
            }
        check (d < 1e-3 * pk, "state round trip (model IR + a slot IR, " + String (st.getSize() / 1024) + " kB): max diff " + String (d / pk * 100, 4) + " % of the peak");
    }
    std::cout << (fails ? "FAILED " : "ALL OK ") << fails << std::endl;
    return fails;
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    const String mode = argc > 1 ? argv[1] : "map";
    if (mode == "repro") return repro (argc > 2 ? argv[2] : "params.csv");
    if (mode == "sweep") return sweep (argc > 2 && String (argv[2]) == "refine");
    if (mode == "rt") return rt();
    if (mode == "edtprobe") return edtProbe (File (argv[2]));
    if (mode == "tailcache") return tailCache (File (argv[2]));
    if (mode == "serve") return serve();
    if (mode == "diag") return diag (argv[2], String (argv[3]).getFloatValue(), String (argv[4]).getFloatValue());
    StringArray flags; for (int i = 3; i < argc; ++i) flags.add (argv[i]);
    if (mode == "convert") return convertCmd (File (argv[2]), flags);
    if (mode == "convset") { StringArray f2; for (int i = 4; i < argc; ++i) f2.add (argv[i]); return convSet (File (argv[2]), File (argv[3]), f2); }
    if (mode == "polish")
    {   // polish <list.txt (row<TAB>path)> <params.csv> <out.csv> [flags]
        StringArray f2; for (int i = 5; i < argc; ++i) f2.add (argv[i]);
        StringArray lines; lines.addLines (File (argv[2]).loadFileAsString()); lines.removeEmptyStrings();
        std::ofstream csv (argv[4], std::ios::app);
        for (auto& l : lines) polishCmd (File (l.fromFirstOccurrenceOf ("\t", false, false)), File (argv[3]), l.upToFirstOccurrenceOf ("\t", false, false).getIntValue(), f2, &csv);
        return 0;
    }
    if (mode == "presets") return presetsCmd (File (argv[2]));
    if (mode == "cancel") return cancelCmd (File (argv[2]));
    if (mode == "shot") { StringArray f2; for (int i = 2; i < argc; ++i) f2.add (argv[i]); return shotCmd (f2); }
    if (mode == "prep") { StringArray f2; for (int i = 2; i < argc; ++i) f2.add (argv[i]); return prepCmd (f2); }
    if (mode == "rtconv") return rtConv (File (argv[2]));
    if (mode == "modelrt") return modelRt (File (argv[2]));
    if (mode == "batch") { StringArray f2; for (int i = 3; i < argc; ++i) f2.add (argv[i]); return batchCmd (File (argv[2]), f2); }
    if (mode == "budcheck")
    {   // the flat build-up's convolution = the direct convolution with its FIR (random block sizes, a cross-fade in the middle)
        bool ok = true;
        for (int L : { 48, 700, 3000, 14400 })
        {
            auto h = rv::flatbu::design (L, -0.3, 777); auto f = rv::flatbu::makeFir (h);
            auto h2 = rv::flatbu::design (L, 0.4, 778); auto f2 = rv::flatbu::makeFir (h2);
            rv::flatbu::Conv c; c.prepare (14400 + 64);
            const int N = 48000; std::vector<float> x (N), y (N); Random r (5); for (auto& v : x) v = r.nextFloat() * 2 - 1;
            y = x; float xf = 1; int p = 0; const int fadeAt = 20000; const float st = 1.0f / 960;
            while (p < N) { int m = std::min (N - p, 1 + r.nextInt (700)); if (p < fadeAt && p + m > fadeAt) m = fadeAt - p;
                            const bool fade = p >= fadeAt; if (p == fadeAt) xf = 0; c.process (y.data() + p, m, f.get(), fade ? f2.get() : nullptr, fade, xf, st); p += m; }
            double err = 0, ref = 0; float xx = 0;
            for (int i = 0; i < N; ++i)
            {
                double a = 0, b = 0; for (int q = 0; q < L && q <= i; ++q) { a += h[(size_t) q] * x[(size_t) (i - q)]; b += h2[(size_t) q] * x[(size_t) (i - q)]; }
                double want = a; if (i >= fadeAt) { xx = std::min (1.0f, xx + st); want = a + xx * (b - a); }
                if (err < 1e-3 && std::abs (want - y[(size_t) i]) >= 1e-3) std::cout << "  first bad at " << i << " want " << want << " got " << y[(size_t) i] << std::endl;
                err = std::max (err, std::abs (want - y[(size_t) i])); ref = std::max (ref, std::abs (want));
            }
            std::cout << "L " << L << ": max error " << err << " of " << ref << std::endl; ok = ok && err < 1e-4 * ref;
        }
        std::cout << (ok ? "BUDCHECK OK" : "BUDCHECK FAIL") << std::endl; return ok ? 0 : 1;
    }
    if (mode == "rendertime")
    {
        auto x = XmlDocument::parse (File (argv[2])); IRFitProcessor q; q.setRateAndBufferSizeDetails (48000, 256); q.prepareToPlay (48000, 256);
        q.applyStateXml (*x, true); q.pushNow();
        AudioBuffer<float> b; q.renderWetIR (b, 4.0);
        const auto t0 = Time::getMillisecondCounterHiRes(); for (int i = 0; i < 20; ++i) q.renderWetIR (b, 4.0);
        std::cout << "render " << String ((Time::getMillisecondCounterHiRes() - t0) / 20, 1) << " ms" << std::endl; return 0;
    }
    if (mode == "budtime")
    {
        for (int L : { 1000, 3000, 6000, 14400 }) for (int it : { 6, 12, 24 })
        {
            const auto t0 = Time::getMillisecondCounterHiRes(); int n = 0;
            while (Time::getMillisecondCounterHiRes() - t0 < 500) { auto h = rv::flatbu::design (L, -0.2, 777 + (unsigned) n, it); auto f = rv::flatbu::makeFir (h); ++n; }
            std::cout << "L " << L << " iters " << it << ": " << String ((Time::getMillisecondCounterHiRes() - t0) / n, 2) << " ms per design" << std::endl;
        }
        return 0;
    }
    if (mode == "reso") { StringArray f2; for (int i = 2; i < argc; ++i) f2.add (argv[i]); return resoCmd (f2); }
    if (mode == "hybrid") return hybridCmd (File (argv[2]), File (argv[3]));
    return mapOnly();
}
