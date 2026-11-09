#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "PresetManager.h"
#include "ui/SomniaLookAndFeel.h"
#include "ui/TapeDisplay.h"

//==============================================================================
class SomniaAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit SomniaAudioProcessorEditor (SomniaAudioProcessor&);
    ~SomniaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void setCompact (bool shouldBeCompact);
    void updateTransportUi();

    void refreshPresets();
    void loadPresetByName (const juce::String& name);
    void showSavePresetDialog();
    void applyTheme (bool shouldBeDark);
    void loadThemePreference();
    void refreshThemedColours();

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    };

    void addKnob (Knob& k, const juce::String& paramId, const juce::String& name,
                  const juce::String& tooltip);
    void addCombo (juce::ComboBox& box, juce::Label& label,
                   const juce::String& paramId, const juce::String& name,
                   const juce::String& tooltip);
    void paintSectionLabel (juce::Graphics&, juce::Rectangle<int>,
                            const juce::String& title);
    void paintScrew (juce::Graphics&, juce::Point<float> centre);

    SomniaAudioProcessor& processorRef;
    somnia::SomniaLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow;
    PresetManager presetManager;

    // TAPE
    Knob speed, feedback;
    juce::ComboBox loopLengthBox, directionBox;
    juce::Label    loopLengthLabel, directionLabel;

    // ENVELOPE
    Knob attack, decay, sustain, release;

    // DISINTEGRATION
    Knob wear, dropouts, splice, oxide, wow, hiss;

    // TONE / OUTPUT
    Knob tone, sat, mix, output;
    juce::ToggleButton limiterButton { "Limiter" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> limiterAttach;

    juce::ToggleButton syncButton { "Sync" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> syncAttach;

    // Transport (momentary buttons; the state lives in the transport parameter).
    juce::TextButton recButton     { "REC" };
    juce::TextButton playButton    { "PLAY" };
    juce::TextButton stopButton    { "STOP" };
    juce::TextButton slowButton    { "SLOW" };
    juce::TextButton fastButton    { "FAST" };
    juce::TextButton clearButton   { "CLEAR" };

    juce::ComboBox presetBox;
    somnia::IconButton saveButton  { somnia::IconButton::Icon::Save, "Save preset" };
    somnia::IconButton themeButton { somnia::IconButton::Icon::Moon, "Toggle theme" };
    somnia::IconButton logButton   { somnia::IconButton::Icon::Log,  "Open the debug log" };

    juce::TextButton randomButton  { "RANDOM" };
    juce::TextButton compactButton { "COMPACT" };

    somnia::TapeDisplay tapeDisplay;

    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAttachments;

    bool compact = false;
    bool darkTheme = true;
    bool lastRecording = false;
    bool lastPlaying = false;

    static constexpr int kWidth         = 1120;
    static constexpr int kFullHeight    = 740;
    static constexpr int kCompactHeight = 500;

    juce::Rectangle<int> enclosure;
    juce::Rectangle<int> displayArea, transportArea;
    juce::Rectangle<int> tapeArea, envelopeArea, disintArea, toneArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SomniaAudioProcessorEditor)
};
