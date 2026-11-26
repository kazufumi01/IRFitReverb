#include "IRFitProcessor.h"
#include "IRFitEditor.h"

const char* const IRFitProcessor::kHybIds[] = { "erBlend", "tailBlend", "hSplit", "irStart", "irAttack", "irLength", "irSize", "irDecL", "irDecM", "irDecH",
                                                "irReverse", "irWidth", "irErLvl", "irTailLvl", "irLink" };
const char* const IRFitProcessor::kMacroIds[] = { "character", "era", "predelay", "decay", "size", "tone", "early", "buildup", "density", "width", "lowcut", "highcut", "mix" };

juce::AudioProcessorValueTreeState::ParameterLayout IRFitProcessor::layout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout L;
    auto fl = [&] (const char* id, const char* name, float lo, float hi, float def, float centre, std::function<String (float, int)> txt, const char* unit = "")
    {
        NormalisableRange<float> r (lo, hi); if (centre > 0) r.setSkewForCentre (centre);
        L.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction (std::move (txt))));
    };
    auto sgn = [] (float v, int dec, const char* u) { return (v > 0.0005f ? "+" : "") + (dec == 0 ? String (roundToInt (v)) : String (v, dec)) + u; };
    L.add (std::make_unique<AudioParameterChoice> (ParameterID { "character", 1 }, "Character", StringArray { "All", "Hall", "Room", "Chamber", "Plate", "Ambient" }, 0));
    L.add (std::make_unique<AudioParameterChoice> (ParameterID { "era", 1 }, "Era", StringArray { "Any", "Vintage", "Modern" }, 0));
    fl ("predelay", "Pre-Delay", 0.0f, 200.0f, 10.0f, 30.0f, [] (float v, int) { return v < 10 ? String (v, 1) + " ms" : String (roundToInt (v)) + " ms"; }, "ms");
    fl ("decay", "Decay", 0.2f, 15.0f, 2.0f, 2.0f, [] (float v, int) { return String (v, v < 10 ? 2 : 1) + " s"; }, "s");
    fl ("size", "Size", 50.0f, 200.0f, 100.0f, 100.0f, [] (float v, int) { return String (roundToInt (v)) + " %"; }, "%");
    fl ("tone", "Tone", -100.0f, 100.0f, 0.0f, -1, [sgn] (float v, int) { return sgn (v, 0, ""); });
    fl ("early", "Early", -24.0f, 24.0f, 0.0f, -1, [sgn] (float v, int) { return sgn (v, 1, " dB"); }, "dB");
    fl ("buildup", "Build-up", -100.0f, 100.0f, 0.0f, -1, [sgn] (float v, int) { return sgn (v, 0, ""); });
    fl ("density", "Density", -100.0f, 100.0f, 0.0f, -1, [sgn] (float v, int) { return sgn (v, 0, ""); });
    fl ("width", "Width", 0.0f, 150.0f, 100.0f, -1, [] (float v, int) { return String (roundToInt (v)) + " %"; }, "%");
    fl ("lowcut", "Low Cut", 20.0f, 1000.0f, 20.0f, 150.0f, [] (float v, int) { return v <= 20.5f ? String ("Off") : String (roundToInt (v)) + " Hz"; }, "Hz");
    fl ("highcut", "High Cut", 1000.0f, 20000.0f, 20000.0f, 6000.0f, [] (float v, int) { return v >= 19950.0f ? String ("Off") : v >= 10000 ? String (v / 1000, 1) + " kHz" : String (v / 1000, 2) + " kHz"; }, "Hz");
    fl ("mix", "Mix", 0.0f, 100.0f, 30.0f, -1, [] (float v, int) { return String (roundToInt (v)) + " %"; }, "%");
    L.add (std::make_unique<AudioParameterBool> (ParameterID { "refine", 1 }, "Refine", true));
    // (0.3.0) the hybrid (at their defaults the plug-in is the 0.2.x one)
    auto pct = [] (float v, int) { return String (roundToInt (v)) + " %"; };
    auto ms = [] (float v, int) { return v < 10 ? String (v, 1) + " ms" : String (roundToInt (v)) + " ms"; };
    fl ("erBlend", "ER IR Mix", 0.0f, 100.0f, 0.0f, -1, pct, "%");
    fl ("tailBlend", "Tail IR Mix", 0.0f, 100.0f, 0.0f, -1, pct, "%");
    fl ("hSplit", "ER / Tail Split", 20.0f, 300.0f, 80.0f, 80.0f, ms, "ms");
    fl ("irStart", "IR Start", 0.0f, 500.0f, 0.0f, 60.0f, ms, "ms");
    fl ("irAttack", "IR Attack", 0.0f, 300.0f, 0.0f, 40.0f, ms, "ms");
    fl ("irLength", "IR Length", 5.0f, 100.0f, 100.0f, -1, pct, "%");
    fl ("irSize", "IR Size", 50.0f, 200.0f, 100.0f, 100.0f, pct, "%");
    fl ("irDecL", "IR Decay Low", 25.0f, 400.0f, 100.0f, 100.0f, pct, "%");
    fl ("irDecM", "IR Decay Mid", 25.0f, 400.0f, 100.0f, 100.0f, pct, "%");
    fl ("irDecH", "IR Decay High", 25.0f, 400.0f, 100.0f, 100.0f, pct, "%");
    L.add (std::make_unique<AudioParameterBool> (ParameterID { "irReverse", 1 }, "IR Reverse", false));
    fl ("irWidth", "IR Width", 0.0f, 200.0f, 100.0f, -1, pct, "%");
    fl ("irErLvl", "IR ER Level", -24.0f, 12.0f, 0.0f, -1, [sgn] (float v, int) { return sgn (v, 1, " dB"); }, "dB");
    fl ("irTailLvl", "IR Tail Level", -24.0f, 12.0f, 0.0f, -1, [sgn] (float v, int) { return sgn (v, 1, " dB"); }, "dB");
    L.add (std::make_unique<AudioParameterBool> (ParameterID { "irLink", 1 }, "IR Follows Knobs", true));
    return L;
}

