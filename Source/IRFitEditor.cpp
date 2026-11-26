#include "IRFitEditor.h"

namespace
{
const char* const kKnobIds[] = { "predelay", "decay", "size", "tone", "early", "buildup", "density", "width", "lowcut", "highcut", "mix" };
const char* const kKnobNames[] = { "PRE-DELAY", "DECAY", "SIZE", "TONE", "EARLY", "BUILD-UP", "DENSITY", "WIDTH", "LOW CUT", "HIGH CUT", "MIX" };
const char* const kKnobTips[] = {
    "Time from the input to the reverb's arrival.",
    "Reverb time (T30, mid frequencies). The models follow what real spaces of that length do.",
    "Room dimension: reflection spacing and the tank's line lengths (the decay stays).",
    "Dark <-> bright: high-frequency decay relative to the low, around what is typical for the decay.",
    "Early energy (C80) relative to what is typical for the decay: + = clearer, - = more tail.",
    "Fast, punchy onset <-> slow swell (EDT against T30).",
    "Reflection count and tail diffusion relative to the model.",
    "Stereo width of the reverb.",
    "High-pass on the reverb.",
    "Low-pass on the reverb.",
    "Dry / wet (the dry stays at full level up to 50 %)." };
constexpr int kW = 800, kH = 694;
}

// ---------------------------------------------------------------------------
void IRFitEditor::KnobLnF::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s)
{
    const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float rad = std::min (r.getWidth(), r.getHeight()) * 0.5f, cx = r.getCentreX(), cy = r.getCentreY();
    const float ang = a0 + pos * (a1 - a0);
    juce::Path bgArc; bgArc.addCentredArc (cx, cy, rad - 3, rad - 3, 0.0f, a0, a1, true);
    g.setColour (theme::line); g.strokePath (bgArc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // bipolar knobs light from the centre
    const bool bip = s.getMinimum() < 0 && s.getMaximum() > 0;
    const float from = bip ? a0 + (float) ((0 - s.getMinimum()) / (s.getMaximum() - s.getMinimum())) * (a1 - a0) : a0;
    juce::Path arc; arc.addCentredArc (cx, cy, rad - 3, rad - 3, 0.0f, std::min (from, ang), std::max (from, ang), true);
    g.setColour (theme::accent); g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (theme::panel.brighter (theme::lightPanels ? 0.0f : 0.08f)); g.fillEllipse (cx - rad + 8, cy - rad + 8, 2 * (rad - 8), 2 * (rad - 8));
    g.setColour (theme::line); g.drawEllipse (cx - rad + 8, cy - rad + 8, 2 * (rad - 8), 2 * (rad - 8), 1.0f);
    juce::Path tick; tick.startNewSubPath (0, -(rad - 10)); tick.lineTo (0, -(rad - 20));
    g.setColour (theme::text); g.strokePath (tick, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), juce::AffineTransform::rotation (ang).translated (cx, cy));
}
juce::Label* IRFitEditor::KnobLnF::createSliderTextBox (juce::Slider& s)
{
    auto* l = theme::LookAndFeel::createSliderTextBox (s);
    l->setFont (theme::mono (12.0f)); l->setJustificationType (juce::Justification::centred);
    l->setColour (juce::Label::textColourId, theme::text); l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    return l;
}

// ---------------------------------------------------------------------------
void IRFitEditor::TailView::setCurve (std::vector<float> e, std::vector<float> d, double dtMs, const rv::Measure& m)
{
    env = std::move (e); edc = std::move (d); dt = dtMs; meas = m; has = ! edc.empty(); repaint();
}
void IRFitEditor::TailView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::well); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (theme::line); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    auto plot = r.reduced (36, 14).withTrimmedBottom (14).withTrimmedRight (-20);
    // the time span: the measured T30 x 1.3 (at least 0.5 s), from 0
    const double t30 = meas.valid && meas.t30Wide > 0 ? meas.t30Wide : 2.0;
    const double span = std::max (0.5, std::min ((double) edc.size() * dt / 1000.0, t30 * 1.3));
    auto X = [&] (double s) { return plot.getX() + (float) (s / span) * plot.getWidth(); };
    auto Y = [&] (double db) { return plot.getY() + (float) (juce::jlimit (0.0, 1.0, -db / 70.0)) * plot.getHeight(); };
    g.setFont (theme::mono (10.0f));
    for (int db = 0; db >= -60; db -= 20)
    {
        g.setColour (theme::grid); g.drawHorizontalLine ((int) Y (db), plot.getX(), plot.getRight());
        g.setColour (theme::faint); g.drawText (juce::String (db), (int) r.getX() + 2, (int) Y (db) - 6, 30, 12, juce::Justification::centredRight);
    }
    const double step = span > 4 ? 1.0 : span > 1.6 ? 0.5 : span > 0.7 ? 0.2 : 0.1;
    for (double s = 0; s <= span + 1e-6; s += step)
    {
        g.setColour (theme::grid); g.drawVerticalLine ((int) X (s), plot.getY(), plot.getBottom());
        g.setColour (theme::faint); g.drawText (juce::String (s, step < 0.5 ? 1 : 1) + " s", (int) X (s) - 20, (int) plot.getBottom() + 2, 40, 12, juce::Justification::centred);
    }
    if (! has) { g.setColour (theme::muted); g.setFont (theme::ui (12.0f)); g.drawText ("measuring...", plot, juce::Justification::centred); return; }
    auto curve = [&] (const std::vector<float>& v, juce::Colour c, float th)
    {
        juce::Path p; bool started = false;
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double s = (double) i * dt / 1000.0; if (s > span) break;
            const float px = X (s), py = Y (v[i]);
            if (! started) { p.startNewSubPath (px, py); started = true; } else p.lineTo (px, py);
        }
        g.setColour (c); g.strokePath (p, juce::PathStrokeType (th));
    };
    curve (env, theme::faint.withAlpha (0.8f), 1.0f);
    curve (edc, theme::accent, 2.0f);
    g.setColour (theme::text); g.setFont (theme::mono (11.0f));
    juce::String s; s << "T30 " << juce::String (meas.t30Wide, 2) << " s   EDT " << juce::String (meas.edtWide, 2) << " s   C80 " << juce::String (meas.c80, 1) << " dB";
    g.drawText (s, plot.withHeight (14).translated (0, -2), juce::Justification::topRight);
    g.setColour (theme::muted); g.setFont (theme::ui (10.5f));
    g.drawText (note, plot.withTrimmedTop (16).withHeight (14), juce::Justification::topRight);
}

