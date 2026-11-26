#pragma once
// IR Fit Reverb (product): a few classic controls over the IR-fitted algorithmic models.
// The engine is Reverb Anatomy Lab's ERProcessor (ER taps + 16-line FDN), owned and hidden: the host sees only the macros.
// 0.2.0: IR -> ALGO conversion (a model played directly, the knobs relative to it), presets in folders with arrows,
//        the product's own post stage (band width, fine EQ) and dry / wet after it.
#include "PluginProcessor.h"
#include "IRFitMap.h"
#include "IRFitModel.h"
#include "IRFitPresets.h"
#include "IRFitConvert.h"
#include "IRFitHybrid.h"
#include "IRFitBatch.h"

class IRFitProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    IRFitProcessor();
    ~IRFitProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    static const char* const kMacroIds[];
    static constexpr int kNumMacros = 13;

    irfit::Macros macros() const;                       // the current knob values
    void setMacros (const irfit::Macros& m);            // message thread (each parameter as one gesture)
    void loadModel (int entry);                         // a library model's knob positions (character / era included); library mode
    ERProcessor& engine() { return *eng; }
    const ERProcessor& engine() const { return *eng; }
    // apply the mapping to the engine now (tests / offline: the audio thread does this once per block by itself)
    void pushNow (bool instant = false);
    std::atomic<int> pushes { 0 };                      // how often the engine was updated (tests)
    std::atomic<bool> nonFinite { false };              // set if the output ever held a NaN / Inf (it is muted then)

    // ---- refine (background): once the knobs rest, the reverb is rendered and measured and the gap to the knobs' targets
    // goes back into the mapping as a trim (3-4 renders, no IR needed). At a model's anchor there is nothing to refine.
    irfit::Trim trim() const { return { tLt.load(), tTilt.load(), tC80.load(), tEdtr.load() }; }
    void setTrim (const irfit::Trim& t) { tLt = t.lt; tTilt = t.tilt; tC80 = t.c80; tEdtr = t.edtr; ++trimVersion; }
    irfit::Trim refine (const irfit::Macros& m, irfit::Trim start, int iters, double* errOut = nullptr, std::atomic<bool>* cancel = nullptr,
                        const irfit::ModelCore* mc = nullptr, const irfit::Feat* modelFeat = nullptr);
    std::atomic<int> refines { 0 };
    static double featErr (const irfit::Feat& target, const irfit::Feat& got);
    static bool measureFeat (ERProcessor& e, const irfit::Result* withMap, irfit::Feat& out, double* energyDb = nullptr);

    // ---- (0.2.0) a model played directly (message thread)
    bool modelActive() const { return modelOn; }
    const irfit::UserModel& currentModel() const { return model; }
    void activateModel (const irfit::UserModel& m, bool toAnchor);       // toAnchor: the knobs go to the model's own position
    void activateLibrary();                                               // back to the factory library (the engine's shared settings)
    irfit::Feat targetFor (const irfit::Macros& m) const;                // the features the knobs ask for (relative to the model in model mode)
    static bool sameShape (const irfit::Macros& a, const irfit::Macros& b);   // equal apart from Mix / Character / Era

    // ---- (0.2.0) presets (message thread)
    irfit::PresetBank bank;
    irfit::PresetRef current;                           // the preset loaded / saved last (invalid = none)
    void rescanPresets() { bank.scan(); }
    bool loadPreset (const irfit::PresetRef& r);
    bool stepPreset (int delta);
    juce::File savePresetAs (const juce::String& folder, const juce::String& name, bool overwrite = false);
    bool saveCurrentPreset();                           // over the current user preset (false: none / factory)
    bool deletePreset (const irfit::PresetRef& r);
    bool newFolder (const juce::String& name);
    bool presetDirty() const;
    juce::String presetLabel() const;
    std::unique_ptr<juce::XmlElement> stateXml (bool forPreset) const;
    bool applyStateXml (const juce::XmlElement& x, bool isPreset);

    // ---- (0.2.0) IR -> ALGO (message thread); the result is saved to "Converted" and played
    juce::String convertIR (const juce::File& f);
    void cancelConversion() { if (conv) conv->cancel(); }
    irfit::Converter* converter() { return conv.get(); }
    std::atomic<int> convFinished { 0 };                 // bumps when a conversion ended (done / failed / cancelled)
    juce::String lastConvReport;
    irfit::Converter::Options convOptions;               // (tests: drivePoll etc.)
    void pollForTests() { timerCallback(); }

    // ---- (0.3.1) a folder of IRs -> presets (side by side, skips what is already there), and Reverb Anatomy Lab presets
    juce::String startBatch (const juce::File& folder, const irfit::Batch::Settings& s);
    void cancelBatch() { batch.cancel(); }
    const irfit::Batch& batchState() const { return batch; }
    juce::String lastBatchReport;
    // a finished model as an IR Fit preset (its knobs at the model's anchor), without playing it
    juce::File writeModelPreset (const irfit::UserModel& m, const juce::String& folder, const juce::String& name);
    // the sound as it plays now (engine + post stage; not the hybrid's IR parts) -> <Lab>/User Presets/IR Fit/<name>.rvpreset
    juce::File exportToLab (const juce::String& name);

    // ---- (0.2.0) Advanced: the post stage on / off, the fine EQ amount, the width per band (x the model's own)
    struct Adv { bool on = true; float fineAmt = 100; std::array<float, irfit::kSideBands> width { 100, 100, 100, 100, 100 }; };
    Adv adv() const { return advS; }
    void setAdv (const Adv& a) { advS = a; publishCore(); }
    irfit::PostSettings effectivePost() const;
    // ---- (0.3.0) the hybrid: the ER and the tail each ALGO / IR / a blend (message thread)
    static const char* const kHybIds[];
    static constexpr int kNumHyb = 15;
    enum HybParam { hErBlend = 0, hTailBlend, hSplit, hStart, hAttack, hLength, hSize, hDecL, hDecM, hDecH, hReverse, hWidth, hErLvl, hTailLvl, hLink };
    juce::String loadSlotIR (bool er, const juce::File& f);           // an IR file into the ER (A) / tail (B) slot, as it is (no conversion)
    void useModelIR (bool er);                                         // the slot back to the model's own IR
    void copySlot (bool fromEr, bool toEr) { (toEr ? slotEr : slotTl) = sourceIR (fromEr); updateHybrid(); }   // the other part's IR
    juce::String slotName (bool er) const;                             // what the part plays as IR ("" = none)
    bool slotCustom (bool er) const { return (er ? slotEr : slotTl) != nullptr; }
    bool hybridAvailable (bool er) const { return sourceIR (er) != nullptr; }
    std::atomic<int> hybBuilds { 0 };                                  // IRs rendered for the convolutions (tests)
    bool hybridIdle() const { return ! hybBusy.load() && ! pendReady.load() && hybUpToDate; }
    void pollHybridForTests() { updateHybrid (true); }

    // the product's wet IR at 48 kHz (engine + post), the current settings (tests)
    void renderWetIR (juce::AudioBuffer<float>& out, double maxLenS = 8.0);
    static juce::String tableHash();