IRFitProcessor::IRFitProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "IRFIT", layout())
{
    eng = std::make_unique<ERProcessor>(); eng->flatBuildUp = std::getenv ("IRFIT_SPARSE_BU") == nullptr;      // (0.3.2) no fixed resonances from the build-up
    eng->reverbGapMs = std::getenv ("IRFIT_GAP0") ? 0 : 20;     // (0.2.3) a moving knob re-sets the tail at most every 20 ms
    // the settings every model shares, then the product's own fixed ones (no Auto Level: the source never changes;
    // (0.2.0) the engine at 100 % wet: the post stage and the dry / wet mix are the product's)
    for (const auto& kv : irfit::kBase) eng->setParam (kv.first, kv.second);
    eng->setParam ("autoLvl", 0.0f); eng->setParam ("out", 0.0f); eng->setParam ("tOn", 1.0f); eng->setParam ("erOn", 1.0f); eng->setParam ("mix", 100.0f);
    eng->undo.clearUndoHistory();
    for (auto* p : eng->getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto id = rp->getParameterID();
            auto* r = eng->apvts.getRawParameterValue (id); rawOf[rp] = r;
            // every value once through its normalisation (see writeExact): a parameter written and restored = one that never moved
            writeExact (rp, r, r->load());
            const bool skip = id == "mix" || id == "out" || id == "autoLvl";
            skipParam.push_back (skip);
            baseline.push_back ({ rp, r->load() });
        }
    for (int i = 0; i < kNumMacros; ++i) { raw[(size_t) i] = apvts.getRawParameterValue (kMacroIds[i]); seen[(size_t) i] = -1e9f; }
    for (int i = 0; i < kNumHyb; ++i) hRaw[(size_t) i] = apvts.getRawParameterValue (kHybIds[i]);
    // the engine parameters map() writes, in its order (found once: no string look-ups on the audio thread)
    irfit::map (macros(), res);
    for (int i = 0; i < res.n; ++i)
    {
        auto* p = eng->apvts.getParameter (res.v[i].first); jassert (p != nullptr);
        dst.push_back (p); dstRaw.push_back (eng->apvts.getRawParameterValue (res.v[i].first));
        const juce::String id (res.v[i].first); isSpread.push_back (id == "tSpread" || id == "tShape" || id == "tSpreadHi");
        if (id == "mix") mixIdx = i;
    }
    sent.assign (dst.size(), std::numeric_limits<float>::quiet_NaN());
    bank.scan();
    pushNow (true);
    lastSeen = macros(); markClean();
    startTimerHz (10);
}

IRFitProcessor::~IRFitProcessor() { stopTimer(); conv.reset(); joinRefine(); joinHybrid(); }

bool IRFitProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    if (l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    const auto in = l.getMainInputChannelSet();
    return in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

irfit::Macros IRFitProcessor::macros() const
{
    irfit::Macros m;
    auto v = [this] (int i) { return raw[(size_t) i] != nullptr ? raw[(size_t) i]->load() : 0.0f; };
    m.character = (int) v (0); m.era = (int) v (1); m.preDelay = v (2); m.decay = v (3); m.size = v (4); m.tone = v (5); m.early = v (6);
    m.buildUp = v (7); m.density = v (8); m.width = v (9); m.lowCut = v (10); m.highCut = v (11); m.mix = v (12);
    return m;
}

void IRFitProcessor::setMacros (const irfit::Macros& m)
{
    const float v[kNumMacros] = { (float) m.character, (float) m.era, m.preDelay, m.decay, m.size, m.tone, m.early, m.buildUp, m.density, m.width, m.lowCut, m.highCut, m.mix };
    for (int i = 0; i < kNumMacros; ++i)
        if (auto* p = apvts.getParameter (kMacroIds[i])) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (v[i])); p->endChangeGesture(); }
}

bool IRFitProcessor::sameShape (const irfit::Macros& a, const irfit::Macros& b)
{
    return a.preDelay == b.preDelay && a.decay == b.decay && a.size == b.size && a.tone == b.tone && a.early == b.early && a.buildUp == b.buildUp
        && a.density == b.density && a.width == b.width && a.lowCut == b.lowCut && a.highCut == b.highCut;
}

void IRFitProcessor::loadModel (int e)
{
    if (e < 0 || e >= irfit::kNumEntries) return;
    activateLibrary();
    auto m = irfit::macrosOf (e, macros());
    setMacros (m);
    // a library model is reproduced exactly by the mapping itself: no trim, nothing to refine
    joinRefine(); setTrim ({});
    { std::lock_guard<std::mutex> l (refMutex); refDone = macros(); refDoneValid = true; }
}

double IRFitProcessor::getTailLengthSeconds() const
{
    const auto m = macros();
    return juce::jlimit (0.5, 40.0, 2.0 * m.decay * std::pow (2.0, std::abs (m.tone) / 100.0) + m.preDelay / 1000.0 + 0.3);
}

// ---------------------------------------------------------------------------------------------------------------------
//  model mode
// ---------------------------------------------------------------------------------------------------------------------
irfit::PostSettings IRFitProcessor::effectivePost() const
{
    irfit::PostSettings p; if (! advS.on) return p;
    if (modelOn) p = model.post;
    for (auto& g : p.fine) g = g * juce::jlimit (0.0f, 200.0f, advS.fineAmt) / 100.0f;
    for (int k = 0; k < irfit::kSideBands; ++k)
    {
        const float w = juce::jlimit (0.0f, 300.0f, advS.width[(size_t) k]);
        p.side[(size_t) k] = juce::jlimit (-40.0f, 15.0f, p.side[(size_t) k] + 20.0f * std::log10 (std::max (0.5f, w) / 100.0f));
    }
    return p;
}

void IRFitProcessor::publishCore()
{
    { juce::SpinLock::ScopedLockType l (coreLock); pendCore = modelOn ? model.core : irfit::ModelCore(); pendPost = effectivePost(); }
    ++coreVersion;
}

void IRFitProcessor::writeEngineValues (const std::vector<std::pair<juce::RangedAudioParameter*, float>>& v)
{
    for (auto& [p, x] : v) { auto it = rawOf.find (p); writeExact (p, it != rawOf.end() ? it->second : nullptr, x); }      // (as a host's automation does)
}

IRFitProcessor::SwitchScope::SwitchScope (IRFitProcessor& q) : p (q), own (q.switchDepth++ == 0)
{
    if (! own) return;
    p.fadeReq = true;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (now - p.lastBlockMs.load() < 200)               // audio is running: wait for the wet to be silent
        for (int k = 0; k < 80 && ! p.wetMuted.load(); ++k) juce::Thread::sleep (1);
}
IRFitProcessor::SwitchScope::~SwitchScope()
{
    if (--p.switchDepth > 0 || ! own) return;
    p.eng->snapReq = true; p.eng->requestTransition (true);     // the tail starts clean, the ER fades through its own silent hold
    p.fadeReq = false;
}

void IRFitProcessor::activateModel (const irfit::UserModel& m, bool toAnchor)
{
    SwitchScope sw (*this);
    if (! m.valid()) return;
    joinRefine();
    model = m; modelOn = true;
    {   // the anchor on the knobs' own grid (a value through the parameter's normalisation comes back a little different):
        // there the mapping gives exactly the model's values
        auto snap = [this] (const char* id, float v) { auto* p = apvts.getParameter (id); return p->convertFrom0to1 (p->convertTo0to1 (v)); };
        auto& A = model.core.anchor;
        A.preDelay = snap ("predelay", A.preDelay); A.decay = snap ("decay", A.decay); A.size = snap ("size", A.size); A.tone = snap ("tone", A.tone);
        A.early = snap ("early", A.early); A.buildUp = snap ("buildup", A.buildUp); A.density = snap ("density", A.density); A.width = snap ("width", A.width);
        A.lowCut = snap ("lowcut", A.lowCut); A.highCut = snap ("highcut", A.highCut);
    }
    std::vector<std::pair<juce::RangedAudioParameter*, float>> v;
    for (size_t i = 0; i < baseline.size(); ++i)
    {
        if (skipParam[i]) continue;
        auto* p = baseline[i].first; const auto id = p->getParameterID();
        float val = p->convertFrom0to1 (p->getDefaultValue()); bool found = false;
        for (auto& kv : m.eng) if (kv.first == id) { val = kv.second; found = true; break; }
        juce::ignoreUnused (found);
        v.push_back ({ p, val });
    }
    writeEngineValues (v);
    publishCore();                                   // (the core before the knobs: the audio thread sees the new model first)
    if (toAnchor)
    {
        auto a = model.core.anchor; const auto cur = macros();
        a.mix = cur.mix; a.character = cur.character; a.era = cur.era;
        setMacros (a);
    }
    setTrim ({});
    { std::lock_guard<std::mutex> l (refMutex); refDone = macros(); refDoneValid = true; }
    updateHybrid();
}

