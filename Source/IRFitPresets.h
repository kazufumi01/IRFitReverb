#pragma once
// IR Fit Reverb (0.2.0): presets in folders.
//  Factory: the library models, one folder per character (Hall / Room / Chamber / Plate / Ambient; vintage first, then modern).
//  User:    <app data>/JPK/IR Fit Reverb/Presets/<folder>/<name>.xml  (macOS: ~/Library/Application Support/JPK/IR Fit Reverb/Presets,
//           Windows: %APPDATA%\JPK\IR Fit Reverb\Presets, Linux: ~/.config/JPK/IR Fit Reverb/Presets; IRFIT_PRESET_DIR overrides).
//           IR conversions go to "Converted". A preset file holds the knobs and, for a model, the whole model.
// The arrows step through the current folder and go on to the next / previous folder at its ends (around at the very end).
#include <juce_core/juce_core.h>
#include "IRFitMap.h"

namespace irfit
{
struct PresetRef
{
    bool factory = false; int entry = -1;          // factory: the library entry
    juce::String folder, name; juce::File file;    // user: the file
    bool valid() const { return factory ? entry >= 0 : file != juce::File(); }
    bool operator== (const PresetRef& o) const { return factory == o.factory && (factory ? entry == o.entry : file == o.file); }
};

class PresetBank
{
public:
    struct Folder { juce::String name; bool factory = false; std::vector<PresetRef> items; };
    std::vector<Folder> folders;
    static constexpr const char* kExt = ".xml";
    static constexpr const char* kConverted = "Converted";

    static juce::File root()
    {
        if (auto* env = std::getenv ("IRFIT_PRESET_DIR")) return juce::File (juce::String::fromUTF8 (env));
       #if JUCE_MAC
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/JPK/IR Fit Reverb/Presets");
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("JPK").getChildFile ("IR Fit Reverb").getChildFile ("Presets");
       #endif
    }
    static juce::String factoryFolderName (int type) { return juce::String ("Factory ") + kTypeName[type]; }
    static juce::String legalName (const juce::String& s) { return juce::File::createLegalFileName (s.trim()).substring (0, 80); }

    void scan()
    {
        folders.clear();
        for (int t = 0; t < 5; ++t)
        {
            Folder f; f.name = factoryFolderName (t); f.factory = true;
            for (int era = 0; era < 2; ++era)
                for (int e = 0; e < kNumEntries; ++e)
                    if (kEntries[e].type == t && kEntries[e].era == era && ! kEntries[e].shortIr)
                    { PresetRef r; r.factory = true; r.entry = e; r.folder = f.name; r.name = juce::String::fromUTF8 (kEntries[e].name); f.items.push_back (r); }
            folders.push_back (std::move (f));
        }
        const auto dir = root(); dir.createDirectory();
        auto addFolder = [this] (const juce::File& d, const juce::String& nm)
        {
            Folder f; f.name = nm;
            auto files = d.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExt);
            files.sort();
            for (auto& x : files) { PresetRef r; r.folder = nm; r.name = x.getFileNameWithoutExtension(); r.file = x; f.items.push_back (r); }
            folders.push_back (std::move (f));
        };
        auto subs = dir.findChildFiles (juce::File::findDirectories, false); subs.sort();
        // "Converted" first, then the others; presets saved straight in the root show as "User"
        for (auto& d : subs) if (d.getFileName() == kConverted) addFolder (d, d.getFileName());
        for (auto& d : subs) if (d.getFileName() != kConverted && ! d.getFileName().startsWith (".")) addFolder (d, d.getFileName());
        if (! dir.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExt).isEmpty()) addFolder (dir, "User");
    }
    juce::StringArray userFolders() const
    {
        juce::StringArray a; for (auto& f : folders) if (! f.factory && f.name != "User") a.add (f.name);
        if (! a.contains (kConverted)) a.insert (0, kConverted);
        return a;
    }
    juce::File folderDir (const juce::String& name) const { return name == "User" ? root() : root().getChildFile (legalName (name)); }
    bool find (const PresetRef& r, int& fi, int& ii) const
    {
        for (size_t f = 0; f < folders.size(); ++f) for (size_t i = 0; i < folders[f].items.size(); ++i) if (folders[f].items[i] == r) { fi = (int) f; ii = (int) i; return true; }
        return false;
    }
    PresetRef step (const PresetRef& cur, int delta) const
    {
        int fi = 0, ii = 0;
        auto firstNonEmpty = [this] (int from, int dir) { const int n = (int) folders.size(); for (int k = 0; k < n; ++k) { const int f = ((from + dir * k) % n + n) % n; if (! folders[(size_t) f].items.empty()) return f; } return -1; };
        if (! find (cur, fi, ii))
        {   // not in the list (unsaved, or gone): the start / end of the current folder if known, else of the bank
            int f = -1; for (size_t k = 0; k < folders.size(); ++k) if (folders[k].name == cur.folder && ! folders[k].items.empty()) f = (int) k;
            if (f < 0) f = firstNonEmpty (delta > 0 ? 0 : (int) folders.size() - 1, delta > 0 ? 1 : -1);
            if (f < 0) return {};
            return delta > 0 ? folders[(size_t) f].items.front() : folders[(size_t) f].items.back();
        }
        const int n = (int) folders[(size_t) fi].items.size(), j = ii + delta;
        if (j >= 0 && j < n) return folders[(size_t) fi].items[(size_t) j];
        const int f = firstNonEmpty (fi + (delta > 0 ? 1 : -1), delta > 0 ? 1 : -1);
        if (f < 0) return cur;
        return delta > 0 ? folders[(size_t) f].items.front() : folders[(size_t) f].items.back();
    }
    // a free file name in a folder ("name", "name 2", ...)
    juce::File freeFile (const juce::String& folder, const juce::String& name) const
    {
        const auto d = folderDir (folder); auto base = legalName (name); if (base.isEmpty()) base = "Preset";
        auto f = d.getChildFile (base + kExt);
        for (int k = 2; f.exists() && k < 1000; ++k) f = d.getChildFile (base + " " + juce::String (k) + kExt);
        return f;
    }
};
}