// ---------------------------------------------------------------------------
//  (0.2.0) Advanced panel
// ---------------------------------------------------------------------------
IRFitEditor::AdvPanel::AdvPanel (IRFitProcessor& p) : proc (p)
{
    onBtn.setTooltip ("The product's post stage after the reverb: the band width and the fine EQ fitted to the IR (off = the engine alone).");
    onBtn.onClick = [this] { if (! updating) send(); };
    addAndMakeVisible (onBtn);
    auto setup = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, const juce::String& tip)
    {
        s.setSliderStyle (juce::Slider::LinearVertical); s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 16);
        s.setRange (lo, hi, 1); s.setTextValueSuffix (" %"); s.setDoubleClickReturnValue (true, 100); s.setTooltip (tip);
        s.onValueChange = [this] { if (! updating) send(); };
        addAndMakeVisible (s);
        l.setText (name, juce::dontSendNotification); l.setJustificationType (juce::Justification::centred);
        l.setFont (theme::ui (10.5f, true)); l.setColour (juce::Label::textColourId, theme::muted); addAndMakeVisible (l);
    };
    setup (fine, fineL, "FINE EQ", 0, 200, "How much of the fitted 1/3-octave EQ (100 % = as fitted; models from the library have none).");
    const char* wn[irfit::kSideBands] = { "W 250", "W 500", "W 1k", "W 2k", "W 4k" };
    for (int k = 0; k < irfit::kSideBands; ++k)
        setup (width[(size_t) k], widthL[(size_t) k], wn[k], 0, 300, "Stereo width in this octave, x the model's own (100 % = as fitted).");
}
void IRFitEditor::AdvPanel::send()
{
    IRFitProcessor::Adv a; a.on = onBtn.getToggleState(); a.fineAmt = (float) fine.getValue();
    for (int k = 0; k < irfit::kSideBands; ++k) a.width[(size_t) k] = (float) width[(size_t) k].getValue();
    proc.setAdv (a); refresh();
}
void IRFitEditor::AdvPanel::refresh()
{
    const auto a = proc.adv();
    updating = true;
    onBtn.setToggleState (a.on, juce::dontSendNotification); fine.setValue (a.fineAmt, juce::dontSendNotification);
    for (int k = 0; k < irfit::kSideBands; ++k) width[(size_t) k].setValue (a.width[(size_t) k], juce::dontSendNotification);
    updating = false;
    juce::String t;
    if (proc.modelActive())
    {
        const auto& I = proc.currentModel().info;
        t << "Model: " << proc.currentModel().name;
        if (I.score >= 0)
            t << "\nFit to " << I.source << ": score " << juce::String (I.score, 1) << " (Lab fit alone " << juce::String (I.labScore, 1) << ", lower = closer)\n"
              << "T30 " << juce::String (I.t30, 1) << " %   EDT " << juce::String (I.edt, 1) << " %   C80 " << juce::String (I.c80, 2) << " dB   envelope " << juce::String (I.env, 2) << " dB\n"
              << "spectrum late / early " << juce::String (I.specL, 2) << " / " << juce::String (I.specE, 2) << "   IACC " << juce::String (I.iacc, 2) << "   density " << juce::String (I.ned, 2)
              << "\n" << juce::String (juce::roundToInt (I.seconds)) << " s, " << I.renders << " renders after the Lab fit";
    }
    else t << "Factory library model (no fitted post stage). Width per band still applies.";
    const auto ps = proc.effectivePost();
    bool ch = t != info; for (int k = 0; k < irfit::kFineBands && ! ch; ++k) ch = ps.fine[(size_t) k] != shown.fine[(size_t) k];
    for (int k = 0; k < irfit::kSideBands && ! ch; ++k) ch = ps.side[(size_t) k] != shown.side[(size_t) k];
    if (ch) { info = t; shown = ps; repaint(); }
}
void IRFitEditor::AdvPanel::resized()
{
    onBtn.setBounds (10, 8, 110, 22);
    const int y = 34, h = getHeight() - y - 4;
    fineL.setBounds (10, y, 60, 14); fine.setBounds (14, y + 14, 52, h - 14);
    for (int k = 0; k < irfit::kSideBands; ++k) { const int x = 84 + k * 50; widthL[(size_t) k].setBounds (x, y, 50, 14); width[(size_t) k].setBounds (x + 2, y + 14, 46, h - 14); }
}
void IRFitEditor::AdvPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::well); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (theme::line); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    // the post stage's response: the fine EQ (both channels) and the side gain per band
    auto plot = juce::Rectangle<float> (350.0f, 10.0f, r.getWidth() - 362.0f, 84.0f);
    g.setColour (theme::panel); g.fillRect (plot);
    auto X = [&] (double f) { return plot.getX() + (float) (std::log (f / 30.0) / std::log (20000.0 / 30.0)) * plot.getWidth(); };
    auto Y = [&] (double db) { return plot.getCentreY() - (float) juce::jlimit (-15.0, 15.0, db) / 15.0f * plot.getHeight() * 0.5f; };
    g.setColour (theme::grid); g.drawHorizontalLine ((int) Y (0), plot.getX(), plot.getRight());
    for (double f : { 100.0, 1000.0, 10000.0 }) g.drawVerticalLine ((int) X (f), plot.getY(), plot.getBottom());
    g.setFont (theme::mono (9.0f)); g.setColour (theme::faint);
    g.drawText ("+15", (int) plot.getX() + 2, (int) plot.getY(), 30, 10, juce::Justification::left); g.drawText ("-15", (int) plot.getX() + 2, (int) plot.getBottom() - 10, 30, 10, juce::Justification::left);
    for (auto [f, t] : { std::pair<double, const char*> { 100, "100" }, { 1000, "1k" }, { 10000, "10k" } }) g.drawText (t, (int) X (f) + 2, (int) plot.getBottom() - 10, 30, 10, juce::Justification::left);
    auto curve = [&] (bool side, juce::Colour c)
    {
        juce::Path p; rv::BQ b;
        for (int i = 0; i <= 120; ++i)
        {
            const double f = 30.0 * std::pow (20000.0 / 30.0, i / 120.0); double db = 0;
            if (side) for (int k = 0; k < irfit::kSideBands; ++k) { irfit::setSide (b, k, shown.side[(size_t) k], 48000); db += irfit::bqDb (b, f, 48000); }
            else for (int k = 0; k < irfit::kFineBands; ++k) { if (shown.fine[(size_t) k] == 0) continue; irfit::setFine (b, k, shown.fine[(size_t) k], 48000); db += irfit::bqDb (b, f, 48000); }
            if (i == 0) p.startNewSubPath (X (f), Y (db)); else p.lineTo (X (f), Y (db));
        }
        g.setColour (c); g.strokePath (p, juce::PathStrokeType (1.6f));
    };
    curve (true, theme::faint); curve (false, theme::accent);
    g.setFont (theme::ui (10.0f)); g.setColour (theme::accent); g.drawText ("fine EQ", (int) plot.getRight() - 120, (int) plot.getY() + 2, 56, 12, juce::Justification::right);
    g.setColour (theme::faint); g.drawText ("side (width)", (int) plot.getRight() - 64, (int) plot.getY() + 2, 62, 12, juce::Justification::right);
    g.setColour (theme::text); g.setFont (theme::ui (11.0f));
    g.drawFittedText (info, juce::Rectangle<int> ((int) plot.getX(), (int) plot.getBottom() + 6, (int) plot.getWidth(), (int) r.getHeight() - (int) plot.getBottom() - 10), juce::Justification::topLeft, 6, 0.9f);
}

