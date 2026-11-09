#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace somnia
{

//==============================================================================
//  Minimal, tactile look-and-feel with a switchable dark / soft-light theme.
//
//  The visual language follows Vespera and Lunara (chunky cylinder knobs, a
//  ring of ticks, a rounded metal enclosure), but the palette is Somnia's own:
//  deep twilight violet, moonlight lavender, oxidised copper and a cold tape
//  hiss teal.
//==============================================================================
class SomniaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    enum ColourIds
    {
        bgColourId        = 0x1a2b0001,
        panelColourId     = 0x1a2b0002,
        enclosureColourId = 0x1a2b0003,
        accentColourId    = 0x1a2b0004,
        accentDimColourId = 0x1a2b0005,
        coolColourId      = 0x1a2b0006,
        textColourId      = 0x1a2b0007,
        textDimColourId   = 0x1a2b0008,
        hotColourId       = 0x1a2b0009,
        scopeColourId     = 0x1a2b000a,   // the "screen" behind the tape map
        scopeDimColourId  = 0x1a2b000b    // wash over the material outside the loop
    };

    SomniaLookAndFeel();

    void setDark (bool shouldBeDark);
    bool isDark() const noexcept { return dark; }

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;

private:
    void applyPalette();
    bool dark = true;
};

//==============================================================================
//  A small square button that draws a vector icon (save / sun / moon / log).
//==============================================================================
class IconButton : public juce::Button
{
public:
    enum class Icon { Save, Sun, Moon, Log };

    IconButton (Icon iconToDraw, const juce::String& name);

    void setIcon (Icon newIcon) { icon = newIcon; repaint(); }

    void setWarning (bool shouldWarn)
    {
        if (warning != shouldWarn) { warning = shouldWarn; repaint(); }
    }

    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    Icon icon;
    bool warning = false;
};

} // namespace somnia
