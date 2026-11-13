#pragma once

#include <juce_core/juce_core.h>

#include <vector>

//==============================================================================
//  Factory presets bundled with the plugin.
//
//  The XML state of each preset is built at start-up and embedded in the
//  binary, so the presets are always available (even on a fresh install with
//  an empty user folder).  They are installed to the user preset directory
//  (see PresetManager::installFactoryPresets) and are also used as a fallback
//  when loading a preset whose file is missing.
//
//  kFactoryPresetVersion is bumped whenever the bundled presets change; the
//  installer then refreshes the factory files in the user folder (while user
//  presets are never touched).
//
//  Loop Length is deliberately not part of a preset: it belongs to the take the
//  user recorded, so selecting a preset must never resize the tape and cut off
//  existing material.
//==============================================================================
namespace somnia::factory
{
    inline constexpr int kFactoryPresetVersion = 4;

    struct Preset
    {
        juce::String name;
        juce::String xml;
    };

    struct Values
    {
        float mix        = 0.70f;
        float loopStart  = 0.0f;    // fraction of the recorded tape
        float loopEnd    = 1.0f;
        float speed      = 0.0f;    // semitones
        int   direction  = 0;       // Forward, Reverse, Ping-Pong, Random
        float feedback   = 0.55f;
        int   transport  = 0;       // Stop (presets never start the tape by themselves)

        float attack     = 20.0f;
        float decay      = 200.0f;
        float sustain    = 0.90f;
        float release    = 150.0f;

        float wear       = 0.35f;
        float dropouts   = 0.25f;
        float splice     = 0.10f;
        float oxide      = 0.20f;
        float wow        = 0.20f;
        float hiss       = 0.12f;

        float tone       = 0.70f;
        float sat        = 0.20f;
        float output     = 0.0f;
        float limiter    = 1.0f;
        float syncTransport = 1.0f;
    };

    inline juce::String makeXml (const juce::String& name, const Values& v)
    {
        juce::String xml;
        xml << "<PARAMETERS presetName=\"" << name << "\">\n";

        auto param = [&xml] (const char* id, float value)
        {
            xml << "  <PARAM id=\"" << id << "\" value=\""
                << juce::String (value, 6) << "\"/>\n";
        };

        param ("mix",        v.mix);
        param ("loopStart",  v.loopStart);
        param ("loopEnd",    v.loopEnd);
        param ("speed",      v.speed);
        param ("direction",  (float) v.direction);
        param ("feedback",   v.feedback);
        param ("transport",  (float) v.transport);
        param ("attack",     v.attack);
        param ("decay",      v.decay);
        param ("sustain",    v.sustain);
        param ("release",    v.release);
        param ("wear",       v.wear);
        param ("dropouts",   v.dropouts);
        param ("splice",     v.splice);
        param ("oxide",      v.oxide);
        param ("wow",        v.wow);
        param ("hiss",       v.hiss);
        param ("tone",       v.tone);
        param ("sat",        v.sat);
        param ("output",     v.output);
        param ("limiter",    v.limiter);
        param ("syncTransport", v.syncTransport);

        xml << "</PARAMETERS>\n";
        return xml;
    }