// ---------------------------------------------------------------------------
//  (0.3.0) the hybrid panel
// ---------------------------------------------------------------------------
namespace
{
const char* const kEditIds[] = { "hSplit", "irStart", "irAttack", "irLength", "irSize", "irDecL", "irDecM", "irDecH", "irWidth", "irErLvl", "irTailLvl" };
const char* const kEditNames[] = { "SPLIT", "START", "ATTACK", "LENGTH", "SIZE", "DEC LOW", "DEC MID", "DEC HIGH", "WIDTH", "ER LVL", "TAIL LVL" };
const char* const kEditTips[] = {
    "Where the IR's early part ends and its tail begins (after its first arrival); a 10 ms cross-over.",
    "Cuts the IR's start: it begins this much later into the IR (and arrives earlier).",
    "Fades the IR in from its first arrival over this time (a softer onset).",
    "Where the IR ends (a fade over its last 10 %).",
    "The IR's time: the early reflections further apart / closer, the tail as much longer / shorter.",
    "The IR's decay time below 250 Hz (x).",
    "The IR's decay time from 250 Hz to 4 kHz (x).",
    "The IR's decay time above 4 kHz (x).",
    "The IR's stereo width (M / S).",
    "The IR's early part, level.",
    "The IR's tail, level." };
}
IRFitEditor::HybPanel::HybPanel (IRFitProcessor& p) : proc (p)
{
    auto setupK = [this] (K& k, const char* id, const char* name, const char* tip, int box)
    {
        k.s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); k.s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, box, 15);
        k.s.setTooltip (tip); auto* prm = proc.apvts.getParameter (id);
        k.s.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
        addAndMakeVisible (k.s); k.a = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.s);
        k.l.setText (name, juce::dontSendNotification); k.l.setJustificationType (juce::Justification::centred);
        k.l.setFont (theme::ui (9.5f, true)); k.l.setColour (juce::Label::textColourId, theme::muted); addAndMakeVisible (k.l);
    };
    setupK (blend[0], "erBlend", "ER IR MIX", "The early part: ALGO model (0 %) <-> the IR itself (100 %), equal power.", 64);
    setupK (blend[1], "tailBlend", "TAIL IR MIX", "The tail: ALGO model (0 %) <-> the IR itself (100 %), equal power.", 64);
    for (size_t i = 0; i < edit.size(); ++i) setupK (edit[i], kEditIds[i], kEditNames[i], kEditTips[i], 58);
    const char* mn[3] = { "ALGO", "MIX", "IR" }; const float mv[3] = { 0, 50, 100 };
    for (int part = 0; part < 2; ++part)
    {
        for (int m = 0; m < 3; ++m)
        {
            auto& b = mode[(size_t) part][(size_t) m]; b.setButtonText (mn[m]); b.setClickingTogglesState (false);
            b.setTooltip (m == 0 ? "The ALGO model only" : m == 1 ? "Half and half (equal power)" : "The IR only");
            b.onClick = [this, part, v = mv[m]] { if (auto* q = proc.apvts.getParameter (part == 0 ? "erBlend" : "tailBlend")) { q->beginChangeGesture(); q->setValueNotifyingHost (q->convertTo0to1 (v)); q->endChangeGesture(); } };
            b.setConnectedEdges ((m > 0 ? juce::Button::ConnectedOnLeft : 0) | (m < 2 ? juce::Button::ConnectedOnRight : 0));
            addAndMakeVisible (b);
        }
        auto& sb = slotBtn[(size_t) part]; sb.getProperties().set ("comboLook", true);
        sb.setTooltip (part == 0 ? "IR A: the IR the early part plays (the model's own, or load another; you can drop a file here)"
                                 : "IR B: the IR the tail plays (the model's own, or load another; you can drop a file here)");
        sb.onClick = [this, part] { slotMenu (part); };
        addAndMakeVisible (sb);
    }
    reverse.setTooltip ("The IR's tail backwards (after the split)."); link.setTooltip ("The IR follows Pre-Delay, Decay, Size, Tone, Early, Width and the cuts (relative to the model).");
    addAndMakeVisible (reverse); addAndMakeVisible (link);
    revA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "irReverse", reverse);
    linkA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "irLink", link);
    refresh();
}
void IRFitEditor::HybPanel::slotMenu (int part)
{
    juce::PopupMenu m;
    m.addItem (1, "Load an IR file...");
    m.addItem (2, "Use the model's IR", proc.modelActive() && proc.currentModel().ir != nullptr, ! proc.slotCustom (part == 0) && proc.hybridAvailable (part == 0));
    m.addItem (3, part == 0 ? "Same IR as the tail" : "Same IR as the early part", proc.hybridAvailable (part != 0));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slotBtn[(size_t) part]), [this, part] (int r)
    {
        if (r == 1)
        {
            chooser = std::make_unique<juce::FileChooser> (part == 0 ? "IR A (early part)" : "IR B (tail)", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.caf");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, part] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult(); if (! f.existsAsFile()) return;
                const auto err = proc.loadSlotIR (part == 0, f);
                if (onStatus) onStatus (err.isEmpty() ? (part == 0 ? "IR A: " : "IR B: ") + proc.slotName (part == 0) : "Cannot load the IR: " + err);
                refresh();
            });
        }
        else if (r == 2) { proc.useModelIR (part == 0); refresh(); }
        else if (r == 3) { proc.copySlot (part != 0, part == 0); refresh(); }
    });
}
int IRFitEditor::HybPanel::partAt (juce::Point<int> p) const
{
    if (p.y < 0 || p.y > 84 || p.x < 0 || p.x > getWidth()) return -1;
    return p.x < getWidth() / 2 ? 0 : 1;
}
void IRFitEditor::HybPanel::refresh()
{
    bool ch = false;
    for (int part = 0; part < 2; ++part)
    {
        const bool av = proc.hybridAvailable (part == 0);
        const auto nm = av ? proc.slotName (part == 0) : juce::String ("no IR: convert one or load a file");
        if (nm != slotText[(size_t) part]) { slotText[(size_t) part] = nm; slotBtn[(size_t) part].setButtonText ((part == 0 ? "A: " : "B: ") + nm); }
        if (av != avail[part]) { avail[part] = av; ch = true; }
        const float v = proc.apvts.getRawParameterValue (part == 0 ? "erBlend" : "tailBlend")->load();
        for (int m = 0; m < 3; ++m)
        {
            auto& b = mode[(size_t) part][(size_t) m]; const bool on = std::abs (v - (float) (m * 50)) < 0.5f;
            if (b.getToggleState() != on) b.setToggleState (on, juce::dontSendNotification);
            b.setEnabled (av);
        }
        blend[(size_t) part].s.setEnabled (av);
    }
    const bool any = avail[0] || avail[1];
    for (auto& k : edit) k.s.setEnabled (any);
    reverse.setEnabled (avail[1]); link.setEnabled (any);
    if (ch) repaint();
}
void IRFitEditor::HybPanel::resized()
{
    for (int part = 0; part < 2; ++part)
    {
        const int x0 = part * (getWidth() / 2) + 10;
        for (int m = 0; m < 3; ++m) mode[(size_t) part][(size_t) m].setBounds (x0 + 80 + m * 50, 6, 50, 22);
        slotBtn[(size_t) part].setBounds (x0, 34, getWidth() / 2 - 104, 24);
        blend[(size_t) part].l.setBounds (x0 + getWidth() / 2 - 96, 2, 76, 12);
        blend[(size_t) part].s.setBounds (x0 + getWidth() / 2 - 92, 14, 68, 68);
    }
    const int y = 92, cw = 60;
    for (size_t i = 0; i < edit.size(); ++i)
    {
        edit[i].l.setBounds (8 + (int) i * cw, y, cw, 12);
        edit[i].s.setBounds (8 + (int) i * cw + 4, y + 12, cw - 8, 70);
    }
    reverse.setBounds (8 + 11 * cw + 6, y + 14, 110, 22); link.setBounds (8 + 11 * cw + 6, y + 44, 110, 22);
}
void IRFitEditor::HybPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::panel); g.fillRoundedRectangle (r, 5.0f);
    g.setColour (theme::line); g.drawVerticalLine (getWidth() / 2, 6.0f, 80.0f); g.drawHorizontalLine (86, 8.0f, r.getWidth() - 8);
    g.setFont (theme::ui (11.0f, true));
    for (int part = 0; part < 2; ++part)
    {
        g.setColour (avail[part] ? theme::text : theme::faint);
        g.drawText (part == 0 ? "EARLY" : "TAIL", part * (getWidth() / 2) + 10, 6, 70, 22, juce::Justification::centredLeft);
    }
    g.setColour (theme::faint); g.setFont (theme::ui (9.5f));
    g.drawText ("HYBRID  -  IR EDIT", getWidth() - 130, 88, 122, 12, juce::Justification::centredRight);
}

