// Offline round-trip test for the PresetManager plus a check that the bundled
// factory presets are present and parse as valid APVTS state.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <iostream>

#include "../Source/PresetManager.h"

namespace
{
    struct DummyProcessor : juce::AudioProcessor
    {
        DummyProcessor() : AudioProcessor (BusesProperties()) {}
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        const juce::String getName() const override { return "Dummy"; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        double getTailLengthSeconds() const override { return 0.0; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    };
}

int main()
{
    DummyProcessor proc;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> ("mix", "Mix", 0.0f, 1.0f, 0.70f));
    layout.add (std::make_unique<juce::AudioParameterChoice> ("loopLength", "Loop Length",
                 juce::StringArray { "Free", "1/1", "2/1", "4/1", "8/1", "16/1" }, 2));
    layout.add (std::make_unique<juce::AudioParameterFloat> ("wear", "Wear", 0.0f, 1.0f, 0.35f));
    layout.add (std::make_unique<juce::AudioParameterChoice> ("direction", "Direction",
                 juce::StringArray { "Forward", "Reverse", "Ping-Pong", "Random" }, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> ("transport", "Transport",
                 juce::StringArray { "Stop", "Play", "Rec" }, 0));

    juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", std::move (layout));

    auto tmpDir = juce::File::getCurrentWorkingDirectory().getChildFile ("somnia_preset_test_tmp");
    tmpDir.deleteRecursively();
    tmpDir.createDirectory();

    PresetManager pm (apvts, tmpDir);

    auto setVal = [&] (const char* id, float v)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    auto getVal = [&] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    setVal ("mix", 0.42f);
    setVal ("loopLength", 5.0f);
    setVal ("wear", 0.81f);
    setVal ("direction", 2.0f);

    const bool saved = pm.savePreset ("Unit Test");
    const bool listed = pm.getPresetNames().contains ("Unit Test");

    // The loop length belongs to the take, not to the preset: it must be absent
    // from the file and survive loading whatever the current value is.
    const auto unitTestFile = tmpDir.getChildFile (juce::String ("Unit Test")
                                                   + PresetManager::extension);
    const bool lengthAbsent = unitTestFile.existsAsFile()
                           && ! unitTestFile.loadFileAsString().contains ("id=\"loopLength\"");

    setVal ("mix", 0.10f);
    setVal ("loopLength", 0.0f);
    setVal ("wear", 0.05f);
    setVal ("direction", 0.0f);

    const bool loaded = pm.loadPreset ("Unit Test");

    const float mix = getVal ("mix");
    const float len = getVal ("loopLength");
    const float wear = getVal ("wear");
    const float dir = getVal ("direction");

    const bool valuesOk = std::abs (mix - 0.42f) < 0.001f
                       && std::abs (len - 0.0f) < 0.01f
                       && std::abs (wear - 0.81f) < 0.001f
                       && std::abs (dir - 2.0f) < 0.01f;

    // --- non-ASCII preset names ---------------------------------------------
    // sanitise() used to keep ASCII only, so a name in another script became an
    // empty string and saving failed silently.
    const juce::String unicodeName = juce::String::fromUTF8 ("Меланхолия \u00c4");
    const bool unicodeSaved = pm.savePreset (unicodeName);
    const bool unicodeListed = pm.getPresetNames().contains (unicodeName);
    setVal ("wear", 0.11f);
    const bool unicodeLoaded = pm.loadPreset (unicodeName);
    const bool unicodeOk = unicodeSaved && unicodeListed && unicodeLoaded
                        && std::abs (getVal ("wear") - 0.81f) < 0.001f;

    // --- the transport must not travel inside a preset -----------------------
    // Loading a preset used to restore the transport, which could start a
    // recording (or stop playback) just because a sound was selected.
    setVal ("transport", 2.0f);   // Rec
    const bool transportSaved = pm.savePreset ("Transport Test");
    const auto transportFile = tmpDir.getChildFile (juce::String ("Transport Test") + PresetManager::extension);
    const bool transportAbsent = transportFile.existsAsFile()
                              && ! transportFile.loadFileAsString().contains ("id=\"transport\"");

    setVal ("transport", 0.0f);
    const bool transportLoaded = pm.loadPreset ("Transport Test");
    const bool transportOk = transportSaved && transportAbsent && transportLoaded
                          && std::abs (getVal ("transport") - 0.0f) < 0.01f;

    // --- an older preset that still stores the loop length -------------------
    // Loading such a file must keep the live value instead of resizing the tape
    // and cutting off the material that is already recorded.
    const auto oldLengthFile = tmpDir.getChildFile (juce::String ("Old Length")
                                                    + PresetManager::extension);
    oldLengthFile.replaceWithText ("<PARAMETERS presetName=\"Old Length\">\n"
                                   "  <PARAM id=\"loopLength\" value=\"5\"/>\n"
                                   "  <PARAM id=\"wear\" value=\"0.42\"/>\n"
                                   "</PARAMETERS>\n");

    setVal ("loopLength", 1.0f);
    const bool oldLengthLoaded = pm.loadPreset ("Old Length");
    const bool oldLengthOk = oldLengthLoaded
                          && std::abs (getVal ("loopLength") - 1.0f) < 0.01f
                          && std::abs (getVal ("wear") - 0.42f) < 0.001f;

    tmpDir.deleteRecursively();

    // --- factory presets -----------------------------------------------------
    const auto& factory = somnia::factory::presets();
    const bool factoryOk = factory.size() >= 8
                        && somnia::factory::defaultPresetName() == "Melancholia"
                        && somnia::factory::findXml ("Melancholia") != nullptr
                        && somnia::factory::findXml ("Broken Reel") != nullptr;

    bool factoryXmlOk = true;
    bool factoryLengthAbsent = true;
    for (const auto& p : factory)
    {
        auto xml = juce::XmlDocument::parse (p.xml);
        if (xml == nullptr || ! xml->hasTagName ("PARAMETERS")
            || xml->getNumChildElements() < 15)
            factoryXmlOk = false;

        if (p.xml.contains ("id=\"loopLength\""))
            factoryLengthAbsent = false;
    }

    std::cout << "saved=" << saved << " listed=" << listed << " loaded=" << loaded
              << " mix=" << mix << " len=" << len << " wear=" << wear << " dir=" << dir
              << " lengthAbsent=" << lengthAbsent
              << " unicode=" << unicodeOk << " transport=" << transportOk
              << " oldLength=" << oldLengthOk
              << " factory=" << factory.size() << " factoryOk=" << factoryOk
              << " factoryXmlOk=" << factoryXmlOk
              << " factoryLengthAbsent=" << factoryLengthAbsent << std::endl;

    if (! (saved && listed && loaded && valuesOk && lengthAbsent && unicodeOk
           && transportOk && oldLengthOk && factoryOk && factoryXmlOk
           && factoryLengthAbsent))
    {
        std::cout << "FAIL: preset round-trip" << std::endl;
        return 1;
    }

    std::cout << "PASS: preset round-trip" << std::endl;
    return 0;
}