void IRFitProcessor::activateLibrary()
{
    if (! modelOn) return;
    SwitchScope sw (*this);
    joinRefine();
    modelOn = false; model = irfit::UserModel();
    std::vector<std::pair<juce::RangedAudioParameter*, float>> v;
    for (size_t i = 0; i < baseline.size(); ++i) if (! skipParam[i]) v.push_back (baseline[i]);
    writeEngineValues (v);
    publishCore();
    updateHybrid();
}

irfit::Feat IRFitProcessor::targetFor (const irfit::Macros& m) const
{
    auto T = irfit::targetOf (m);
    if (! modelOn) return T;
    const auto A = irfit::targetOf (model.core.anchor), F = model.feat;
    return { F.lt + T.lt - A.lt, F.tilt + T.tilt - A.tilt, F.c80 + T.c80 - A.c80, F.edtr + T.edtr - A.edtr };
}

// ---------------------------------------------------------------------------------------------------------------------
// the mapping -> the engine's parameters (only the ones that moved). The engine glides them (FDN gains / sizes, the ER's
// cross-fade, filters, delays), as it does for host automation. Called on the audio thread, about every 10 ms while knobs move.
void IRFitProcessor::push()
{
    held = false;
    irfit::map (macros(), res, trim(), audioCore.active ? &audioCore : nullptr);
    jassert (res.n == (int) dst.size());
    const bool spreadOk = sinceSpread >= (int) (0.06 * sr); bool spreadSent = false;
    for (int i = 0; i < res.n && i < (int) dst.size(); ++i)
    {
        if (i == mixIdx) continue;                                            // (the engine stays at 100 % wet)
        const float x = res.v[i].second;
        if (x == sent[(size_t) i]) continue;
        if (isSpread[(size_t) i]) { if (! spreadOk) { held = true; continue; } spreadSent = true; }
        sent[(size_t) i] = x;
        writeExact (dst[(size_t) i], dstRaw[(size_t) i], x);                 // (as a host's automation does)
    }
    if (spreadSent) sinceSpread = 0;
    ++pushes;
}

void IRFitProcessor::pushNow (bool instant)
{
    {   // (not on the audio thread: take a published model / post stage now)
        juce::SpinLock::ScopedLockType l (coreLock);
        if (coreVersion.load() != coreSeen) { audioCore = pendCore; post.setTarget (pendPost, true); coreSeen = coreVersion.load(); std::fill (sent.begin(), sent.end(), std::numeric_limits<float>::quiet_NaN()); }
    }
    for (int i = 0; i < kNumMacros; ++i) seen[(size_t) i] = raw[(size_t) i]->load();
    sinceSpread = 1 << 20; push(); dirty = held; since = 0;
    if (instant) eng->snapReq = true;
}

void IRFitProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate; maxBlock = std::max (32, samplesPerBlock);
    dryBuf.setSize (2, maxBlock);
    post.prepare (sr);
    pushNow();
    post.setTarget (effectivePost(), true);
    eng->setRateAndBufferSizeDetails (sampleRate, maxBlock);
    eng->prepareToPlay (sampleRate, maxBlock);       // (takes every setting at once)
    joinHybrid(); hyb.prepare (sampleRate, maxBlock); pendReady = false; pendSet.reset(); builtSrc = {}; curFilt = {}; doneFilt = {}; wantSrc = {};
    const float m = juce::jlimit (0.0f, 1.0f, raw[12]->load() / 100.0f); dryG = std::min (1.0f, 2 * (1 - m)); wetG = std::min (1.0f, 2 * m);
}

void IRFitProcessor::processChunk (float* L, float* R, int n, juce::MidiBuffer& midi)
{
    dryBuf.copyFrom (0, 0, L, n); dryBuf.copyFrom (1, 0, R, n);
    // (0.3.0) the hybrid: a new IR for a part (rendered on the worker), the ALGO / IR balance of each part
    if (pendReady.load (std::memory_order_acquire) && hyb.install (pendSet))
    {
        loadedCut = pendCut; pendReady.store (false, std::memory_order_release);
    }
    if (const auto* ns = hyb.newest()) for (int k = 0; k < 2; ++k) loadedId[(size_t) k] = ns->part[(size_t) k] ? ns->part[(size_t) k]->id : -1;
    const bool okE = loadedId[0] == wantIdA[0].load(), okT = loadedId[1] == wantIdA[1].load();
    const float bE = okE ? juce::jlimit (0.0f, 1.0f, hRaw[hErBlend]->load() / 100.0f) : 0.0f, bT = okT ? juce::jlimit (0.0f, 1.0f, hRaw[hTailBlend]->load() / 100.0f) : 0.0f;
    const float hp = juce::MathConstants<float>::halfPi, aE = bE > 0 ? std::cos (bE * hp) : 1.0f, aT = bT > 0 ? std::cos (bT * hp) : 1.0f;
    eng->extErG.store (aE, std::memory_order_relaxed); eng->extTailG.store (aT, std::memory_order_relaxed);
    float* ch[2] = { L, R }; juce::AudioBuffer<float> b (ch, 2, n);
    eng->processBlock (b, midi);                     // -> the wet signal (the engine at 100 %)
    if (audioCore.active)
        post.setTapTracking (eng->value ("predelay") - audioCore.predelay, eng->value ("size") / std::max (1.0f, audioCore.sizeP),
                             eng->value ("level") - audioCore.p[irfit::p_level] + (std::getenv ("IRFIT_TAPS_022") ? 0.0f : 20.0f * std::log10 (ERProcessor::kWetCal))       /* (0.3.0) the taps were fitted before the wet bus calibration: 0.2.x played them 12 dB hot */ + (aE < 1.0f ? 20.0f * std::log10 (std::max (1e-8f, aE)) : 0.0f));     // (the taps are the ALGO ER's)
    post.process (L, R, n, dryBuf.getReadPointer (0), dryBuf.getReadPointer (1));      // (+ the model's early taps of the input)
    {
        const bool link = hRaw[hLink]->load() > 0.5f;
        const float dE = link ? raw[6]->load() - hRefEarly.load() : 0.0f;
        irfit::LiveSpec ls;
        // (the IR at the reference energy before the wet bus calibration, as the model was fitted: x kWetCal as the engine's own wet)
        ls.erG = bE > 0 ? std::sin (bE * hp) * ERProcessor::kWetCal * juce::Decibels::decibelsToGain (hRaw[hErLvl]->load() + 0.5f * dE) : 0.0f;
        ls.tailG = bT > 0 ? std::sin (bT * hp) * ERProcessor::kWetCal * juce::Decibels::decibelsToGain (hRaw[hTailLvl]->load() - 0.5f * dE) : 0.0f;
        ls.shiftMs = (link ? raw[2]->load() - hRefPre.load() : 0.0f) + loadedCut;
        ls.width = (link ? raw[9]->load() / std::max (1.0f, hRefWidth.load()) : 1.0f) * hRaw[hWidth]->load() / 100.0f;
        ls.lowCut = link ? raw[10]->load() : 20.0f; ls.highCut = link ? raw[11]->load() : 20000.0f;
        hyb.setLive (ls);
        hyb.process (dryBuf.getReadPointer (0), dryBuf.getReadPointer (1), L, R, n);
    }
    // the engine's own mix law: the dry stays at full level up to 50 %, the wet from 50 % on (gains glide)
    const float m = juce::jlimit (0.0f, 1.0f, raw[12]->load() / 100.0f), dT = std::min (1.0f, 2 * (1 - m)), wT = std::min (1.0f, 2 * m);
    const float* dL = dryBuf.getReadPointer (0); const float* dR = dryBuf.getReadPointer (1);
    const bool want0 = fadeReq.load(); const float dn = (float) (1.0 / (0.010 * sr)), up = (float) (1.0 / (0.030 * sr));
    for (int i = 0; i < n; ++i)
    {
        dryG += 0.003f * (dT - dryG); wetG += 0.003f * (wT - wetG);
        if (want0) { swG = std::max (0.0f, swG - dn); swHold = (int) (0.040 * sr); }
        else if (swHold > 0) { --swHold; swG = 0; }
        else if (swG < 1.0f) swG = std::min (1.0f, swG + up);
        const float wg = wetG * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * swG));
        L[i] = dL[i] * dryG + L[i] * wg; R[i] = dR[i] * dryG + R[i] * wg;
    }
    wetMuted = want0 && swG <= 0.0f;
    if (wetMuted.load()) { if (! hybResetReq) { hyb.reset(); hybResetReq = true; } }      // (a switch: the IR's tail restarts clean, as the engine's)
    else hybResetReq = false;
}

void IRFitProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    lastBlockMs = juce::Time::getMillisecondCounterHiRes();
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2) return;
    if (getTotalNumInputChannels() == 1) buffer.copyFrom (1, 0, buffer, 0, 0, n);
    // a new model / post stage from the message thread (a try-lock: the audio thread never waits)
    if (const int v = coreVersion.load(); v != coreSeen)
    {
        juce::SpinLock::ScopedTryLockType l (coreLock);
        if (l.isLocked())
        {
            audioCore = pendCore; post.setTarget (pendPost, false); coreSeen = v; dirty = true;
            std::fill (sent.begin(), sent.end(), std::numeric_limits<float>::quiet_NaN());
        }
    }
    bool charChanged = false;
    for (int i = 0; i < kNumMacros; ++i)
    {
        const float v = raw[(size_t) i]->load();
        if (v != seen[(size_t) i])
        {
            if (i <= 1 && seen[(size_t) i] > -1e8f) charChanged = true;          // (Character / Era: another space)
            if (i == 2 || i == 4) sinceDelayKnob = 0;                            // (Pre-Delay / Size)
            seen[(size_t) i] = v; dirty = true;
        }
    }
    if (const int tv = trimVersion.load(); tv != trimSeen) { trimSeen = tv; dirty = true; }
    since += n; sinceSpread += n; sinceDelayKnob = std::min (sinceDelayKnob + n, 1 << 30);
    if (charChanged)
    {   // (0.2.1) another character / era: at once, no morph (the tail starts anew, the ER swaps while silent)
        sinceSpread = 1 << 20; push(); dirty = held; since = 0; sinceDelayKnob = 1 << 30;
        eng->snapReq = true; eng->requestTransition (true);
    }
    if (dirty && since >= (int) (pushMs * 0.001 * sr)) { push(); dirty = held; since = 0; }       // (a held build-up change goes in a later block)
    // (0.2.1) the tail's delays move with at most ~7 cents of pitch (a refine, Decay / Tone through the neighbours); ~50 cents for
    // 0.5 s after Size / Pre-Delay move. 0.2.0: 5 % (85 cents), heard as the head of a note going flat / sharp after a change
    eng->tailGlideLim = sinceDelayKnob < (int) (0.5 * sr) ? 0.03 : 0.004;
    float* L = buffer.getWritePointer (0); float* R = buffer.getWritePointer (1);
    for (int i0 = 0; i0 < n; i0 += maxBlock) processChunk (L + i0, R + i0, std::min (maxBlock, n - i0), midi);
    // a safety net: a non-finite sample mutes the block and restarts the engine's state
    bool bad = false;
    for (int c = 0; c < 2 && ! bad; ++c) { const float* d = buffer.getReadPointer (c); for (int i = 0; i < n; ++i) if (! std::isfinite (d[i])) { bad = true; break; } }
    if (bad) { buffer.clear(); nonFinite = true; eng->prepareToPlay (sr, maxBlock); post.reset(); }
}

// ---------------------------------------------------------------------------------------------------------------------
void IRFitProcessor::timerCallback()
{
    if (++ticks % 5 == 0) eng->undo.clearUndoHistory();          // (the hidden engine's parameters move with every knob: keep its undo list empty)
    // ---- the IR conversion
    if (conv)
    {
        conv->tick();
        const int s = conv->state.load();
        if (s != convStateSeen)
        {
            convStateSeen = s;
            if (s == irfit::Converter::done)
            {
                irfit::UserModel m;
                if (conv->takeResult (m))
                {
                    activateModel (m, true);
                    const auto f = savePresetAs (irfit::PresetBank::kConverted, m.name);
                    lastConvReport = m.name + ": score " + juce::String (m.info.score, 1) + " (Lab fit " + juce::String (m.info.labScore, 1) + ")  "
                                   + "T30 " + juce::String (m.info.t30, 1) + " %, EDT " + juce::String (m.info.edt, 1) + " %, C80 " + juce::String (m.info.c80, 2) + " dB, "
                                   + juce::String (juce::roundToInt (m.info.seconds)) + " s" + (f.existsAsFile() ? "" : "  (could not save the preset)");
                }
                ++convFinished;
            }
            else if (s == irfit::Converter::failed || s == irfit::Converter::cancelled)
            {
                lastConvReport = s == irfit::Converter::failed ? "Conversion failed: " + conv->error : juce::String ("Conversion cancelled");
                ++convFinished;
            }
        }
    }
    // ---- (0.3.1) the batch
    if (batch.running() || batch.finished.load() != batchSeen)
    {
        batch.tick();
        if (batch.finished.load() != batchSeen)
        {
            batchSeen = batch.finished.load();
            lastBatchReport = "Batch \"" + batch.folderName() + "\": " + batch.summary() + " in " + juce::String (juce::roundToInt (batch.secondsTotal / 60.0 * 10) / 10.0, 1) + " min";
            bank.scan();
        }
    }
    updateHybrid();
    // ---- refine once the knobs have rested ~0.4 s (a move cancels a running one)
    const auto m = macros();
    if (m != lastSeen) { lastSeen = m; stableTicks = 0; if (refBusy.load()) refCancel = true; return; }
    if (++stableTicks < 4 || refBusy.load() || apvts.getRawParameterValue ("refine")->load() < 0.5f) return;
    { std::lock_guard<std::mutex> l (refMutex); if (refDoneValid && refDone == m) return; }
    if (modelOn && sameShape (irfit::snapToAnchor (m, model.core.anchor), model.core.anchor))
    {   // a model at its anchor plays exactly: no trim
        if (trim().lt != 0 || trim().tilt != 0 || trim().c80 != 0 || trim().edtr != 0) setTrim ({});
        std::lock_guard<std::mutex> l (refMutex); refDone = m; refDoneValid = true; return;
    }
    if (refWorker.joinable()) refWorker.join();
    refCancel = false; refBusy = true;
    const auto start = trim();
    const bool mo = modelOn; const irfit::ModelCore mc = mo ? model.core : irfit::ModelCore(); const irfit::Feat mf = model.feat;
    refWorker = std::thread ([this, m, start, mo, mc, mf]
    {
        double err = 0; const auto t = refine (m, start, 4, &err, &refCancel, mo ? &mc : nullptr, mo ? &mf : nullptr);
        if (! refCancel.load())
        {
            setTrim (t);
            std::lock_guard<std::mutex> l (refMutex); refDone = m; refDoneValid = true;
        }
        refBusy = false;
    });
}