// ---------------------------------------------------------------------------
IRFitEditor::IRFitEditor (IRFitProcessor& p) : juce::AudioProcessorEditor (p), proc (p), advPanel (p)
{
    setLookAndFeel (&lnf);
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        k.s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 16);
        k.s.setTooltip (kKnobTips[i]);
        k.s.setDoubleClickReturnValue (true, proc.apvts.getParameter (kKnobIds[i])->convertFrom0to1 (proc.apvts.getParameter (kKnobIds[i])->getDefaultValue()));
        addAndMakeVisible (k.s);
        k.a = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, kKnobIds[i], k.s);
        k.l.setText (kKnobNames[i], juce::dontSendNotification); k.l.setJustificationType (juce::Justification::centred);
        k.l.setFont (theme::ui (11.0f, true)); k.l.setColour (juce::Label::textColourId, theme::muted);
        addAndMakeVisible (k.l);
    }
    character.addItemList ({ "All", "Hall", "Room", "Chamber", "Plate", "Ambient" }, 1);
    era.addItemList ({ "Any era", "Vintage ('70s-'80s)", "Modern ('00s-'10s)" }, 1);
    character.setTooltip ("The library models the knobs blend (factory presets). Not used while a converted model plays.");
    addAndMakeVisible (character); addAndMakeVisible (era);
    chA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "character", character);
    erA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "era", era);
    // the preset bar
    nameBtn.getProperties().set ("comboLook", true);
    nameBtn.setTooltip ("Presets: factory (by character) and yours (by folder). IR conversions are saved in \"Converted\".");
    nameBtn.onClick = [this] { showPresetMenu(); };
    prevBtn.setTooltip ("Previous preset (at the start of a folder: the end of the one before)");
    nextBtn.setTooltip ("Next preset (at the end of a folder: the next folder)");
    prevBtn.onClick = [this] { proc.stepPreset (-1); updateStatus(); };
    nextBtn.onClick = [this] { proc.stepPreset (+1); updateStatus(); };
    saveBtn.setTooltip ("Save over the current user preset (a factory preset or a new sound: Save As)");
    saveBtn.onClick = [this] { if (proc.current.valid() && ! proc.current.factory) proc.saveCurrentPreset(); else saveAsDialog(); updateStatus(); };
    moreBtn.setTooltip ("Save As, Delete, New Folder, show the preset folder");
    moreBtn.onClick = [this] { showMoreMenu(); };
    loadBtn.setTooltip ("IR -> ALGO: read an impulse response (wav / aif; mono, stereo or 4-channel true stereo) and fit the reverb to it. "
                        "Takes a few minutes in the background; the result plays and is saved in \"Converted\". You can also drop an IR on the window. "
                        "A folder of IRs (or a folder dropped on the window): one preset per IR, several side by side, optionally also as Reverb Anatomy Lab presets.");
    loadBtn.onClick = [this] { showLoadMenu(); };
    advBtn.setClickingTogglesState (true); advBtn.setTooltip ("The post stage (band width, fine EQ) and the model's fit report");
    advBtn.onClick = [this] { tail.setVisible (! advBtn.getToggleState()); advPanel.setVisible (advBtn.getToggleState()); advPanel.refresh(); };
    cancelBtn.onClick = [this] { proc.cancelConversion(); };
    for (auto* b : { &prevBtn, &nextBtn, &nameBtn, &saveBtn, &moreBtn, &loadBtn, &advBtn }) addAndMakeVisible (*b);
    addChildComponent (cancelBtn);
    status.setFont (theme::ui (11.0f)); status.setColour (juce::Label::textColourId, theme::muted); status.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (status);
    addAndMakeVisible (tail); addChildComponent (advPanel); addChildComponent (overlay);
    addAndMakeVisible (hybPanel);
    hybPanel.onStatus = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); reportUntil = juce::Time::getMillisecondCounterHiRes() + 8000; };
    setSize (kW, kH);
    updateStatus();
    startTimerHz (10);
}
IRFitEditor::~IRFitEditor() { stopTimer(); if (dialog) dialog->exitModalState (0); setLookAndFeel (nullptr); }

