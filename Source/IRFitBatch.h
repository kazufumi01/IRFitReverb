#pragma once
// IR Fit Reverb (0.3.1): many IRs -> presets in one go, and presets for Reverb Anatomy Lab.
//  Batch:  a folder (its sub-folders too) -> one IR Fit preset per IR in Presets/<folder name>/, several conversions side by side
//          (each on its own hidden engine). An IR whose preset is already there is skipped, so a stopped batch goes on where it was.
//          An *_L / *_R pair is one IR (its _L file). Fast = the Lab fit's first start only on a shorter sequence.
//  Lab:    <Reverb Anatomy Lab>/User Presets/IR Fit/<folder>/<name>.rvpreset : every engine parameter (the same engine) and the post
//          stage as <IRFITPOST> (fine EQ, side width, early taps at the model's anchor: the Lab moves the taps with its Pre-Delay / Size / ER level).
//          The Mix and the output are left out (the Lab keeps its own). The hybrid's IR parts do not go over (the Lab plays the ALGO).
#include "IRFitConvert.h"
#include "IRFitPresets.h"

namespace irfit
{
// ---- Reverb Anatomy Lab presets
inline juce::File labPresetRoot()
{
    if (auto* env = std::getenv ("IRFIT_LAB_DIR")) return juce::File (juce::String::fromUTF8 (env));
    return ERProcessor::presetRoot().getChildFile ("User Presets");
}
inline juce::File labIrFitDir (const juce::String& folder)
{
    auto d = labPresetRoot().getChildFile ("IR Fit");
    return folder.isEmpty() ? d : d.getChildFile (PresetBank::legalName (folder));
}
// the Lab reads the same numbers: decimal, 9 digits (a float survives the round trip)
inline juce::String labJoin (const float* v, int n) { juce::StringArray a; for (int i = 0; i < n; ++i) a.add (juce::String (v[i], 9)); return a.joinIntoString (","); }

inline std::unique_ptr<juce::XmlElement> labPresetXml (const juce::String& name, const std::vector<std::pair<juce::String, float>>& eng,
                                                       const PostSettings& post, float anchorPre, float anchorSize, float anchorLevel, const juce::String& note)
{
    auto x = std::make_unique<juce::XmlElement> ("RVPRESET");
    x->setAttribute ("name", name); x->setAttribute ("version", JucePlugin_VersionString); x->setAttribute ("from", "IR Fit Reverb");
    if (note.isNotEmpty()) x->setAttribute ("note", note);
    static const juce::StringArray skip { "mix", "out", "outByp", "outWet", "autoLvl" };
    for (auto& [id, v] : eng)
    {
        if (skip.contains (id) || ! std::isfinite (v)) continue;
        auto* e = x->createNewChildElement ("P"); e->setAttribute ("id", id); e->setAttribute ("v", juce::String (v, 9));
    }
    if (! post.isNeutral())
    {
        auto* p = x->createNewChildElement ("IRFITPOST");
        p->setAttribute ("fine", labJoin (post.fine.data(), kFineBands)); p->setAttribute ("side", labJoin (post.side.data(), kSideBands));
        juce::StringArray t;
        for (int k = 0; k < post.nTaps; ++k)
            t.add (juce::String (post.tapMs[(size_t) k], 9) + ":" + juce::String (post.tapL[(size_t) k], 9) + ":" + juce::String (post.tapR[(size_t) k], 9));
        p->setAttribute ("taps", t.joinIntoString (","));
        p->setAttribute ("pre", juce::String (anchorPre, 9)); p->setAttribute ("size", juce::String (anchorSize, 9)); p->setAttribute ("level", juce::String (anchorLevel, 9));
    }
    return x;
}
inline float engValue (const std::vector<std::pair<juce::String, float>>& eng, const char* id, float def)
{
    for (auto& kv : eng) if (kv.first == id) return kv.second;
    return def;
}
// a model as it was fitted (at its anchor, the post stage at its full amounts)
inline juce::File writeLabPreset (const UserModel& m, const juce::String& folder, const juce::String& name, bool overwrite = true)
{
    const auto dir = labIrFitDir (folder); dir.createDirectory();
    auto f = dir.getChildFile (PresetBank::legalName (name) + ".rvpreset");
    if (! overwrite) for (int k = 2; f.existsAsFile(); ++k) f = dir.getChildFile (PresetBank::legalName (name) + " " + juce::String (k) + ".rvpreset");
    const auto note = juce::String ("IR Fit Reverb: ") + m.name + " (score " + juce::String (m.info.score, 1) + ")";
    auto x = labPresetXml (name, m.eng, m.post, engValue (m.eng, "predelay", 0), engValue (m.eng, "size", 100), engValue (m.eng, "level", 0), note);
    return x->writeTo (f) ? f : juce::File();
}

// ---- the batch
class Batch
{
public:
    enum JobState { jQueued = 0, jRunning, jDone, jSkipped, jFailed, jCancelled };
    struct Job
    {
        juce::File src; juce::String name, folder;      // the IR, the preset name, the preset folder
        int state = jQueued; double score = 0, seconds = 0; juce::String note;
        std::unique_ptr<Converter> conv;
    };
    struct Settings
    {
        bool fast = true;                // Fast (1 start) or Standard (3 starts, as one conversion)
        int parallel = 0;                // conversions side by side (0: from the cores, 1..4)
        bool toLab = true;               // also a Reverb Anatomy Lab preset per IR
        bool skipDone = true;            // an IR whose preset is already there is not converted again
    };
    // writes one finished model as a preset of the plug-in (the processor's job: its knobs' layout); false = not saved
    std::function<juce::File (const UserModel&, const juce::String& folder, const juce::String& name)> savePreset;
    Converter::Options baseOptions;