private:
    void timerCallback() override;
    void push();
    void publishCore();
    void writeEngineValues (const std::vector<std::pair<juce::RangedAudioParameter*, float>>& v);     // (parameter, plain value)
    // a value written so that the parameter's raw value ends as convertFrom0to1 (convertTo0to1 (v)) whatever it was before
    // (a comparison of the normalised values would leave an ulp of history in it: switching would not be bit-exact)
    static void writeExact (juce::RangedAudioParameter* p, std::atomic<float>* raw, float v)
    {
        const float n = p->convertTo0to1 (v), t = p->convertFrom0to1 (n);
        if (raw == nullptr ? n != p->getValue() : raw->load() != t) { p->setValue (n); p->sendValueChangedMessageToListeners (n); }
    }
    std::vector<std::atomic<float>*> dstRaw; std::unordered_map<juce::RangedAudioParameter*, std::atomic<float>*> rawOf;
    std::atomic<double> tLt { 0 }, tTilt { 0 }, tC80 { 0 }, tEdtr { 0 }; std::atomic<int> trimVersion { 0 }; int trimSeen = -1;
    std::thread refWorker; std::atomic<bool> refCancel { false }, refBusy { false };
    std::mutex refMutex; irfit::Macros refDone; bool refDoneValid = false;      // (the knobs the trim was made for)
    irfit::Macros lastSeen; int stableTicks = 0, ticks = 0;
    void joinRefine() { refCancel = true; if (refWorker.joinable()) refWorker.join(); refCancel = false; }
    std::unique_ptr<ERProcessor> eng;
    std::array<std::atomic<float>*, kNumMacros> raw {};
    std::array<float, kNumMacros> seen {};
    std::vector<juce::RangedAudioParameter*> dst;       // the engine parameters, in the order map() writes them
    std::vector<float> sent; int mixIdx = -1;
    bool dirty = true, held = false; int since = 1 << 20, sinceSpread = 1 << 20; double sr = 48000;
    int sinceDelayKnob = 1 << 30;                                    // (0.2.1) samples since Size / Pre-Delay moved
    std::vector<bool> isSpread;                          // tSpread / tShape / tSpreadHi: the build-up FIR cross-fades its taps over 20 ms on
                                                         // every change, and a change while one runs restarts it: those go at most every 60 ms
    irfit::Result res;                                   // (audio thread only)
    static constexpr float pushMs = 20.0f;               // (0.2.0: was 10 ms) a mapping push at most every 20 ms while knobs move:
                                                         // the FDN set-up behind each push is the CPU; the engine glides between them
    // model mode: the message thread publishes, the audio thread takes it with a try-lock (never waits)
    irfit::UserModel model; bool modelOn = false; Adv advS;
    juce::SpinLock coreLock; irfit::ModelCore pendCore; irfit::PostSettings pendPost; std::atomic<int> coreVersion { 0 }; int coreSeen = -1;
    irfit::ModelCore audioCore; irfit::Post post; bool postInstant = true;
    std::vector<std::pair<juce::RangedAudioParameter*, float>> baseline;     // the engine as the library needs it (kBase; plain values)
    std::vector<bool> skipParam;
    // dry / wet (the engine runs at 100 % wet; the post stage, then the same mix law as the engine's)
    juce::AudioBuffer<float> dryBuf; float dryG = 1, wetG = 0; int maxBlock = 512;
    // (0.2.0) a model / preset switch: the wet fades out (10 ms), the engine is swapped while silent (its tail restarts clean),
    // stays silent 40 ms while its own transitions finish, then fades in (30 ms). The message thread waits for the silence
    // (at most ~60 ms; not at all when no audio runs).
    std::atomic<bool> fadeReq { false }, wetMuted { false }; std::atomic<double> lastBlockMs { -1e9 }; float swG = 1; int swHold = 0;
    struct SwitchScope { IRFitProcessor& p; bool own; explicit SwitchScope (IRFitProcessor& q); ~SwitchScope(); };
    int switchDepth = 0;
    void processChunk (float* L, float* R, int n, juce::MidiBuffer& midi);
    // presets: what the current preset holds (for the "modified" mark)
    irfit::Macros presetMacros; juce::String presetModelName; bool presetModelOn = false;
    void markClean() { presetMacros = macros(); presetModelOn = modelOn; presetModelName = modelOn ? model.name : juce::String(); }
    std::unique_ptr<irfit::Converter> conv; int convStateSeen = 0;
    irfit::Batch batch; int batchSeen = 0;
    // (0.3.0) the hybrid
    irfit::IRPtr sourceIR (bool er) const { const auto& s = er ? slotEr : slotTl; return s ? s : (modelOn ? model.ir : nullptr); }
    irfit::BuildSpec specFor (bool er) const;
    void updateHybrid (bool force = false);
    void joinHybrid() { if (hybWorker.joinable()) hybWorker.join(); }
    irfit::IRPtr slotEr, slotTl;
    irfit::HybridDsp hyb;
    std::array<std::atomic<float>*, kNumHyb> hRaw {};
    std::thread hybWorker; std::atomic<bool> hybBusy { false };
    std::unique_ptr<irfit::FilterSet> pendSet; std::atomic<bool> pendReady { false }; float pendCut = 0;      // worker -> audio thread
    std::array<irfit::PartPtr, 2> curFilt, doneFilt; double hybSr = 48000;                                         // (message thread / worker result)
    std::array<irfit::IRPtr, 2> builtSrc, wantSrc; std::array<irfit::BuildSpec, 2> builtSpec, lastSpec; std::array<int, 2> wantId { 0, 0 }; std::array<std::atomic<int>, 2> wantIdA {};
    std::array<int, 2> loadedId { -1, -1 }; float loadedCut = 0; int ticksSinceBuild = 100; bool hybUpToDate = true;
    std::atomic<float> hRefPre { 0 }, hRefEarly { 0 }, hRefWidth { 100 };     // what the linked knobs are relative to
    bool hybResetReq = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IRFitProcessor)
};