// ---- presets
void IRFitEditor::showPresetMenu()
{
    proc.rescanPresets();
    juce::PopupMenu m, fac; int id = 1;
    std::vector<irfit::PresetRef> refs; refs.push_back ({});
    for (auto& f : proc.bank.folders)
    {
        juce::PopupMenu sub;
        int lastEra = -1;
        for (auto& r : f.items)
        {
            if (r.factory && irfit::kEntries[r.entry].era != lastEra) { lastEra = irfit::kEntries[r.entry].era; sub.addSectionHeader (irfit::kEraName[lastEra]); }
            sub.addItem (id++, r.name, true, r == proc.current); refs.push_back (r);
        }
        if (f.items.empty()) sub.addItem (-1, "(empty)", false);
        const bool here = proc.current.folder == f.name;
        if (f.factory) fac.addSubMenu (f.name.fromFirstOccurrenceOf ("Factory ", false, false), sub, true, nullptr, here);
        else m.addSubMenu (f.name, sub, true, nullptr, here);
    }
    juce::PopupMenu top; top.addSubMenu ("Factory", fac); top.addSeparator();
    if (m.getNumItems() > 0) { top.addSectionHeader ("User"); for (juce::PopupMenu::MenuItemIterator it (m); it.next();) top.addItem (it.getItem()); }
    top.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameBtn), [this, refs] (int r) { if (r > 0 && r < (int) refs.size()) { proc.loadPreset (refs[(size_t) r]); updateStatus(); } });
}
void IRFitEditor::showMoreMenu()
{
    juce::PopupMenu m;
    const bool user = proc.current.valid() && ! proc.current.factory;
    m.addItem (1, "Save", user); m.addItem (2, "Save As..."); m.addItem (3, "Delete \"" + proc.current.name + "\"...", user); m.addSeparator();
    m.addItem (4, "New Folder..."); m.addItem (5, "Show the Preset Folder"); m.addItem (6, "Rescan");
    m.addSeparator();
    m.addItem (7, "Export to Reverb Anatomy Lab");
    m.addItem (8, "Show the Lab's \"IR Fit\" Presets");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreBtn), [this] (int r)
    {
        if (r == 1) proc.saveCurrentPreset();
        else if (r == 2) saveAsDialog();
        else if (r == 3)
        {
            const auto ref = proc.current;
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Delete preset")
                                              .withMessage ("Move \"" + ref.folder + " / " + ref.name + "\" to the trash?").withButton ("Delete").withButton ("Cancel").withAssociatedComponent (this),
                                          [this, ref] (int res) { if (res == 1) { proc.deletePreset (ref); updateStatus(); } });
        }
        else if (r == 4)
        {
            dialog = std::make_unique<juce::AlertWindow> ("New folder", "A folder for your presets", juce::MessageBoxIconType::NoIcon, this);
            dialog->addTextEditor ("name", "My Presets", "Name");
            dialog->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey)); dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            dialog->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int res)
            {
                const auto nm = dialog ? dialog->getTextEditorContents ("name") : juce::String();
                { juce::Component::SafePointer<IRFitEditor> sp (this); juce::MessageManager::callAsync ([sp] { if (sp) sp->dialog.reset(); }); }   // (MSVC: no `this` in a nested init-capture)
                if (res == 1 && nm.trim().isNotEmpty()) proc.newFolder (nm);
            }), false);
        }
        else if (r == 5) { irfit::PresetBank::root().createDirectory(); irfit::PresetBank::root().revealToUser(); }
        else if (r == 6) proc.rescanPresets();
        else if (r == 7)
        {
            const auto f = proc.exportToLab ({});
            status.setText (f.existsAsFile() ? "Reverb Anatomy Lab preset: User Presets / IR Fit / " + f.getFileNameWithoutExtension()
                                                   + (proc.apvts.getRawParameterValue ("erBlend")->load() + proc.apvts.getRawParameterValue ("tailBlend")->load() < 0.5f ? juce::String() : juce::String ("  (the ALGO part; the IR parts stay here)"))
                                             : juce::String ("Could not write the Lab preset"), juce::dontSendNotification);
            reportUntil = juce::Time::getMillisecondCounterHiRes() + 10000; return;
        }
        else if (r == 8) { const auto d = irfit::labIrFitDir ({}); d.createDirectory(); d.revealToUser(); }
        updateStatus();
    });
}
void IRFitEditor::saveAsDialog()
{
    proc.rescanPresets();
    const auto folders = proc.bank.userFolders();
    dialog = std::make_unique<juce::AlertWindow> ("Save preset", "The knobs and, for a converted IR, the whole model are saved.", juce::MessageBoxIconType::NoIcon, this);
    const auto defName = proc.current.valid() && ! proc.current.factory ? proc.current.name : proc.modelActive() ? proc.currentModel().name : juce::String ("My Reverb");
    dialog->addTextEditor ("name", defName, "Name");
    dialog->addComboBox ("folder", folders, "Folder");
    if (auto* cb = dialog->getComboBoxComponent ("folder"))
    {
        int sel = folders.indexOf (proc.current.factory ? juce::String() : proc.current.folder);
        if (sel < 0) sel = folders.indexOf (proc.modelActive() ? irfit::PresetBank::kConverted : "User");
        cb->setSelectedItemIndex (std::max (0, sel));
    }
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey)); dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int res)
    {
        juce::String nm, fo;
        if (dialog) { nm = dialog->getTextEditorContents ("name"); if (auto* cb = dialog->getComboBoxComponent ("folder")) fo = cb->getText(); }
        { juce::Component::SafePointer<IRFitEditor> sp (this); juce::MessageManager::callAsync ([sp] { if (sp) sp->dialog.reset(); }); }   // (MSVC: no `this` in a nested init-capture)
        if (res == 1 && nm.trim().isNotEmpty())
        {
            const auto f = proc.bank.folderDir (fo).getChildFile (irfit::PresetBank::legalName (nm) + irfit::PresetBank::kExt);
            proc.savePresetAs (fo, nm, f.existsAsFile());          // (the same name in the same folder: replaced)
            updateStatus();
        }
    }), false);
}

