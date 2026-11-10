#include "PluginEditor.h"

using namespace somnia;

//==============================================================================
SomniaAudioProcessorEditor::SomniaAudioProcessorEditor (SomniaAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
      tooltipWindow (this, 650), presetManager (p.apvts),
      tapeDisplay (p)
{
    setLookAndFeel (&lookAndFeel);
    setSize (kWidth, kFullHeight);

    // --- tape ---------------------------------------------------------------
    addKnob (speed, ids::speed, "SPEED",
             "SPEED - varispeed in semitones. Slow the tape down for a darker, drifting pitch");
    addKnob (feedback, ids::feedback, "FEEDBACK",
             "FEEDBACK - how much of the worn loop is written back into the tape on every pass");

    addCombo (loopLengthBox, loopLengthLabel, ids::loopLength, "LOOP LENGTH",
              "LOOP LENGTH - Free records until STOP; the other values lock the loop to the host tempo");
    addCombo (directionBox, directionLabel, ids::direction, "DIRECTION",
              "DIRECTION - Forward, Reverse, Ping-Pong or a random direction on every pass");

    // --- envelope ------------------------------------------------------------
    addKnob (attack, ids::attack, "ATTACK",
             "ATTACK - fade-in of the loop envelope (retriggered on every pass)");
    addKnob (decay, ids::decay, "DECAY",
             "DECAY - time to fall from the peak to the sustain level");
    addKnob (sustain, ids::sustain, "SUSTAIN",
             "SUSTAIN - level held while the tape is running");
    addKnob (release, ids::release, "RELEASE",
             "RELEASE - fade-out at the end of every pass and on STOP");

    // --- disintegration ------------------------------------------------------
    addKnob (wear, ids::wear, "WEAR",
             "WEAR - overall generation loss: darker top end, lower level, slow breathing");
    addKnob (dropouts, ids::dropouts, "DROPOUTS",
             "DROPOUTS - missing fragments of tape (with occasional stuck samples)");
    addKnob (splice, ids::splice, "SPLICE",
             "SPLICE - the read head jumps to another piece of tape: repeats and skips");
    addKnob (oxide, ids::oxide, "OXIDE",
             "OXIDE - random patches where the high frequencies have fallen off the tape");
    addKnob (wow, ids::wow, "WOW",
             "WOW - slow speed instability (wow and flutter)");
    addKnob (hiss, ids::hiss, "HISS",
             "HISS - tape noise, gated by the signal");

    // --- tone / output -------------------------------------------------------
    addKnob (tone, ids::tone, "TONE",
             "TONE - low-pass character of the tape: dark and closed on the left, open on the right");
    addKnob (sat, ids::sat, "SAT",
             "SAT - tape saturation and gentle compression");
    addKnob (mix, ids::mix, "MIX",
             "MIX - balance between the dry input and the tape loop");
    addKnob (output, ids::output, "OUTPUT",
             "OUTPUT - final output level");

    limiterButton.setTooltip ("LIMITER - transparent peak limiter on the output (off = bypass)");
    addAndMakeVisible (limiterButton);
    limiterAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processorRef.apvts, ids::limiter, limiterButton);

    syncButton.setTooltip ("SYNC - PLAY/STOP follow the host transport; turn off to run the tape freely");
    addAndMakeVisible (syncButton);
    syncAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processorRef.apvts, ids::syncTransport, syncButton);

    // --- transport -----------------------------------------------------------
    auto styleTransport = [this] (juce::TextButton& b, const juce::String& tip)
    {
        b.setTooltip (tip);
        b.setClickingTogglesState (false);
        addAndMakeVisible (b);
    };

    styleTransport (recButton,     "REC - record into the loop (or overdub over an existing loop)");
    styleTransport (playButton,    "PLAY - play the loop without recording");
    styleTransport (stopButton,    "STOP - stop the tape; a Free recording is closed at this point");
    styleTransport (slowButton,    "SLOW - ease the tape down one semitone (pitch and time follow)");
    styleTransport (fastButton,    "FAST - push the tape up one semitone (pitch and time follow)");
    styleTransport (clearButton,   "CLEAR - erase the tape");

    recButton.onClick     = [this] { processorRef.setTransport (2); };
    playButton.onClick    = [this] { processorRef.setTransport (1); };
    stopButton.onClick    = [this] { processorRef.setTransport (0); };
    slowButton.onClick    = [this] { processorRef.nudgeSpeed (-1.0f); };
    fastButton.onClick    = [this] { processorRef.nudgeSpeed ( 1.0f); };
    clearButton.onClick   = [this] { processorRef.clearLoop(); };

    // --- header --------------------------------------------------------------
    randomButton.setTooltip ("RANDOM - generate a new disintegrating loop");
    compactButton.setTooltip ("Toggle compact mode (hide the cassette)");
    randomButton.onClick  = [this] { processorRef.randomizeParameters(); };
    compactButton.onClick = [this] { setCompact (! compact); };
    addAndMakeVisible (randomButton);
    addAndMakeVisible (compactButton);

    // --- presets -------------------------------------------------------------
    presetBox.setTooltip ("PRESET - select a saved preset");
    presetBox.setJustificationType (juce::Justification::centredLeft);
    presetBox.setColour (juce::ComboBox::textColourId, findColour (SomniaLookAndFeel::accentColourId));
    presetBox.onChange = [this]
    {
        const auto name = presetBox.getText();
        if (name.isNotEmpty() && name != "(no presets)")
            loadPresetByName (name);
    };
    addAndMakeVisible (presetBox);

    saveButton.setTooltip ("SAVE - save the current settings as a preset");
    saveButton.onClick = [this] { showSavePresetDialog(); };
    addAndMakeVisible (saveButton);

    themeButton.setTooltip ("Toggle light / dark theme");
    themeButton.onClick = [this] { applyTheme (! darkTheme); };
    addAndMakeVisible (themeButton);

    logButton.setTooltip ("Open the debug log (records level anomalies, e.g. runaway feedback)");
    logButton.onClick = [this]
    {
        auto& logger = processorRef.getDebugLogger();
        if (! logger.getLogFile().existsAsFile())
            logger.clearLog();
        logger.getLogFile().revealToUser();
    };
    addAndMakeVisible (logButton);

    loadThemePreference();
    presetManager.installFactoryPresets();
    refreshPresets();

    if (! processorRef.wasStateRestored() && ! processorRef.wasDefaultPresetApplied())
    {
        loadPresetByName (PresetManager::getDefaultPresetName());
        processorRef.markDefaultPresetApplied();
    }

    addAndMakeVisible (tapeDisplay);
    updateTransportUi();
    startTimerHz (20);
}

