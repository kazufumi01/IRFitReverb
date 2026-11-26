#pragma once
#include "IRFitProcessor.h"
#include "Theme.h"

// IR Fit Reverb: the product's own simple screen. Looks from Theme.h.
//  header: title | preset bar  [<] [folder / name] [>] [Save] [...] | [Load IR...]   (an IR can also be dropped anywhere)
//  row 2:  Character, Era | model / conversion status | [Advanced]
//  view:   the measured decay, or the Advanced panel (post stage), with the conversion's progress over it
//  knobs:  two rows
class IRFitEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit IRFitEditor (IRFitProcessor&);
    ~IRFitEditor() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override { dragOver = true; fileDragMove ({}, x, y); repaint(); }
    void fileDragMove (const juce::StringArray&, int x, int y) override { const int p = hybPanel.partAt (juce::Point<int> (x, y) - hybPanel.getPosition()); if (p != dragPart) { dragPart = p; repaint(); } }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

    struct KnobLnF : public theme::LookAndFeel
    {
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
        juce::Label* createSliderTextBox (juce::Slider&) override;
    };
    // the reverb's measured decay (the engine re-measures in the background once the settings settle)
    class TailView : public juce::Component
    {
    public:
        void paint (juce::Graphics&) override;
        void setCurve (std::vector<float> envDb, std::vector<float> edcDb, double dtMs, const rv::Measure& m);
        juce::String note;
    private:
        std::vector<float> env, edc; double dt = 10; rv::Measure meas; bool has = false;
    };
    // (0.2.0) Advanced: the post stage (band width, fine EQ) and the model's fit report
    class AdvPanel : public juce::Component
    {
    public:
        explicit AdvPanel (IRFitProcessor& p);
        void paint (juce::Graphics&) override;
        void resized() override;
        void refresh();                      // from the processor (timer)
    private:
        void send();
        IRFitProcessor& proc;
        juce::ToggleButton onBtn { "Post stage" };
        juce::Slider fine; juce::Label fineL;
        std::array<juce::Slider, irfit::kSideBands> width; std::array<juce::Label, irfit::kSideBands> widthL;
        juce::String info; irfit::PostSettings shown; bool updating = false;
    };

    // (0.3.0) the hybrid: the ER and the tail each ALGO / MIX / IR, their IR slots, the IR edits
    class HybPanel : public juce::Component
    {
    public:
        explicit HybPanel (IRFitProcessor& p);
        void paint (juce::Graphics&) override;
        void resized() override;
        void refresh();                      // (timer)
        int partAt (juce::Point<int> p) const;   // 0 = ER, 1 = tail, -1 = neither (a drop)
        std::function<void (const juce::String&)> onStatus;
    private:
        void slotMenu (int part);
        IRFitProcessor& proc;
        struct K { juce::Slider s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
        std::array<K, 2> blend; std::array<K, 11> edit;
        std::array<std::array<juce::TextButton, 3>, 2> mode;     // ALGO / MIX / IR
        std::array<juce::TextButton, 2> slotBtn; std::array<juce::String, 2> slotText;
        juce::ToggleButton reverse { "Reverse tail" }, link { "Follow knobs" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> revA, linkA;
        std::unique_ptr<juce::FileChooser> chooser;
        bool avail[2] { false, false };
    };

    // (0.2.0) the conversion's progress, over the view (Cancel sits on it)
    class ConvOverlay : public juce::Component
    {
    public:
        explicit ConvOverlay (IRFitProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override;
    private:
        IRFitProcessor& proc;
    };

private:
    void timerCallback() override;
    void showPresetMenu();
    void showMoreMenu();
    void saveAsDialog();
    void chooseIR();
    void showLoadMenu();
    void chooseBatchFolder();
    void batchDialog (const juce::File& dir);
    void startConversion (const juce::File& f);
    void updateStatus();
    IRFitProcessor& proc;
    KnobLnF lnf;
    struct Knob { juce::Slider s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
    std::array<Knob, 11> knobs;
    juce::ComboBox character, era;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> chA, erA;
    juce::TextButton prevBtn { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextBtn { juce::String::fromUTF8 ("\xe2\x96\xb6") }, nameBtn, saveBtn { "Save" }, moreBtn { juce::String::fromUTF8 ("\xe2\x80\xa6") };
    juce::TextButton loadBtn { "Load IR..." }, advBtn { "Advanced" }, cancelBtn { "Cancel" };
    juce::Label status;
    TailView tail; AdvPanel advPanel; HybPanel hybPanel { proc }; ConvOverlay overlay { proc }; int dragPart = -1;
    juce::TooltipWindow tips { this, 700 };
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> dialog;
    int batchSeen = 0;
    int measSeen = -1, convSeen = -1; bool dragOver = false, converting = false; double reportUntil = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IRFitEditor)
};