// ---- IR -> ALGO
bool IRFitEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;caf") || juce::File (f).isDirectory()) return true;
    return false;
}
void IRFitEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    dragOver = false; repaint();
    const int part = hybPanel.partAt (juce::Point<int> (x, y) - hybPanel.getPosition()); dragPart = -1;
    for (auto& f : files) if (juce::File (f).isDirectory()) { batchDialog (juce::File (f)); return; }      // (0.3.1) a folder: the batch
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;caf"))
    {
        if (part >= 0)
        {   // (0.3.0) on the hybrid's EARLY / TAIL: into that IR slot, as it is
            const auto err = proc.loadSlotIR (part == 0, juce::File (f));
            status.setText (err.isEmpty() ? (part == 0 ? "IR A: " : "IR B: ") + proc.slotName (part == 0) : "Cannot load the IR: " + err, juce::dontSendNotification);
            reportUntil = juce::Time::getMillisecondCounterHiRes() + 8000; hybPanel.refresh();
            return;
        }
        startConversion (juce::File (f)); return;
    }
}
void IRFitEditor::chooseIR()
{
    chooser = std::make_unique<juce::FileChooser> ("IR -> ALGO: choose an impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult(); if (f.existsAsFile()) startConversion (f);
    });
}
void IRFitEditor::showLoadMenu()
{
    juce::PopupMenu m;
    const bool b = proc.batchState().running(), one = proc.converter() != nullptr && proc.converter()->running();
    m.addItem (1, "Convert an IR...", ! b && ! one);
    m.addItem (2, "Convert a folder of IRs (batch)...", ! b && ! one);
    if (b) { m.addSeparator(); m.addItem (3, "Stop the batch (done ones stay; run it again to go on)"); }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loadBtn), [this] (int r)
    {
        if (r == 1) chooseIR();
        else if (r == 2) chooseBatchFolder();
        else if (r == 3) proc.cancelBatch();
    });
}
void IRFitEditor::chooseBatchFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Batch: choose a folder of IRs (its sub-folders too)", juce::File());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        const auto d = fc.getResult(); if (d.isDirectory()) batchDialog (d);
    });
}
void IRFitEditor::batchDialog (const juce::File& dir)
{
    if (proc.batchState().running() || (proc.converter() != nullptr && proc.converter()->running())) { status.setText ("A conversion is already running", juce::dontSendNotification); return; }
    int n = 0; for (auto& f : dir.findChildFiles (juce::File::findFiles, true)) n += irfit::Batch::isIRFile (f) ? 1 : 0;
    if (n == 0) { status.setText ("No IR files in \"" + dir.getFileName() + "\"", juce::dontSendNotification); reportUntil = juce::Time::getMillisecondCounterHiRes() + 8000; return; }
    const int par = irfit::Batch::autoParallel();
    dialog = std::make_unique<juce::AlertWindow> ("Batch: " + dir.getFileName(),
        juce::String (n) + " IRs -> presets in the folder \"" + dir.getFileName() + "\". IRs already converted there are skipped, so a stopped batch goes on where it was. "
        "You can keep playing while it runs.", juce::MessageBoxIconType::NoIcon, this);
    dialog->addComboBox ("mode", { "Fast (one fit start: about half the time)", "Standard (as one conversion)" }, "Mode");
    dialog->addComboBox ("par", { "Auto (" + juce::String (par) + ")", "1", "2", "3", "4" }, "Side by side");
    dialog->addComboBox ("lab", { "Also as Reverb Anatomy Lab presets", "IR Fit presets only" }, "Reverb Anatomy Lab");
    dialog->addComboBox ("again", { "Skip IRs already converted", "Convert all again (replaces them)" }, "Already converted");
    for (auto* id : { "mode", "par", "lab", "again" }) if (auto* cb = dialog->getComboBoxComponent (id)) cb->setSelectedItemIndex (0);
    dialog->addButton ("Start", 1, juce::KeyPress (juce::KeyPress::returnKey)); dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([this, dir] (int res)
    {
        irfit::Batch::Settings s;
        if (dialog)
        {
            if (auto* cb = dialog->getComboBoxComponent ("mode")) s.fast = cb->getSelectedItemIndex() == 0;
            if (auto* cb = dialog->getComboBoxComponent ("par")) s.parallel = cb->getSelectedItemIndex();
            if (auto* cb = dialog->getComboBoxComponent ("lab")) s.toLab = cb->getSelectedItemIndex() == 0;
            if (auto* cb = dialog->getComboBoxComponent ("again")) s.skipDone = cb->getSelectedItemIndex() == 0;
        }
        { juce::Component::SafePointer<IRFitEditor> sp (this); juce::MessageManager::callAsync ([sp] { if (sp) sp->dialog.reset(); }); }   // (MSVC: no `this` in a nested init-capture)
        if (res != 1) return;
        const auto err = proc.startBatch (dir, s);
        if (err.isNotEmpty()) { status.setText ("Batch: " + err, juce::dontSendNotification); reportUntil = juce::Time::getMillisecondCounterHiRes() + 8000; }
        updateStatus();
    }), false);
}
void IRFitEditor::startConversion (const juce::File& f)
{
    if (proc.converter() != nullptr && proc.converter()->running()) { status.setText ("A conversion is already running", juce::dontSendNotification); return; }
    const auto err = proc.convertIR (f);
    if (err.isNotEmpty()) { status.setText ("Cannot convert: " + err, juce::dontSendNotification); reportUntil = juce::Time::getMillisecondCounterHiRes() + 8000; }
    updateStatus();
}

