#pragma once
// IR Fit Reverb (0.2.0): a model played directly - an IR converted to ALGO (or a saved snapshot of the engine).
// It holds every engine parameter (so it is reproduced without the IR and without the factory table), the product's
// post stage, its measured features and the knob positions where it plays exactly (the anchor).
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "IRFitMap.h"
#include "IRFitPost.h"
#include "IRFitHybrid.h"
#include <cstring>

namespace irfit
{
struct FitInfo
{
    double score = -1, labScore = -1, t30 = 0, edt = 0, c80 = 0, env = 0, specL = 0, specE = 0, iacc = 0, ned = 0;      // parts in the score's units (% / dB) and the totals
    double seconds = 0; int renders = 0;
    juce::String source;                       // the IR file it came from
};

struct UserModel
{
    juce::String name;
    std::vector<std::pair<juce::String, float>> eng;       // every engine parameter (id, value)
    ModelCore core;                                        // what the mapping needs (from eng + the measures)
    PostSettings post;                                     // the fitted post stage
    Feat feat; float onset = 0;                            // measured (product convention: C80 from the arrival)
    FitInfo info;
    IRPtr ir;                                              // (0.3.0) the IR it was fitted to (48 kHz, dry removed, at the reference energy): the hybrid
    bool valid() const { return core.active && ! eng.empty(); }