double IRFitProcessor::featErr (const irfit::Feat& T, const irfit::Feat& g)
{   // in the score's units: T30 5 %, tilt 0.1, C80 1.5 dB, EDT 8 %
    return std::abs (g.lt - T.lt) / 0.05 + std::abs (g.tilt - T.tilt) / 0.1 + std::abs (g.c80 - T.c80) / 1.5 + std::abs (g.edtr - T.edtr) / 0.08;
}

bool IRFitProcessor::measureFeat (ERProcessor& e, const irfit::Result* r, irfit::Feat& out, double* energyDb)
{
    std::unordered_map<std::string, float> ov;
    if (r != nullptr) for (int i = 0; i < r->n; ++i) ov[r->v[i].first] = r->v[i].second;
    ERProcessor::ExportOpts o; o.sr = 48000; o.bits = 32; o.tail = true; o.normalize = false;
    juce::AudioBuffer<float> b; double g = 0;
    ERProcessor::tlOverride = r != nullptr ? &ov : nullptr; ERProcessor::tlMaxLenS = 8.0;
    e.renderIR (o, b, g);
    ERProcessor::tlOverride = nullptr; ERProcessor::tlMaxLenS = 0;
    const auto m = fit::measure (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
    if (energyDb) { double s = 0; for (int c = 0; c < 2; ++c) { const float* d = b.getReadPointer (c); for (int i = 0; i < b.getNumSamples(); ++i) s += 0.5 * (double) d[i] * d[i]; } *energyDb = 10 * std::log10 (s + 1e-30); }
    return irfit::featOf (m.t30, m.edt, irfit::c80FromOnset (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples()), out);
}

irfit::Trim IRFitProcessor::refine (const irfit::Macros& m, irfit::Trim t, int iters, double* errOut, std::atomic<bool>* cancel, const irfit::ModelCore* mc, const irfit::Feat* mf)
{
    // per feature a secant step: the trim moves by the measured gap over the measured gain (start: T30 1, tilt 1, C80 0.8, EDT 0.8
    // per unit of trim; kept within 0.25 .. 2), damped; the trim with the smallest error over the renders is kept
    auto T = irfit::targetOf (m);
    if (mc != nullptr && mf != nullptr)
    {   // a model: relative to its own measured features at its anchor
        const auto A = irfit::targetOf (mc->anchor);
        T = { mf->lt + T.lt - A.lt, mf->tilt + T.tilt - A.tilt, mf->c80 + T.c80 - A.c80, mf->edtr + T.edtr - A.edtr };
    }
    irfit::Trim best = t; double bestErr = 1e30;
    double gain[4] = { 1.0, 1.0, 0.8, 0.8 }, prevF[4] {}, prevT[4] {}; bool havePrev = false;
    const double lim[4] = { 0.7, 0.6, 12.0, 0.6 }; double firstLt = 0;
    irfit::Result r;
    for (int it = 0; it < iters; ++it)
    {
        if (cancel != nullptr && cancel->load()) break;
        irfit::map (m, r, t, mc);
        irfit::Feat f; if (! measureFeat (*eng, &r, f)) break;
        const double err = featErr (T, f), eLt = std::abs (f.lt - T.lt);
        if (it == 0) firstLt = eLt;
        // (Decay is the main knob: a trim is only taken if T30 is no worse than the plain mapping's, or within 3 %)
        if (err < bestErr && (it == 0 || eLt <= std::max (firstLt, 0.03))) { bestErr = err; best = t; }
        double* tv[4] = { &t.lt, &t.tilt, &t.c80, &t.edtr };
        const double fv[4] = { f.lt, f.tilt, f.c80, f.edtr }, tg[4] = { T.lt, T.tilt, T.c80, T.edtr };
        for (int k = 0; k < 4; ++k)
        {
            if (havePrev && std::abs (*tv[k] - prevT[k]) > 1e-6)
                gain[k] = std::clamp ((fv[k] - prevF[k]) / (*tv[k] - prevT[k]), 0.25, 2.0);
            prevF[k] = fv[k]; prevT[k] = *tv[k];
            *tv[k] = std::clamp (*tv[k] + 0.8 * (tg[k] - fv[k]) / gain[k], -lim[k], lim[k]);
        }
        havePrev = true;
    }
    ++refines;
    if (errOut) *errOut = bestErr;
    return best;
}

// ---------------------------------------------------------------------------------------------------------------------
//  (0.3.0) the hybrid
// ---------------------------------------------------------------------------------------------------------------------
irfit::BuildSpec IRFitProcessor::specFor (bool er) const
{
    irfit::BuildSpec s; const auto src = sourceIR (er); if (! src) return s;
    const auto m = macros(); const bool link = hRaw[hLink]->load() > 0.5f;
    const auto& A = model.core.anchor;
    auto q = [] (float v, float step) { return std::round (v / step) * step; };
    const float shift = link ? m.preDelay - hRefPre.load() : 0.0f, cut = shift < 0 ? std::ceil (-shift) : 0.0f;     // (earlier than the IR: cut from it)
    s.splitMs = q (hRaw[hSplit]->load(), 1); s.startMs = q (hRaw[hStart]->load(), 1) + cut; s.attackMs = q (hRaw[hAttack]->load(), 1);
    s.lengthPct = q (hRaw[hLength]->load(), 0.5f); s.reverse = ! er && hRaw[hReverse]->load() > 0.5f;
    const float sMacro = link ? m.size / std::max (1.0f, modelOn ? A.size : 100.0f) : 1.0f;
    s.stretch = q (juce::jlimit (0.5f, 2.0f, hRaw[hSize]->load() / 100.0f * sMacro), 0.005f);
    // Decay / Tone: the decay time per band relative to the anchor (a model) or to the IR's own (the library); the Size knob keeps it
    const double r = link ? m.decay / std::max (0.05, (double) (modelOn ? A.decay : src->midT())) : 1.0;
    const double dTilt = link ? irfit::kTiltB * std::log (r) + irfit::kToneScale * (m.tone - (modelOn ? A.tone : 0.0f)) : 0.0;
    const double tl[3] = { std::exp (-0.5 * dTilt), 1.0, std::exp (0.5 * dTilt) };
    const float ed[3] = { hRaw[hDecL]->load(), hRaw[hDecM]->load(), hRaw[hDecH]->load() };
    for (int b = 0; b < 3; ++b)
    {
        const double k = juce::jlimit (0.1, 10.0, r * tl[b] * ed[b] / 100.0);
        s.decay[(size_t) b] = (float) std::exp (std::round (std::log (k) * 400.0) / 400.0);
        if (std::abs (s.decay[(size_t) b] - 1.0f) < 1e-3f) s.decay[(size_t) b] = 1.0f;
    }
    s.tailK = q (juce::jlimit (0.5f, 2.0f, hRaw[hSize]->load() / 100.0f), 0.005f);
    return s;
}

void IRFitProcessor::updateHybrid (bool force)
{
    const irfit::IRPtr src[2] = { sourceIR (true), sourceIR (false) };
    {   // what the linked knobs are relative to: a model's anchor, or (the library) the IR as it is
        const auto& A = model.core.anchor;
        hRefPre = modelOn ? A.preDelay : src[0] ? src[0]->onsetMs : src[1] ? src[1]->onsetMs : 0.0f;
        hRefEarly = modelOn ? A.early : 0.0f; hRefWidth = modelOn ? A.width : 100.0f;
    }
    for (int k = 0; k < 2; ++k) if (src[k] != wantSrc[(size_t) k]) { wantSrc[(size_t) k] = src[k]; wantIdA[(size_t) k] = ++wantId[(size_t) k]; }
    hyb.collect();
    if (hybBusy.load() || pendReady.load()) { hybUpToDate = false; return; }
    joinHybrid();
    for (int k = 0; k < 2; ++k) if (doneFilt[(size_t) k]) curFilt[(size_t) k] = std::move (doneFilt[(size_t) k]);
    if (hybSr != sr) { hybSr = sr; builtSrc = {}; curFilt = {}; }               // (another sample rate: every IR again)
    ++ticksSinceBuild;
    irfit::BuildSpec spec[2]; bool need[2]; bool srcNew = false;
    for (int k = 0; k < 2; ++k)
    {
        spec[k] = specFor (k == 0);
        const float bl = hRaw[k == 0 ? hErBlend : hTailBlend]->load();
        const bool newSrc = src[k] != builtSrc[(size_t) k];
        need[k] = src[k] != nullptr && (newSrc || (spec[k] != builtSpec[(size_t) k] && (bl > 0 || force)));
        srcNew = srcNew || (newSrc && src[k] != nullptr);
    }
    const bool stable = spec[0] == lastSpec[0] && spec[1] == lastSpec[1];
    lastSpec[0] = spec[0]; lastSpec[1] = spec[1];
    if (! need[0] && ! need[1]) { hybUpToDate = true; return; }
    // a moving knob: a new IR at most every 300 ms (the convolution cross-fades to it); at rest / a new IR: now
    if (! force && ! srcNew && ! stable && ticksSinceBuild < 3) { hybUpToDate = false; return; }
    hybUpToDate = false; ticksSinceBuild = 0; hybBusy = true;
    const float cut = std::max (0.0f, spec[0].startMs - std::round (hRaw[hStart]->load()));
    struct Job { irfit::IRPtr src; irfit::BuildSpec spec; bool on; int id; };
    std::array<Job, 2> jobs;
    for (int k = 0; k < 2; ++k)
    {
        jobs[(size_t) k] = { src[k], spec[k], need[k], wantId[(size_t) k] };
        if (need[k]) { builtSrc[(size_t) k] = src[k]; builtSpec[(size_t) k] = spec[k]; }
    }
    const auto keep = curFilt; const double fs = sr;
    for (int k = 0; k < 2; ++k) if (! src[k]) builtSrc[(size_t) k] = nullptr;
    hybWorker = std::thread ([this, jobs, cut, keep, fs]
    {
        auto set = std::make_unique<irfit::FilterSet>();
        for (int k = 0; k < 2; ++k)
        {
            const auto& J = jobs[(size_t) k];
            if (! J.src) continue;                                              // (no IR for this part)
            if (! J.on) { set->part[(size_t) k] = keep[(size_t) k]; continue; }
            juce::AudioBuffer<float> b; irfit::buildPart (*J.src, J.spec, k == 0, b);
            irfit::resampleIR (b, irfit::hyb::kSR, fs);
            if (b.getNumSamples() > (int) (irfit::HybridDsp::kMaxLenS * fs)) b.setSize (2, (int) (irfit::HybridDsp::kMaxLenS * fs), true);
            set->part[(size_t) k] = irfit::makePartFilter (b, J.id);
            doneFilt[(size_t) k] = set->part[(size_t) k];
        }
        pendSet = std::move (set); pendCut = cut;
        pendReady.store (true, std::memory_order_release);
        ++hybBuilds;
        hybBusy = false;
    });
}

juce::String IRFitProcessor::loadSlotIR (bool er, const juce::File& f)
{
    auto d = std::make_shared<irfit::IRData>();
    if (auto err = irfit::loadIRFile (f, *d, ERProcessor::kRefWetDb); err.isNotEmpty()) return err;
    (er ? slotEr : slotTl) = std::move (d);
    updateHybrid();
    return {};
}

void IRFitProcessor::useModelIR (bool er) { (er ? slotEr : slotTl) = nullptr; updateHybrid(); }

juce::String IRFitProcessor::slotName (bool er) const
{
    const auto s = sourceIR (er); if (! s) return {};
    return slotCustom (er) ? s->name : s->name + " (model)";
}

void IRFitProcessor::renderWetIR (juce::AudioBuffer<float>& out, double maxLenS)
{
    ERProcessor::ExportOpts o; o.sr = 48000; o.bits = 32; o.tail = true; o.normalize = false; double g = 0;
    ERProcessor::tlMaxLenS = maxLenS; eng->renderIR (o, out, g); ERProcessor::tlMaxLenS = 0;
    irfit::Post::apply (effectivePost(), out.getWritePointer (0), out.getWritePointer (1), out.getNumSamples());
}

// ---------------------------------------------------------------------------------------------------------------------
//  state and presets
// ---------------------------------------------------------------------------------------------------------------------
juce::String IRFitProcessor::tableHash()
{
    static const juce::String h = []
    {
        uint64_t x = 1469598103934665603ull;
        auto mix = [&x] (const void* d, size_t n) { auto* b = (const unsigned char*) d; for (size_t i = 0; i < n; ++i) { x ^= b[i]; x *= 1099511628211ull; } };
        for (int e = 0; e < irfit::kNumEntries; ++e) { mix (irfit::kEntries[e].name, std::strlen (irfit::kEntries[e].name)); mix (irfit::kEntries[e].p, sizeof (irfit::kEntries[e].p)); }
        for (auto& kv : irfit::kBase) mix (&kv.second, sizeof (float));
        return juce::String::toHexString ((juce::int64) x);
    }();
    return h;
}

std::unique_ptr<juce::XmlElement> IRFitProcessor::stateXml (bool forPreset) const
{
    auto st = const_cast<juce::AudioProcessorValueTreeState&> (apvts).copyState(); st.setProperty ("version", JucePlugin_VersionString, nullptr);
    const auto t = trim(); st.setProperty ("trimLt", t.lt, nullptr); st.setProperty ("trimTilt", t.tilt, nullptr); st.setProperty ("trimC80", t.c80, nullptr); st.setProperty ("trimEdtr", t.edtr, nullptr);
    auto x = st.createXml(); if (x == nullptr) return nullptr;
    for (auto* c : { "MODEL", "SNAPSHOT", "ADV", "PRESETREF", "HYBRID" }) while (auto* e = x->getChildByName (c)) x->removeChildElement (e, true);
    if (modelOn) x->addChildElement (irfit::modelToXml (model).release());
    else
    {   // the library: the knobs are enough while the factory table is the same; a snapshot of the engine in case it ever changes
        auto* s = x->createNewChildElement ("SNAPSHOT"); s->setAttribute ("table", tableHash());
        for (auto& [p, n] : baseline) { juce::ignoreUnused (n); auto* e = s->createNewChildElement ("P"); const float v = rawOf.at (p)->load();
                                         e->setAttribute ("id", p->getParameterID()); e->setAttribute ("v", juce::String (v)); e->setAttribute ("b", irfit::floatBits (v)); }
    }
    {   auto* a = x->createNewChildElement ("ADV"); a->setAttribute ("on", advS.on ? 1 : 0); a->setAttribute ("fineAmt", advS.fineAmt);
        juce::StringArray w; for (auto v : advS.width) w.add (juce::String (v)); a->setAttribute ("width", w.joinIntoString (",")); }
    if (slotEr || slotTl)
    {   // (0.3.0) IRs loaded into the slots, embedded
        auto* h = x->createNewChildElement ("HYBRID");
        for (int k = 0; k < 2; ++k)
            if (const auto& sp = k == 0 ? slotEr : slotTl)
            { auto* e = h->createNewChildElement ("SLOT"); e->setAttribute ("part", k == 0 ? "er" : "tail"); e->setAttribute ("name", sp->name); e->addTextElement (irfit::encodeIR (*sp)); }
    }
    if (! forPreset)
    {
        auto* r = x->createNewChildElement ("PRESETREF");
        r->setAttribute ("factory", current.factory ? 1 : 0); r->setAttribute ("entry", current.entry); r->setAttribute ("folder", current.folder);
        r->setAttribute ("name", current.name); r->setAttribute ("file", current.file.getFullPathName());
    }
    return x;
}

bool IRFitProcessor::applyStateXml (const juce::XmlElement& xin, bool isPreset)
{
    SwitchScope sw (*this);
    juce::XmlElement x (xin);
    std::unique_ptr<juce::XmlElement> mdl, snap, advX, ref, hybX;
    auto take = [&x] (const char* nm) { std::unique_ptr<juce::XmlElement> r; if (auto* e = x.getChildByName (nm)) { r = std::make_unique<juce::XmlElement> (*e); x.removeChildElement (e, true); } return r; };
    mdl = take ("MODEL"); snap = take ("SNAPSHOT"); advX = take ("ADV"); ref = take ("PRESETREF"); hybX = take ("HYBRID");
    auto st = juce::ValueTree::fromXml (x);
    if (! st.isValid() || ! st.hasType (apvts.state.getType())) return false;
    joinRefine();
    const float mixBefore = raw[12]->load();
    // the model first (its engine settings and core go out before the knobs)
    irfit::UserModel m;
    if (mdl != nullptr && irfit::modelFromXml (*mdl, m)) activateModel (m, false);
    else if (snap != nullptr && snap->getStringAttribute ("table") != tableHash())
    {   // a library preset from another factory table: play its saved engine as a model (anchor = its knobs)
        irfit::UserModel s; s.name = "Snapshot";
        for (auto* p : snap->getChildIterator()) s.eng.push_back ({ p->getStringAttribute ("id"), irfit::floatFrom (p->getStringAttribute ("v"), p->getStringAttribute ("b")) });
        s.fillCoreFromEng();
        irfit::Macros a; { auto pv = [&st] (const char* id, float d) { for (auto c : st) if (c.getProperty ("id").toString() == id) return (float) c.getProperty ("value"); return d; };
            a.preDelay = pv ("predelay", 10); a.decay = pv ("decay", 2); a.size = pv ("size", 100); a.tone = pv ("tone", 0); a.early = pv ("early", 0); a.buildUp = pv ("buildup", 0);
            a.density = pv ("density", 0); a.width = pv ("width", 100); a.lowCut = pv ("lowcut", 20); a.highCut = pv ("highcut", 20000); }
        s.core.anchor = a;
        {   // its ER / tail energies and features, rendered now (rare)
            std::unordered_map<std::string, float> ov; for (auto& kv : s.eng) ov[kv.first.toStdString()] = kv.second;
            auto en = [&] (const char* off) { auto o2 = ov; if (off) o2[off] = 0; ERProcessor::tlOverride = &o2; ERProcessor::tlMaxLenS = 8;
                ERProcessor::ExportOpts o; o.sr = 48000; o.bits = 32; juce::AudioBuffer<float> b; double g; eng->renderIR (o, b, g); ERProcessor::tlOverride = nullptr; ERProcessor::tlMaxLenS = 0;
                double e = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) e += 0.5 * (double) b.getSample (c, i) * b.getSample (c, i);
                if (! off) { const auto mm = fit::measure (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples()); irfit::featOf (mm.t30, mm.edt, irfit::c80FromOnset (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples()), s.feat); }
                return 10 * std::log10 (e + 1e-30); };
            en (nullptr); s.core.eEr = en ("tOn"); s.core.eTail = en ("erOn");
        }
        activateModel (s, false);
    }
    else activateLibrary();
    apvts.replaceState (st);
    if (isPreset) if (auto* p = apvts.getParameter ("mix")) p->setValueNotifyingHost (p->convertTo0to1 (mixBefore));      // (a preset keeps the Mix)
    if (advX != nullptr)
    {
        advS.on = advX->getIntAttribute ("on", 1) != 0; advS.fineAmt = (float) advX->getDoubleAttribute ("fineAmt", 100);
        juce::StringArray w; w.addTokens (advX->getStringAttribute ("width"), ",", "");
        for (int k = 0; k < irfit::kSideBands; ++k) advS.width[(size_t) k] = k < w.size() ? w[k].getFloatValue() : 100.0f;
    }
    else advS = Adv();
    slotEr = slotTl = nullptr;
    if (hybX != nullptr)
        for (auto* e : hybX->getChildWithTagNameIterator ("SLOT"))
        {
            auto d = std::make_shared<irfit::IRData>();
            if (! irfit::decodeIR (e->getAllSubText().trim(), *d)) continue;
            d->name = e->getStringAttribute ("name");
            (e->getStringAttribute ("part") == "er" ? slotEr : slotTl) = std::move (d);
        }
    publishCore();
    updateHybrid();
    // the saved trim comes back with the knobs (so a session sounds as it was without waiting for a refine)
    setTrim ({ (double) st.getProperty ("trimLt", 0.0), (double) st.getProperty ("trimTilt", 0.0), (double) st.getProperty ("trimC80", 0.0), (double) st.getProperty ("trimEdtr", 0.0) });
    { std::lock_guard<std::mutex> l (refMutex); refDone = macros(); refDoneValid = st.hasProperty ("trimLt"); }
    if (ref != nullptr)
    {
        current = {}; current.factory = ref->getIntAttribute ("factory") != 0; current.entry = ref->getIntAttribute ("entry", -1);
        current.folder = ref->getStringAttribute ("folder"); current.name = ref->getStringAttribute ("name"); current.file = juce::File (ref->getStringAttribute ("file"));
        if (! current.factory && ! current.file.existsAsFile()) current.file = juce::File();
    }
    markClean();
    return true;
}

void IRFitProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = stateXml (false)) copyXmlToBinary (*xml, dest);
}

void IRFitProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size); if (xml == nullptr) return;
    if (applyStateXml (*xml, false)) eng->snapReq = true;                    // a loaded state: the tail jumps to it (no morph)
}

bool IRFitProcessor::loadPreset (const irfit::PresetRef& r)
{
    if (! r.valid()) return false;
    SwitchScope sw (*this);
    if (r.factory)
    {
        loadModel (r.entry);
        current = r; markClean(); return true;
    }
    auto x = juce::parseXML (r.file); if (x == nullptr) return false;
    if (! applyStateXml (*x, true)) return false;
    current = r; markClean();
    return true;
}

bool IRFitProcessor::stepPreset (int delta)
{
    bank.scan();
    auto r = bank.step (current, delta);
    return r.valid() && loadPreset (r);
}

juce::File IRFitProcessor::savePresetAs (const juce::String& folder, const juce::String& name, bool overwrite)
{
    const auto dir = bank.folderDir (folder.isEmpty() ? juce::String ("User") : folder); dir.createDirectory();
    const auto f = overwrite ? dir.getChildFile (irfit::PresetBank::legalName (name) + irfit::PresetBank::kExt) : bank.freeFile (folder, name);
    auto x = stateXml (true); if (x == nullptr || ! x->writeTo (f)) return {};
    bank.scan();
    current = {}; current.folder = folder; current.name = f.getFileNameWithoutExtension(); current.file = f;
    markClean();
    return f;
}