SomniaAudioProcessorEditor::~SomniaAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void SomniaAudioProcessorEditor::timerCallback()
{
    updateTransportUi();
    logButton.setWarning (processorRef.getDebugLogger().hasAnomaly());
}

void SomniaAudioProcessorEditor::updateTransportUi()
{
    const bool recording = processorRef.getDisplayRecording();
    const bool playing   = processorRef.getDisplayPlaying();

    recButton.setToggleState (recording, juce::dontSendNotification);
    playButton.setToggleState (playing && ! recording, juce::dontSendNotification);
    stopButton.setToggleState (! playing, juce::dontSendNotification);
}

//==============================================================================
void SomniaAudioProcessorEditor::setCompact (bool shouldBeCompact)
{
    compact = shouldBeCompact;
    tapeDisplay.setVisible (! compact);
    compactButton.setButtonText (compact ? "FULL" : "COMPACT");
    setSize (getWidth(), compact ? kCompactHeight : kFullHeight);
    resized();
}

//==============================================================================
void SomniaAudioProcessorEditor::refreshPresets()
{
    presetBox.clear (juce::dontSendNotification);

    const auto names = presetManager.getPresetNames();
    if (names.isEmpty())
    {
        presetBox.addItem ("(no presets)", 1);
        presetBox.setText ("(no presets)", juce::dontSendNotification);
        return;
    }

    int id = 1;
    for (const auto& n : names)
        presetBox.addItem (n, id++);

    const auto last = processorRef.getLastPresetName();
    const auto def  = PresetManager::getDefaultPresetName();
    const auto toSelect = names.contains (last) ? last
                        : names.contains (def)  ? def
                                                : names[0];
    presetBox.setText (toSelect, juce::dontSendNotification);
    processorRef.setLastPresetName (toSelect);
}

void SomniaAudioProcessorEditor::loadPresetByName (const juce::String& name)
{
    if (presetManager.loadPreset (name))
    {
        processorRef.setLastPresetName (name);
        repaint();
    }
}