    static bool isIRFile (const juce::File& f)
    {
        return f.hasFileExtension ("wav;aif;aiff;flac;caf;ogg") && ! f.getFileName().startsWith (".") && ! fit::isRightOfPair (f);
    }
    static int autoParallel()
    {
        const int cores = juce::SystemStats::getNumCpus();           // (logical; a conversion keeps ~3 busy in its Lab stage)
        return juce::jlimit (1, 4, cores / 4);
    }
    // the IRs of a folder (and its sub-folders: "<folder> <sub>")
    juce::String start (const juce::File& dir, const Settings& s)
    {
        if (running()) return "A batch is already running";
        if (! dir.isDirectory()) return "Not a folder";
        jobs.clear(); set = s; stop = false; t0 = juce::Time::getMillisecondCounterHiRes(); lastFolder = dir.getFileName();
        auto files = dir.findChildFiles (juce::File::findFiles, true);
        files.sort();
        for (auto& f : files)
        {
            if (! isIRFile (f)) continue;
            Job j; j.src = f; j.name = fit::pairName (f);
            auto rel = f.getParentDirectory().getRelativePathFrom (dir);
            j.folder = dir.getFileName() + (rel == "." || rel.isEmpty() ? juce::String() : " " + rel.replaceCharacters ("/\\", "  "));
            if (set.skipDone && PresetBank::root().getChildFile (PresetBank::legalName (j.folder)).getChildFile (PresetBank::legalName (j.name) + PresetBank::kExt).existsAsFile())
            {
                j.state = jSkipped; j.note = "already converted";
                if (set.toLab && ! labIrFitDir (j.folder).getChildFile (PresetBank::legalName (j.name) + ".rvpreset").existsAsFile())
                {   // (the preset is there but not its Lab copy: write it from the preset's model)
                    if (auto x = juce::XmlDocument::parse (PresetBank::root().getChildFile (PresetBank::legalName (j.folder)).getChildFile (PresetBank::legalName (j.name) + PresetBank::kExt)))
                        if (auto* mx = x->getChildByName ("MODEL")) { UserModel m; if (modelFromXml (*mx, m)) writeLabPreset (m, j.folder, j.name); }
                }
            }
            jobs.push_back (std::move (j));
        }
        if (jobs.empty()) return "No IR files in this folder";
        active = true;
        tick();
        return {};
    }
    void cancel()
    {
        if (! active) return;
        stop = true;
        for (auto& j : jobs) if (j.state == jRunning && j.conv) j.conv->cancel();
    }
    // message thread, ~10 Hz
    void tick()
    {
        if (! active) return;
        const int par = set.parallel > 0 ? juce::jlimit (1, 4, set.parallel) : autoParallel();
        int run = 0;
        for (auto& j : jobs)
        {
            if (j.state != jRunning || ! j.conv) continue;
            j.conv->tick();
            const int s = j.conv->state.load();
            if (s == Converter::done)
            {
                UserModel m;
                if (j.conv->takeResult (m))
                {
                    j.score = m.info.score; j.seconds = j.conv->elapsedS();
                    const auto f = savePreset ? savePreset (m, j.folder, j.name) : juce::File();
                    if (set.toLab) writeLabPreset (m, j.folder, j.name);
                    j.state = f.existsAsFile() ? jDone : jFailed; if (! f.existsAsFile()) j.note = "could not save the preset";
                }
                else { j.state = jFailed; j.note = "no result"; }
                j.conv.reset();
            }
            else if (s == Converter::failed) { j.state = jFailed; j.note = j.conv->error; j.conv.reset(); }
            else if (s == Converter::cancelled) { j.state = jCancelled; j.conv.reset(); }
            else ++run;
        }
        if (! stop.load())
            for (auto& j : jobs)
            {
                if (run >= par) break;
                if (j.state != jQueued) continue;
                j.conv = std::make_unique<Converter>();
                auto o = baseOptions; o.fast = set.fast;
                const auto err = j.conv->start (j.src, o);
                if (err.isNotEmpty()) { j.state = jFailed; j.note = err; j.conv.reset(); continue; }
                j.state = jRunning; ++run;
            }
        else for (auto& j : jobs) if (j.state == jQueued) j.state = jCancelled;
        if (run == 0) { active = false; secondsTotal = elapsedS(); ++finished; }
    }
    bool running() const { return active; }
    int count (int st) const { int n = 0; for (auto& j : jobs) n += j.state == st ? 1 : 0; return n; }
    int total() const { return (int) jobs.size(); }
    double elapsedS() const { return t0 > 0 ? (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 : 0.0; }
    // 0..1 over the IRs to convert (skipped ones do not count)
    float progress() const
    {
        double p = 0; int n = 0;
        for (auto& j : jobs)
        {
            if (j.state == jSkipped) continue;
            ++n;
            if (j.state == jRunning && j.conv) p += j.conv->progress.load();
            else if (j.state != jQueued) p += 1;
        }
        return n > 0 ? (float) (p / n) : 1.0f;
    }
    double etaS() const
    {
        const double p = progress(), e = elapsedS();
        return p > 0.03 && active ? e / p * (1 - p) : -1;
    }
    juce::String summary() const
    {
        juce::String s;
        s << count (jDone) << " converted";
        if (const int k = count (jSkipped)) s << ", " << k << " skipped";
        if (const int k = count (jFailed)) s << ", " << k << " failed";
        if (const int k = count (jCancelled)) s << ", " << k << " cancelled";
        return s;
    }
    juce::String folderName() const { return lastFolder; }
    const std::vector<Job>& list() const { return jobs; }
    std::atomic<int> finished { 0 };            // bumps when a batch ended
    double secondsTotal = 0;
    Settings settings() const { return set; }

private:
    std::vector<Job> jobs; Settings set; std::atomic<bool> stop { false }; bool active = false; double t0 = 0; juce::String lastFolder;
};
}