void IRFitEditor::updateStatus()
{
    juce::String label = proc.presetLabel(); if (proc.presetDirty()) label << " *";
    nameBtn.setButtonText (label);
    saveBtn.setEnabled (true);
    const bool lib = ! proc.modelActive();
    character.setEnabled (lib); era.setEnabled (lib);
    auto* c = proc.converter(); const bool run = c != nullptr && c->running();
    if (run != converting) { converting = run; overlay.setVisible (run); cancelBtn.setVisible (run); cancelBtn.toFront (false); loadBtn.setEnabled (! run); repaint(); }
    if (run) overlay.repaint();
    if (juce::Time::getMillisecondCounterHiRes() < reportUntil) return;
    juce::String s;
    const auto& B = proc.batchState();
    if (run) s << "Converting " << c->irName << "...";
    else if (B.running())
    {
        const int todo = B.total() - B.count (irfit::Batch::jSkipped), fin = B.count (irfit::Batch::jDone) + B.count (irfit::Batch::jFailed);
        s << "Batch \"" << B.folderName() << "\": " << fin << " / " << todo << " converted, " << B.count (irfit::Batch::jRunning) << " running, "
          << juce::String (juce::roundToInt (B.progress() * 100)) << " %";
        if (const double e = B.etaS(); e > 0) s << ", about " << juce::String (std::max (1, juce::roundToInt (e / 60))) << " min left";
        if (const int k = B.count (irfit::Batch::jSkipped)) s << "  (" << k << " already there)";
    }
    else if (proc.modelActive())
    {
        const auto& I = proc.currentModel().info;
        s << "MODEL  " << proc.currentModel().name;
        if (I.score >= 0) s << "   |   fit score " << juce::String (I.score, 1) << " (T30 " << juce::String (I.t30, 1) << " %, EDT " << juce::String (I.edt, 1) << " %, C80 " << juce::String (I.c80, 1) << " dB)";
    }
    else s << "Factory library: the knobs blend the nearest models of the character";
    status.setText (s, juce::dontSendNotification);
}

void IRFitEditor::timerCallback()
{
    if (proc.convFinished.load() != convSeen)
    {
        const bool first = convSeen < 0; convSeen = proc.convFinished.load();
        if (! first && proc.lastConvReport.isNotEmpty()) { status.setText (proc.lastConvReport, juce::dontSendNotification); reportUntil = juce::Time::getMillisecondCounterHiRes() + 12000; }
        advPanel.refresh();
    }
    if (proc.batchState().finished.load() != batchSeen)
    {
        batchSeen = proc.batchState().finished.load();
        if (proc.lastBatchReport.isNotEmpty()) { status.setText (proc.lastBatchReport, juce::dontSendNotification); reportUntil = juce::Time::getMillisecondCounterHiRes() + 20000; }
    }
    {   const auto& B = proc.batchState();
        const juce::String t = B.running() ? "Batch " + juce::String (juce::roundToInt (B.progress() * 100)) + " %" : juce::String ("Load IR...");
        if (loadBtn.getButtonText() != t) loadBtn.setButtonText (t); }
    updateStatus();
    if (advPanel.isVisible()) advPanel.refresh();
    hybPanel.refresh();
    // the models behind the current knobs
    juce::String s;
    if (proc.modelActive()) s << "model: " << proc.currentModel().name << " (played directly; the knobs move relative to it)";
    else
    {
        irfit::Result r; irfit::map (proc.macros(), r);
        s << "models: ";
        for (int j = 0; j < r.used; ++j)
            if (r.w[j] >= 0.05) s << (j ? "  +  " : "") << juce::String::fromUTF8 (irfit::kEntries[r.nn[j]].name) << " " << juce::String (juce::roundToInt (r.w[j] * 100)) << "%";
    }
    if (s != tail.note) { tail.note = s; tail.repaint(); }
    const int v = proc.engine().measVersion.load();
    if (v != measSeen)
    {
        measSeen = v;
        std::vector<float> L, R; proc.engine().measuredIR (L, R);
        const auto meas = proc.engine().measurement();
        if (L.size() > 480)
        {
            const int hop = 480;      // 10 ms at 48 kHz (the engine measures at 48 kHz)
            const size_t nb = L.size() / (size_t) hop;
            std::vector<double> e (nb, 0.0); double tot = 0, mx = 1e-30;
            for (size_t b = 0; b < nb; ++b) { for (int i = 0; i < hop; ++i) { const size_t k = b * (size_t) hop + (size_t) i; e[b] += (double) L[k] * L[k] + (double) R[k] * R[k]; } tot += e[b]; mx = std::max (mx, e[b]); }
            std::vector<float> envDb (nb), edcDb (nb); double rest = tot;
            for (size_t b = 0; b < nb; ++b) { envDb[b] = (float) (10 * std::log10 (e[b] / mx + 1e-12)); edcDb[b] = (float) (10 * std::log10 (rest / tot + 1e-12)); rest -= e[b]; }
            tail.setCurve (std::move (envDb), std::move (edcDb), 10.0, meas);
        }
    }
}

void IRFitEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::header); g.fillRect (0, 0, getWidth(), 48);
    g.setColour (theme::line); g.drawHorizontalLine (48, 0, (float) getWidth());
    g.setColour (theme::text); g.setFont (theme::ui (19.0f, true)); g.drawText ("IR Fit Reverb", 16, 6, 200, 24, juce::Justification::centredLeft);
    g.setColour (theme::muted); g.setFont (theme::ui (10.0f)); g.drawText ("by Johan Plus Kei  v" JucePlugin_VersionString, 16, 28, 200, 14, juce::Justification::centredLeft);
    g.setColour (theme::faint); g.setFont (theme::ui (10.0f));
    g.drawText ("PRESET", 214, 4, 80, 12, juce::Justification::centredLeft);
    g.drawText ("CHARACTER", 10, 56, 80, 22, juce::Justification::centredLeft);
    g.drawText ("ERA", 196, 56, 40, 22, juce::Justification::centredLeft);
    for (int row = 0; row < 2; ++row) { g.setColour (theme::panel); g.fillRoundedRectangle (10.0f, 266.0f + row * 114.0f, (float) getWidth() - 20, 106.0f, 5.0f); }
}

void IRFitEditor::paintOverChildren (juce::Graphics& g)
{
    if (dragOver)
    {
        g.setColour (theme::accent.withAlpha (0.18f)); g.fillRect (getLocalBounds());
        g.setColour (theme::accent); g.drawRect (getLocalBounds(), 3);
        g.setColour (theme::text); g.setFont (theme::ui (18.0f, true));
        if (dragPart >= 0)
        {
            auto b = hybPanel.getBounds().withHeight (86); if (dragPart == 0) b.setWidth (b.getWidth() / 2); else b.setLeft (b.getCentreX());
            g.setColour (theme::accent.withAlpha (0.25f)); g.fillRect (b);
            g.setColour (theme::text); g.drawText (dragPart == 0 ? "Drop: IR A for the early part (as it is)" : "Drop: IR B for the tail (as it is)", b, juce::Justification::centred);
        }
        else g.drawText ("Drop the IR to convert it to ALGO  (or on EARLY / TAIL below: play it as it is)", getLocalBounds().withTrimmedTop (60).withHeight (180), juce::Justification::centred);
    }
}

void IRFitEditor::ConvOverlay::paint (juce::Graphics& g)
{
    auto* c = proc.converter(); if (c == nullptr || ! c->running()) return;
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::shade (0.75f)); g.fillRoundedRectangle (r, 6.0f);
    g.setColour (theme::line); g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    auto in = r.reduced (16, 12);
    g.setColour (theme::text); g.setFont (theme::ui (14.0f, true));
    g.drawText ("IR -> ALGO:  " + c->irName, in.removeFromTop (20).withTrimmedRight (100).toNearestInt(), juce::Justification::centredLeft);
    g.setFont (theme::ui (11.5f)); g.setColour (theme::muted);
    g.drawText (c->stageText(), in.removeFromTop (18).toNearestInt(), juce::Justification::centredLeft);
    in.removeFromTop (6);
    auto bar = in.removeFromTop (12);
    g.setColour (theme::well); g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (theme::accent); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, c->progress.load())), 3.0f);
    in.removeFromTop (6);
    auto mmss = [] (double s) { const int t = juce::roundToInt (std::max (0.0, s)); return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2); };
    juce::String t ("elapsed " + mmss (c->elapsedS()));
    if (c->etaS() > 0) t << "   about " << mmss (c->etaS()) << " left";
    t << "   (the sound keeps playing; the result is saved in \"Converted\")";
    g.setColour (theme::muted); g.drawText (t, in.removeFromTop (16).toNearestInt(), juce::Justification::centredLeft);
}

void IRFitEditor::resized()
{
    prevBtn.setBounds (262, 14, 26, 26); nameBtn.setBounds (292, 14, 250, 26); nextBtn.setBounds (546, 14, 26, 26);
    saveBtn.setBounds (580, 14, 52, 26); moreBtn.setBounds (636, 14, 30, 26);
    loadBtn.setBounds (getWidth() - 122, 14, 110, 26);
    character.setBounds (82, 56, 106, 22); era.setBounds (224, 56, 148, 22);
    status.setBounds (382, 56, getWidth() - 382 - 104, 22);
    advBtn.setBounds (getWidth() - 98, 56, 88, 22);
    tail.setBounds (10, 86, getWidth() - 20, 172); advPanel.setBounds (tail.getBounds());
    { auto r = tail.getBounds().reduced (40, 30); overlay.setBounds (r); cancelBtn.setBounds (r.getRight() - 96, r.getY() + 12, 80, 24); }
    hybPanel.setBounds (10, 496, getWidth() - 20, 188);
    // row 1: time / space, row 2: colour / output
    const int perRow[2] = { 6, 5 }; int k = 0;
    for (int row = 0; row < 2; ++row)
    {
        const int n = perRow[row], cw = (getWidth() - 20) / 6, x0 = 10 + (getWidth() - 20 - n * cw) / 2, y = 270 + row * 114;
        for (int i = 0; i < n; ++i, ++k)
        {
            knobs[(size_t) k].l.setBounds (x0 + i * cw, y, cw, 14);
            knobs[(size_t) k].s.setBounds (x0 + i * cw + (cw - 84) / 2, y + 14, 84, 86);
        }
    }
}