void SomniaAudioProcessorEditor::showSavePresetDialog()
{
    auto* aw = new juce::AlertWindow ("Save Preset",
                                      "Enter a name for this preset:",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", processorRef.getLastPresetName(), "Name:");
    aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // The dialog outlives this editor: the host can close the window (or delete
    // the editor) while it is modal, so the callback must not capture `this`.
    juce::Component::SafePointer<SomniaAudioProcessorEditor> safeThis (this);

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis, aw] (int result)
        {
            if (result != 1)
                return;

            const auto name = aw->getTextEditorContents ("name").trim();

            if (name.isEmpty() || safeThis == nullptr)
                return;

            if (safeThis->presetManager.savePreset (name))
            {
                safeThis->processorRef.setLastPresetName (name);
                safeThis->refreshPresets();
                safeThis->presetBox.setText (name, juce::dontSendNotification);
            }
        }), true);
}

//==============================================================================
void SomniaAudioProcessorEditor::refreshThemedColours()
{
    const auto text = findColour (SomniaLookAndFeel::textColourId);
    const auto accent = findColour (SomniaLookAndFeel::accentColourId);
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);
    const auto hot = findColour (SomniaLookAndFeel::hotColourId);

    for (auto* k : { &speed, &feedback, &attack, &decay, &sustain, &release,
                     &wear, &dropouts, &splice, &oxide, &wow, &hiss,
                     &tone, &sat, &mix, &output })
        k->label.setColour (juce::Label::textColourId, text);

    for (auto* l : { &loopLengthLabel, &directionLabel })
        l->setColour (juce::Label::textColourId, accentDim);

    loopLengthBox.setColour (juce::ComboBox::textColourId, accent);
    directionBox.setColour (juce::ComboBox::textColourId, accent);
    presetBox.setColour (juce::ComboBox::textColourId, accent);

    recButton.setColour (juce::TextButton::buttonOnColourId, hot);
    playButton.setColour (juce::TextButton::buttonOnColourId, accent);
    stopButton.setColour (juce::TextButton::buttonOnColourId, accentDim);

    repaint();
}

void SomniaAudioProcessorEditor::applyTheme (bool shouldBeDark)
{
    darkTheme = shouldBeDark;
    lookAndFeel.setDark (shouldBeDark);
    themeButton.setIcon (shouldBeDark ? IconButton::Icon::Sun : IconButton::Icon::Moon);
    refreshThemedColours();
    sendLookAndFeelChange();
    repaint();

    auto dir = PresetManager::getAppDir();
    dir.createDirectory();
    dir.getChildFile ("theme.txt").replaceWithText (shouldBeDark ? "dark" : "light");
}

void SomniaAudioProcessorEditor::loadThemePreference()
{
    const auto f = PresetManager::getAppDir().getChildFile ("theme.txt");
    darkTheme = true;
    if (f.existsAsFile())
        darkTheme = f.loadFileAsString().trim().toLowerCase() != "light";

    lookAndFeel.setDark (darkTheme);
    themeButton.setIcon (darkTheme ? IconButton::Icon::Sun : IconButton::Icon::Moon);
    refreshThemedColours();
}

//==============================================================================
void SomniaAudioProcessorEditor::addKnob (Knob& k, const juce::String& paramId,
                                          const juce::String& name,
                                          const juce::String& tooltip)
{
    addAndMakeVisible (k.slider);

    k.slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 16);
    k.slider.setTooltip (tooltip);

    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (juce::Font (juce::FontOptions (10.5f)));
    addAndMakeVisible (k.label);

    k.attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processorRef.apvts, paramId, k.slider);
}

void SomniaAudioProcessorEditor::addCombo (juce::ComboBox& box, juce::Label& label,
                                           const juce::String& paramId,
                                           const juce::String& name,
                                           const juce::String& tooltip)
{
    addAndMakeVisible (box);
    box.setJustificationType (juce::Justification::centred);
    box.setTooltip (tooltip);

    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::Font (juce::FontOptions (10.0f).withStyle ("Bold")));
    addAndMakeVisible (label);

    box.addItemList (processorRef.apvts.getParameter (paramId)->getAllValueStrings(), 1);

    comboAttachments.emplace_back (
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processorRef.apvts, paramId, box));
}