bool IRFitProcessor::saveCurrentPreset()
{
    if (! current.valid() || current.factory) return false;
    auto x = stateXml (true); if (x == nullptr || ! x->writeTo (current.file)) return false;
    markClean(); bank.scan(); return true;
}

bool IRFitProcessor::deletePreset (const irfit::PresetRef& r)
{
    if (r.factory || ! r.file.existsAsFile()) return false;
    const bool ok = r.file.moveToTrash() || r.file.deleteFile();
    bank.scan();
    if (ok && current == r) { current.file = juce::File(); }
    return ok;
}

bool IRFitProcessor::newFolder (const juce::String& name)
{
    const auto nm = irfit::PresetBank::legalName (name); if (nm.isEmpty()) return false;
    const bool ok = bank.folderDir (nm).createDirectory().wasOk(); bank.scan(); return ok;
}

bool IRFitProcessor::presetDirty() const
{
    const auto m = macros();
    if (modelOn != presetModelOn || (modelOn && model.name != presetModelName)) return true;
    return ! sameShape (irfit::snapToAnchor (m, presetMacros), presetMacros) || (! modelOn && (m.character != presetMacros.character || m.era != presetMacros.era));
}

juce::String IRFitProcessor::presetLabel() const
{
    if (current.valid()) return (current.factory ? current.folder.fromFirstOccurrenceOf ("Factory ", false, false) : current.folder) + " / " + current.name;
    if (modelOn) return model.name + " (not saved)";
    return "Init";
}