    float get (const char* id, float def = 0) const { for (auto& kv : eng) if (kv.first == id) return kv.second; return def; }
    // the mapped parameters (kPid) and the RT points from eng
    void fillCoreFromEng()
    {
        for (int i = 0; i < kNumP; ++i) core.p[i] = get (kPid[i]);
        core.predelay = get ("predelay"); core.sizeP = get ("size", 100); core.eqHP = get ("eqHP", 20);
        const char* rf[5] = { "rtF1", "rtF2", "rtF3", "rtF4", "rtF5" };
        static const float defF[5] = { 80, 250, 1000, 4000, 12000 };
        for (int k = 0; k < 5; ++k) core.rtOct[k] = (float) std::log2 (std::max (10.0f, get (rf[k], defF[k])) / 1000.0f);
        core.active = true;
    }
};

// exact float <-> text (the readable value and its bits; the bits win when present)
inline juce::String floatBits (float v) { uint32_t u; std::memcpy (&u, &v, 4); return juce::String::toHexString ((juce::int64) u).paddedLeft ('0', 8); }
inline float floatFrom (const juce::String& text, const juce::String& bits)
{
    if (bits.length() == 8) { const uint32_t u = (uint32_t) bits.getHexValue64(); float v; std::memcpy (&v, &u, 4); return v; }
    return text.getFloatValue();
}
inline void putF (juce::XmlElement& x, const char* name, double v) { x.setAttribute (name, juce::String (v, 6)); }

inline std::unique_ptr<juce::XmlElement> modelToXml (const UserModel& m)
{
    auto x = std::make_unique<juce::XmlElement> ("MODEL");
    x->setAttribute ("name", m.name);
    {   auto* a = x->createNewChildElement ("ANCHOR"); const auto& A = m.core.anchor;
        a->setAttribute ("preDelay", floatBits (A.preDelay)); a->setAttribute ("decay", floatBits (A.decay)); a->setAttribute ("size", floatBits (A.size));
        a->setAttribute ("tone", floatBits (A.tone)); a->setAttribute ("early", floatBits (A.early)); a->setAttribute ("buildUp", floatBits (A.buildUp));
        a->setAttribute ("density", floatBits (A.density)); a->setAttribute ("width", floatBits (A.width)); a->setAttribute ("lowCut", floatBits (A.lowCut));
        a->setAttribute ("highCut", floatBits (A.highCut)); }
    {   auto* f = x->createNewChildElement ("FEAT");
        putF (*f, "lt", m.feat.lt); putF (*f, "tilt", m.feat.tilt); putF (*f, "c80", m.feat.c80); putF (*f, "edtr", m.feat.edtr); putF (*f, "onset", m.onset);
        f->setAttribute ("eEr", juce::String (m.core.eEr, 9)); f->setAttribute ("eTail", juce::String (m.core.eTail, 9)); }
    {   auto* p = x->createNewChildElement ("POST"); juce::StringArray a, b;
        for (auto g : m.post.fine) a.add (floatBits (g)); for (auto g : m.post.side) b.add (floatBits (g));
        p->setAttribute ("fine", a.joinIntoString (",")); p->setAttribute ("side", b.joinIntoString (","));
        if (m.post.nTaps > 0)
        {   juce::StringArray t; for (int k = 0; k < m.post.nTaps; ++k) t.add (floatBits (m.post.tapMs[(size_t) k]) + ":" + floatBits (m.post.tapL[(size_t) k]) + ":" + floatBits (m.post.tapR[(size_t) k]));
            p->setAttribute ("taps", t.joinIntoString (",")); } }
    {   auto* i = x->createNewChildElement ("INFO"); const auto& I = m.info;
        putF (*i, "score", I.score); putF (*i, "labScore", I.labScore); putF (*i, "t30", I.t30); putF (*i, "edt", I.edt); putF (*i, "c80", I.c80); putF (*i, "env", I.env);
        putF (*i, "specL", I.specL); putF (*i, "specE", I.specE); putF (*i, "iacc", I.iacc); putF (*i, "ned", I.ned); putF (*i, "seconds", I.seconds);
        i->setAttribute ("renders", I.renders); i->setAttribute ("source", I.source); }
    if (m.ir && m.ir->valid()) { auto* r = x->createNewChildElement ("IR"); r->setAttribute ("name", m.ir->name); r->addTextElement (encodeIR (*m.ir)); }
    auto* e = x->createNewChildElement ("ENGINE");
    for (auto& kv : m.eng) { auto* p = e->createNewChildElement ("P"); p->setAttribute ("id", kv.first); p->setAttribute ("v", juce::String (kv.second)); p->setAttribute ("b", floatBits (kv.second)); }
    return x;
}

inline bool modelFromXml (const juce::XmlElement& x, UserModel& m)
{
    if (! x.hasTagName ("MODEL")) return false;
    m = UserModel(); m.name = x.getStringAttribute ("name");
    if (auto* e = x.getChildByName ("ENGINE"))
        for (auto* p : e->getChildIterator()) m.eng.push_back ({ p->getStringAttribute ("id"), floatFrom (p->getStringAttribute ("v"), p->getStringAttribute ("b")) });
    if (m.eng.empty()) return false;
    m.fillCoreFromEng();
    if (auto* a = x.getChildByName ("ANCHOR"))
    {
        auto& A = m.core.anchor; auto g = [a] (const char* n, float d) { return a->hasAttribute (n) ? floatFrom ({}, a->getStringAttribute (n)) : d; };
        A.preDelay = g ("preDelay", 10); A.decay = g ("decay", 2); A.size = g ("size", 100); A.tone = g ("tone", 0); A.early = g ("early", 0); A.buildUp = g ("buildUp", 0);
        A.density = g ("density", 0); A.width = g ("width", 100); A.lowCut = g ("lowCut", 20); A.highCut = g ("highCut", 20000);
    }
    if (auto* f = x.getChildByName ("FEAT"))
    {
        m.feat.lt = f->getDoubleAttribute ("lt"); m.feat.tilt = f->getDoubleAttribute ("tilt"); m.feat.c80 = f->getDoubleAttribute ("c80"); m.feat.edtr = f->getDoubleAttribute ("edtr");
        m.onset = (float) f->getDoubleAttribute ("onset"); m.core.eEr = f->getDoubleAttribute ("eEr"); m.core.eTail = f->getDoubleAttribute ("eTail");
    }
    if (auto* p = x.getChildByName ("POST"))
    {
        juce::StringArray a, b; a.addTokens (p->getStringAttribute ("fine"), ",", ""); b.addTokens (p->getStringAttribute ("side"), ",", "");
        for (int k = 0; k < kFineBands && k < a.size(); ++k) m.post.fine[(size_t) k] = floatFrom ({}, a[k]);
        for (int k = 0; k < kSideBands && k < b.size(); ++k) m.post.side[(size_t) k] = floatFrom ({}, b[k]);
        juce::StringArray t; t.addTokens (p->getStringAttribute ("taps"), ",", ""); t.removeEmptyStrings();
        m.post.nTaps = 0;
        for (auto& e : t) { juce::StringArray q; q.addTokens (e, ":", ""); if (q.size() != 3 || m.post.nTaps >= kMaxTaps) continue;
                            const auto k = (size_t) m.post.nTaps++; m.post.tapMs[k] = floatFrom ({}, q[0]); m.post.tapL[k] = floatFrom ({}, q[1]); m.post.tapR[k] = floatFrom ({}, q[2]); }
    }
    if (auto* r = x.getChildByName ("IR"))
    {   auto d = std::make_shared<IRData>(); if (decodeIR (r->getAllSubText().trim(), *d)) { d->name = r->getStringAttribute ("name"); m.ir = std::move (d); } }
    if (auto* i = x.getChildByName ("INFO"))
    {
        auto& I = m.info;
        I.score = i->getDoubleAttribute ("score", -1); I.labScore = i->getDoubleAttribute ("labScore", -1); I.t30 = i->getDoubleAttribute ("t30"); I.edt = i->getDoubleAttribute ("edt");
        I.c80 = i->getDoubleAttribute ("c80"); I.env = i->getDoubleAttribute ("env"); I.specL = i->getDoubleAttribute ("specL"); I.specE = i->getDoubleAttribute ("specE");
        I.iacc = i->getDoubleAttribute ("iacc"); I.ned = i->getDoubleAttribute ("ned"); I.seconds = i->getDoubleAttribute ("seconds"); I.renders = i->getIntAttribute ("renders");
        I.source = i->getStringAttribute ("source");
    }
    return true;
}
}