//==============================================================================
void SomniaAudioProcessorEditor::paintScrew (juce::Graphics& g, juce::Point<float> centre)
{
    const float r = 7.0f;
    juce::Rectangle<float> c (r * 2.0f, r * 2.0f);
    c.setCentre (centre);

    juce::ColourGradient grad (juce::Colours::white.withAlpha (0.22f), c.getX(), c.getY(),
                               juce::Colours::black.withAlpha (0.45f), c.getRight(), c.getBottom(),
                               false);
    g.setGradientFill (grad);
    g.fillEllipse (c);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawEllipse (c, 1.0f);

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawLine (centre.x - r * 0.55f, centre.y - r * 0.55f,
                centre.x + r * 0.55f, centre.y + r * 0.55f, 1.8f);
}

void SomniaAudioProcessorEditor::paintSectionLabel (juce::Graphics& g, juce::Rectangle<int> r,
                                                    const juce::String& title)
{
    auto header = r.reduced (14, 10).removeFromTop (16);
    g.setColour (findColour (SomniaLookAndFeel::accentDimColourId));
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    g.drawText (title, header, juce::Justification::centredLeft, false);

    g.setColour (findColour (SomniaLookAndFeel::accentDimColourId).withAlpha (0.35f));
    g.drawLine ((float) header.getX(), (float) header.getBottom() + 1.0f,
                (float) header.getRight(), (float) header.getBottom() + 1.0f, 1.0f);
}

//==============================================================================
void SomniaAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto encColour = findColour (SomniaLookAndFeel::enclosureColourId);
    g.fillAll (encColour);

    const auto encF = enclosure.toFloat();

    {
        juce::ColourGradient body (encColour.brighter (0.14f), encF.getX(), encF.getY(),
                                   encColour.darker (0.35f), encF.getRight(), encF.getBottom(),
                                   false);
        g.setGradientFill (body);
        g.fillRoundedRectangle (encF, 14.0f);
    }

    g.setColour (juce::Colours::white.withAlpha (darkTheme ? 0.07f : 0.35f));
    g.drawRoundedRectangle (encF.reduced (1.5f), 12.5f, 1.5f);
    g.setColour (juce::Colours::black.withAlpha (darkTheme ? 0.45f : 0.20f));
    g.drawRoundedRectangle (encF.reduced (0.5f), 14.0f, 1.5f);

    paintScrew (g, { encF.getX() + 22.0f, encF.getY() + 22.0f });
    paintScrew (g, { encF.getRight() - 22.0f, encF.getY() + 22.0f });
    paintScrew (g, { encF.getX() + 22.0f, encF.getBottom() - 22.0f });
    paintScrew (g, { encF.getRight() - 22.0f, encF.getBottom() - 22.0f });

    g.setColour (findColour (SomniaLookAndFeel::textColourId));
    g.setFont (juce::Font (juce::FontOptions (34.0f).withStyle ("Bold")));
    g.drawText ("SOMNIA", juce::roundToInt (encF.getX() + 34.0f),
                juce::roundToInt (encF.getY() + 20.0f), 320, 36,
                juce::Justification::centredLeft, false);

    g.setColour (findColour (SomniaLookAndFeel::textDimColourId));
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    g.drawText ("TAPE DISINTEGRATION RECORDER",
                juce::roundToInt (encF.getX() + 36.0f),
                juce::roundToInt (encF.getY() + 56.0f), 380, 16,
                juce::Justification::centredLeft, false);

   #ifdef JucePlugin_VersionString
    g.setColour (findColour (SomniaLookAndFeel::textDimColourId).withAlpha (0.75f));
    g.drawText ("v" JucePlugin_VersionString,
                juce::roundToInt (encF.getRight() - 34.0f - 120.0f),
                juce::roundToInt (encF.getY() + 56.0f), 120, 16,
                juce::Justification::centredRight, false);
   #endif

    paintSectionLabel (g, tapeArea,     "TAPE");
    paintSectionLabel (g, envelopeArea, "ENVELOPE");
    paintSectionLabel (g, disintArea,   "DISINTEGRATION");
    paintSectionLabel (g, toneArea,     "TONE / OUTPUT");

    if (transportArea.getHeight() > 0)
    {
        g.setColour (findColour (SomniaLookAndFeel::panelColourId).withAlpha (0.45f));
        g.fillRoundedRectangle (transportArea.toFloat(), 6.0f);
        g.setColour (findColour (SomniaLookAndFeel::accentDimColourId).withAlpha (0.35f));
        g.drawRoundedRectangle (transportArea.toFloat(), 6.0f, 1.0f);
    }
}