juce::String IRFitProcessor::convertIR (const juce::File& f)
{
    if (conv && conv->running()) return "A conversion is already running";
    if (batch.running()) return "A batch is running";
    if (! conv) conv = std::make_unique<irfit::Converter>();
    convStateSeen = -1;
    const auto err = conv->start (f, convOptions);
    convStateSeen = conv->state.load();
    if (err.isNotEmpty()) { lastConvReport = "Conversion failed: " + err; ++convFinished; }
    return err;
}

// ---- (0.3.1) batch + Lab export
juce::String IRFitProcessor::startBatch (const juce::File& folder, const irfit::Batch::Settings& s)
{
    batch.baseOptions = convOptions;
    batch.savePreset = [this] (const irfit::UserModel& m, const juce::String& fo, const juce::String& nm) { return writeModelPreset (m, fo, nm); };
    lastBatchReport = {};
    return batch.start (folder, s);
}

juce::File IRFitProcessor::writeModelPreset (const irfit::UserModel& m, const juce::String& folder, const juce::String& name)
{
    auto x = stateXml (true); if (x == nullptr) return {};
    for (auto* c : { "MODEL", "SNAPSHOT", "ADV", "HYBRID", "PRESETREF" }) while (auto* e = x->getChildByName (c)) x->removeChildElement (e, true);
    for (auto* a : { "trimLt", "trimTilt", "trimC80", "trimEdtr" }) x->setAttribute (a, 0);
    const auto& A = m.core.anchor;
    const std::pair<const char*, float> knob[] = { { "predelay", A.preDelay }, { "decay", A.decay }, { "size", A.size }, { "tone", A.tone }, { "early", A.early },
                                                   { "buildup", A.buildUp }, { "density", A.density }, { "width", A.width }, { "lowcut", A.lowCut }, { "highcut", A.highCut } };
    for (auto* e : x->getChildWithTagNameIterator ("PARAM"))
    {
        const auto id = e->getStringAttribute ("id");
        for (auto& [k, v] : knob) if (id == k) e->setAttribute ("value", (double) v);
        for (int i = 0; i < kNumHyb; ++i)
            if (id == kHybIds[i]) if (auto* p = apvts.getParameter (id)) e->setAttribute ("value", (double) p->convertFrom0to1 (p->getDefaultValue()));
    }
    x->addChildElement (irfit::modelToXml (m).release());
    {   auto* a = x->createNewChildElement ("ADV"); a->setAttribute ("on", 1); a->setAttribute ("fineAmt", 100); a->setAttribute ("width", "100,100,100,100,100"); }
    const auto dir = bank.folderDir (folder); dir.createDirectory();
    const auto f = dir.getChildFile (irfit::PresetBank::legalName (name) + irfit::PresetBank::kExt);
    return x->writeTo (f) ? f : juce::File();
}

juce::File IRFitProcessor::exportToLab (const juce::String& nameIn)
{
    std::vector<std::pair<juce::String, float>> v;
    for (auto* pr : eng->getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (pr))
            if (auto* r = eng->apvts.getRawParameterValue (rp->getParameterID())) v.push_back ({ rp->getParameterID(), r->load() });
    // the taps' anchor: where the model put them (the Lab moves them from there with its own Pre-Delay / Size / ER level)
    const auto& src = modelOn ? model.eng : v;
    const float pre = irfit::engValue (src, "predelay", 0), sz = irfit::engValue (src, "size", 100), lv = irfit::engValue (src, "level", 0);
    const auto name = nameIn.trim().isNotEmpty() ? nameIn.trim() : (current.valid() ? current.name : juce::String ("IR Fit"));
    const auto dir = irfit::labIrFitDir ({}); dir.createDirectory();
    const auto f = dir.getChildFile (irfit::PresetBank::legalName (name) + ".rvpreset");
    auto x = irfit::labPresetXml (name, v, effectivePost(), pre, sz, lv, "IR Fit Reverb: " + name);
    return x->writeTo (f) ? f : juce::File();
}

juce::AudioProcessorEditor* IRFitProcessor::createEditor() { return new IRFitEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new IRFitProcessor(); }
