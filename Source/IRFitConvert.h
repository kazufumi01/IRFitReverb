#pragma once
// IR Fit Reverb (0.2.0): IR -> ALGO conversion in the background.
//  1. the Lab's own closed-loop fit (Reverb Anatomy Lab "rebuild precisely": ER analysis -> ALGO, IR tail -> FDN, then
//     Nelder-Mead over the parameter groups, three starts) on a private, hidden engine;
//  2. the study's correction loop v1 in C++ (arrival time, T30 per band, C80, EDT per band, EQ tone, build-up Gauss-Newton),
//     every step measured against the IR itself and kept only when the score improves;
//  3. the product's post stage (band width, fine EQ) fitted on the rendered IR;
//  4. the result as a model (every engine parameter + the post stage + its features and anchor knob positions).
// Message thread: start / tick / cancel / takeResult. The fits run on their own low-priority threads (the audio thread is never touched).
#include "PluginProcessor.h"
#include "IRFitModel.h"

namespace irfit
{
struct StageScore { juce::String stage; double score = 0; fit::Parts parts; double seconds = 0; int renders = 0; };

// the parts in the units people read (T30 / EDT %, C80 / env dB) as one line
juce::String partsText (const fit::Parts& p);

// read any IR (mono / stereo / 4-channel true stereo [LL, LR, RL, RR] / an *_L + *_R pair, any rate) into one stereo file
// (true stereo: the L input's and the R input's responses added, as the Lab does with a pair). "" = ok, else the reason
juce::String prepareStereoIR (const juce::File& in, const juce::File& outWav, juce::String& displayName, int* channelsOut = nullptr, double* lengthSOut = nullptr);

class Converter
{
public:
    struct Options
    {
        bool lab = true;           // run the Lab fit first (off: start from the given values, tests)
        bool polish = true;        // correction loop v1
        bool post = true;          // band width + fine EQ
        bool nedParams = true;     // (0.2.0) diffusion / reflections in the build-up Gauss-Newton (density)
        bool drivePoll = false;    // tests: tick() drives the hidden engine (no message loop)
        int gnIters = 1;
        bool fast = false;         // (0.3.1) Fast: the Lab fit's first start only, on a shorter sequence (batch conversions)
        double fastScale = 0.7;
    };
    enum State { idle = 0, preparing, labFit, polishing, done, failed, cancelled };

    Converter();
    ~Converter();
    juce::String start (const juce::File& ir, const Options& o);                       // message thread
    juce::String startFromValues (const juce::File& ir, const fit::Values& v, const Options& o);   // tests: skip the Lab fit
    void tick();                                     // message thread (~10 Hz)
    void cancel();
    bool running() const { const int s = state.load(); return s == preparing || s == labFit || s == polishing; }
    std::atomic<int> state { idle };
    std::atomic<float> progress { 0 };
    juce::String stageText() const { const juce::ScopedLock l (lock); return text; }
    double elapsedS() const;
    double etaS() const;
    juce::String error;
    juce::String irName;
    bool takeResult (UserModel& out);                // once, after done
    std::vector<StageScore> stages() const { const juce::ScopedLock l (lock); return log; }
    double labSeconds = 0;

private:
    void setText (const juce::String& s) { const juce::ScopedLock l (lock); text = s; }
    void launchPolish (fit::Values v);
    void polishRun (fit::Values v);
    juce::CriticalSection lock; juce::String text; std::vector<StageScore> log;
    std::unique_ptr<ERProcessor> fitEng;
    struct Worker : juce::Thread { std::function<void()> fn; Worker() : juce::Thread ("IRFit convert") {} void run() override { fn(); } };
    std::unique_ptr<Worker> worker; std::atomic<bool> stop { false }, workerDone { false };
    void joinWorker() { if (worker) { worker->stopThread (-1); worker.reset(); } }
    juce::File tmpDir, tmpWav;
    Options opts; double t0 = 0, tLab = 0;
    UserModel result; bool haveResult = false;
};
}
