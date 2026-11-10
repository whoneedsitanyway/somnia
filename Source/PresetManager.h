#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "FactoryPresets.h"

//==============================================================================
//  Simple file-based preset manager.
//
//  Presets are stored as XML files (one per preset) in a per-user folder.
//  Saving copies the whole APVTS state; loading replaces it (which updates all
//  parameter attachments automatically).
//
//  Factory presets are embedded in the binary (see FactoryPresets.h): they are
//  installed into the user folder on first run and also merged into the preset
//  list / used as a load fallback, so they are always available.
//==============================================================================
class PresetManager
{
public:
    static constexpr const char* extension = ".somniapreset";

    explicit PresetManager (juce::AudioProcessorValueTreeState& state,
                            juce::File directory = {})
        : apvts (state)
    {
        dir = directory != juce::File() ? directory : getAppDir().getChildFile ("Presets");
    }

    static juce::File getAppDir()
    {
       #if JUCE_MAC
        return juce::File ("~/Library/Application Support/Somnia");
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Somnia");
       #endif
    }

    juce::File getDirectory() const { return dir; }

    static juce::String getDefaultPresetName() { return somnia::factory::defaultPresetName(); }

    void installFactoryPresets() const
    {
        dir.createDirectory();

        auto stamp = dir.getChildFile ("factory_version.txt");
        const int installedVersion = stamp.existsAsFile()
                                         ? stamp.loadFileAsString().getIntValue()
                                         : 0;
        const bool refreshAll = installedVersion < somnia::factory::kFactoryPresetVersion;

        for (const auto& preset : somnia::factory::presets())
        {
            auto file = fileForName (preset.name);
            if (refreshAll || ! file.existsAsFile())
                file.replaceWithText (preset.xml);
        }

        stamp.replaceWithText (juce::String (somnia::factory::kFactoryPresetVersion));
    }

    juce::StringArray getPresetNames() const
    {
        juce::StringArray names;

        for (const auto& preset : somnia::factory::presets())
            names.addIfNotAlreadyThere (preset.name);

        if (dir.isDirectory())
            for (const auto& e : juce::RangedDirectoryIterator (dir, false, juce::String ("*") + extension))
                names.addIfNotAlreadyThere (e.getFile().getFileNameWithoutExtension());

        names.sort (true);
        return names;
    }

    bool savePreset (const juce::String& name)
    {
        const auto safe = sanitise (name);
        if (safe.isEmpty())
            return false;

        dir.createDirectory();

        // The transport belongs to the session, not to a sound: it is left out of
        // the file so loading a preset can never start a recording or stop
        // playback.
        auto state = apvts.copyState();
        removeTransportFromState (state);

        auto xml = state.createXml();
        if (xml == nullptr)
            return false;

        xml->setAttribute ("presetName", safe);
        return xml->writeTo (fileForName (safe));
    }

    bool loadPreset (const juce::String& name)
    {
        const auto safe = sanitise (name);

        const auto file = fileForName (safe);
        if (file.existsAsFile())
            if (auto xml = juce::XmlDocument::parse (file))
                if (xml->hasTagName (apvts.state.getType()))
                    return applyState (juce::ValueTree::fromXml (*xml));

        if (const auto* embedded = somnia::factory::findXml (safe))
            if (auto xml = juce::XmlDocument::parse (*embedded))
                if (xml->hasTagName (apvts.state.getType()))
                    return applyState (juce::ValueTree::fromXml (*xml));

        return false;
    }

private:
    bool applyState (juce::ValueTree tree)
    {
        if (! tree.isValid())
            return false;

        // Carry the live transport (and its host sync) into the incoming tree so
        // loading an older preset that still stores them cannot move the tape.
        copyTransportInto (tree, apvts.state);

        apvts.replaceState (tree);
        return true;
    }

    static bool isTransportId (const juce::ValueTree& child)
    {
        const auto id = child.getProperty ("id").toString();
        return id == "transport" || id == "syncTransport";
    }

    static void removeTransportFromState (juce::ValueTree& tree)
    {
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (isTransportId (tree.getChild (i)))
                tree.removeChild (i, nullptr);
    }

    // Copies the transport-related parameters of `source` into `target`.
    static void copyTransportInto (juce::ValueTree& target, const juce::ValueTree& source)
    {
        for (int i = 0; i < target.getNumChildren(); ++i)
        {
            auto child = target.getChild (i);

            if (! isTransportId (child))
                continue;

            const auto id = child.getProperty ("id").toString();

            for (int j = 0; j < source.getNumChildren(); ++j)
            {
                const auto from = source.getChild (j);

                if (from.getProperty ("id").toString() == id)
                {
                    child.setProperty ("value", from.getProperty ("value"), nullptr);
                    break;
                }
            }
        }
    }

    // Preset names come from the user and from file names on disk, so they can
    // contain any script.  Only characters that are illegal in a file name are
    // replaced: the old ASCII whitelist turned a non-Latin name into an empty
    // string, so saving such a preset silently failed.
    static juce::String sanitise (const juce::String& name)
    {
        auto safe = juce::File::createLegalFileName (name).trim();

        if (safe.isEmpty() || safe.containsOnly ("."))
            return {};

        return safe.substring (0, 96);
    }

    juce::File fileForName (const juce::String& name) const
    {
        return dir.getChildFile (name + extension);
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::File dir;
};