    inline std::vector<Preset> buildPresets()
    {
        std::vector<Preset> list;

        auto add = [&list] (const char* name, const Values& v)
        {
            list.push_back ({ juce::String (name), makeXml (name, v) });
        };

        // 1. A slow, dignified loop that loses its top end.
        {
            Values v;
            v.feedback = 0.62f;
            v.attack = 40.0f; v.decay = 400.0f; v.sustain = 0.95f; v.release = 300.0f;
            v.wear = 0.55f; v.dropouts = 0.35f; v.splice = 0.08f; v.oxide = 0.45f;
            v.wow = 0.30f; v.hiss = 0.18f; v.tone = 0.55f; v.sat = 0.30f;
            v.mix = 0.75f;
            add ("dlp 1.1", v);
        }

        // 2. Long, heavily worn loop: the tape is almost gone.
        {
            Values v;
            v.feedback = 0.70f;
            v.attack = 80.0f; v.decay = 800.0f; v.sustain = 0.90f; v.release = 600.0f;
            v.wear = 0.75f; v.dropouts = 0.55f; v.splice = 0.10f; v.oxide = 0.60f;
            v.wow = 0.40f; v.hiss = 0.22f; v.tone = 0.40f; v.sat = 0.40f;
            v.mix = 0.80f;
            add ("Disintegration Loop", v);
        }

        // 3. Reverse tape, heavy oxide loss.
        {
            Values v;
            v.speed = -3.0f; v.direction = 1; v.feedback = 0.50f;
            v.attack = 20.0f; v.decay = 300.0f; v.sustain = 0.85f; v.release = 200.0f;
            v.wear = 0.65f; v.dropouts = 0.20f; v.splice = 0.25f; v.oxide = 0.85f;
            v.wow = 0.20f; v.hiss = 0.30f; v.tone = 0.35f; v.sat = 0.25f;
            v.mix = 0.70f;
            add ("Oxide Dust", v);
        }

        // 4. Playful splices and ping-pong head.
        {
            Values v;
            v.direction = 2; v.feedback = 0.60f;
            v.attack = 10.0f; v.decay = 150.0f; v.sustain = 0.80f; v.release = 100.0f;
            v.wear = 0.30f; v.dropouts = 0.30f; v.splice = 0.65f; v.oxide = 0.20f;
            v.wow = 0.15f; v.hiss = 0.10f; v.tone = 0.80f; v.sat = 0.15f;
            v.mix = 0.70f;
            add ("Splice Garden", v);
        }

        // 5. Slow, wow-heavy night loop.
        {
            Values v;
            v.speed = -5.0f; v.feedback = 0.65f;
            v.attack = 120.0f; v.decay = 1200.0f; v.sustain = 0.95f; v.release = 900.0f;
            v.wear = 0.40f; v.dropouts = 0.15f; v.splice = 0.05f; v.oxide = 0.30f;
            v.wow = 0.50f; v.hiss = 0.12f; v.tone = 0.50f; v.sat = 0.20f;
            v.mix = 0.80f;
            add ("Night Ferry", v);
        }

        // 6. Fast, broken and unpredictable.
        {
            Values v;
            v.speed = 2.0f; v.direction = 3; v.feedback = 0.75f;
            v.attack = 5.0f; v.decay = 100.0f; v.sustain = 0.70f; v.release = 80.0f;
            v.wear = 0.80f; v.dropouts = 0.70f; v.splice = 0.50f; v.oxide = 0.50f;
            v.wow = 0.60f; v.hiss = 0.35f; v.tone = 0.30f; v.sat = 0.50f;
            v.mix = 0.70f;
            add ("Broken Reel", v);
        }

        // 7. Noise-forward, almost a pad.
        {
            Values v;
            v.feedback = 0.80f;
            v.attack = 200.0f; v.decay = 2000.0f; v.sustain = 1.0f; v.release = 1500.0f;
            v.wear = 0.45f; v.dropouts = 0.10f; v.splice = 0.00f; v.oxide = 0.50f;
            v.wow = 0.25f; v.hiss = 0.55f; v.tone = 0.45f; v.sat = 0.10f;
            v.mix = 0.90f;
            add ("Hiss Cathedral", v);
        }

        // 8. Deep, slow and melancholic - the default: reverse, a long decay and
        //    a dark but still intelligible loop.
        {
            Values v;
            v.speed = -7.0f; v.direction = 1; v.feedback = 0.60f;
            v.attack = 60.0f; v.decay = 900.0f; v.sustain = 0.90f; v.release = 700.0f;
            v.wear = 0.50f; v.dropouts = 0.25f; v.splice = 0.00f; v.oxide = 0.35f;
            v.wow = 0.35f; v.hiss = 0.15f; v.tone = 0.45f; v.sat = 0.20f;
            v.mix = 0.85f;
            add ("Melancholia", v);
        }

        // 9. Free loop that dies as it plays.
        {
            Values v;
            v.speed = -12.0f; v.feedback = 0.70f;
            v.attack = 10.0f; v.decay = 400.0f; v.sustain = 0.80f; v.release = 500.0f;
            v.wear = 0.60f; v.dropouts = 0.40f; v.splice = 0.20f; v.oxide = 0.40f;
            v.wow = 0.50f; v.hiss = 0.20f; v.tone = 0.35f; v.sat = 0.35f;
            v.mix = 0.75f;
            add ("Tape Stop", v);
        }

        // 10. Long ping-pong loop with a huge, slow envelope.
        {
            Values v;
            v.direction = 2; v.feedback = 0.78f;
            v.attack = 150.0f; v.decay = 1500.0f; v.sustain = 0.98f; v.release = 1200.0f;
            v.wear = 0.60f; v.dropouts = 0.30f; v.splice = 0.15f; v.oxide = 0.55f;
            v.wow = 0.45f; v.hiss = 0.25f; v.tone = 0.40f; v.sat = 0.25f;
            v.mix = 0.80f;
            add ("Infinite November", v);
        }

        return list;
    }

    inline const std::vector<Preset>& presets()
    {
        static const auto list = buildPresets();
        return list;
    }

    inline juce::String defaultPresetName() { return "Melancholia"; }

    inline const juce::String* findXml (const juce::String& name)
    {
        for (const auto& p : presets())
            if (p.name == name)
                return &p.xml;

        return nullptr;
    }
}