//==============================================================================
void SomniaAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    enclosure = area;

    auto inner = area.reduced (34, 28);
    inner.removeFromTop (44);

    auto row = inner.removeFromTop (30);

    presetBox.setBounds (row.removeFromLeft (200));
    row.removeFromLeft (8);
    saveButton.setBounds (row.removeFromLeft (30));
    row.removeFromLeft (6);
    themeButton.setBounds (row.removeFromLeft (30));
    row.removeFromLeft (6);
    logButton.setBounds (row.removeFromLeft (30));

    compactButton.setBounds (row.removeFromRight (100));
    row.removeFromRight (12);
    randomButton.setBounds (row.removeFromRight (100));

    inner.removeFromTop (10);

    if (! compact)
    {
        displayArea = inner.removeFromTop (230);
        tapeDisplay.setBounds (displayArea);
        inner.removeFromTop (12);
    }
    else
    {
        displayArea = {};
        tapeDisplay.setBounds ({});
    }

    transportArea = inner.removeFromTop (36);
    inner.removeFromTop (14);

    {
        auto t = transportArea.reduced (4, 3);
        const int gap = 10;
        const int w = (t.getWidth() - gap * 5) / 6;
        recButton.setBounds     (t.removeFromLeft (w)); t.removeFromLeft (gap);
        playButton.setBounds    (t.removeFromLeft (w)); t.removeFromLeft (gap);
        stopButton.setBounds    (t.removeFromLeft (w)); t.removeFromLeft (gap);
        slowButton.setBounds    (t.removeFromLeft (w)); t.removeFromLeft (gap);
        fastButton.setBounds    (t.removeFromLeft (w)); t.removeFromLeft (gap);
        clearButton.setBounds   (t.removeFromLeft (w));
    }

    const int gap = 14;
    const int colW = (inner.getWidth() - gap * 4) / 5;

    tapeArea = inner.removeFromLeft (colW);
    inner.removeFromLeft (gap);
    envelopeArea = inner.removeFromLeft (colW);
    inner.removeFromLeft (gap);
    disintArea = inner.removeFromLeft (colW * 2 + gap);
    inner.removeFromLeft (gap);
    toneArea = inner;

    auto placeGrid = [] (std::initializer_list<Knob*> ks, juce::Rectangle<int> r, int cols)
    {
        const int n = (int) ks.size();
        const int rows = (n + cols - 1) / cols;
        const int cw = r.getWidth() / cols;
        const int ch = r.getHeight() / rows;
        int i = 0;
        for (auto* k : ks)
        {
            const int c = i % cols, row2 = i / cols;
            juce::Rectangle<int> cell (r.getX() + c * cw, r.getY() + row2 * ch, cw, ch);
            auto lab = cell.removeFromBottom (15);
            k->label.setBounds (lab);
            k->slider.setBounds (cell.reduced (6, 2));
            ++i;
        }
    };

    // TAPE: two knobs + loop length / direction.
    {
        auto col = tapeArea.reduced (10, 8);
        col.removeFromTop (20);
        auto bottom = col.removeFromBottom (86);
        col.removeFromBottom (6);
        placeGrid ({ &speed, &feedback }, col, 1);

        loopLengthLabel.setBounds (bottom.removeFromTop (13));
        loopLengthBox.setBounds (bottom.removeFromTop (26));
        bottom.removeFromTop (6);
        directionLabel.setBounds (bottom.removeFromTop (13));
        directionBox.setBounds (bottom.removeFromTop (26));
    }

    // ENVELOPE: 2x2.
    {
        auto col = envelopeArea.reduced (10, 8);
        col.removeFromTop (20);
        placeGrid ({ &attack, &decay, &sustain, &release }, col, 2);
    }

    // DISINTEGRATION: 3x2.
    {
        auto col = disintArea.reduced (10, 8);
        col.removeFromTop (20);
        placeGrid ({ &wear, &dropouts, &splice, &oxide, &wow, &hiss }, col, 3);
    }

    // TONE / OUTPUT: 2x2 + limiter / sync.
    {
        auto col = toneArea.reduced (10, 8);
        col.removeFromTop (20);
        auto bottom = col.removeFromBottom (24);
        col.removeFromBottom (8);
        placeGrid ({ &tone, &sat, &mix, &output }, col, 2);

        auto half = bottom.removeFromLeft (bottom.getWidth() / 2);
        limiterButton.setBounds (half);
        syncButton.setBounds (bottom);
    }
}
