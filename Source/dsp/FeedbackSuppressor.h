#pragma once
// Stilla – feedback-demper med null latens.
//
// Detektoren lytter på inngangen (mikrofonen). Når den finner feedback, settes et smalt
// kutt-filter på akkurat den frekvensen. Alt er IIR, så lyden forsinkes ikke.
// Ingen JUCE-avhengighet: testes direkte i Tests/DspTest.cpp.
#include "HowlDetector.h"
#include "NotchBank.h"

namespace stilla
{
struct Settings
{
    DetectorSettings detector;
    NotchSettings notches;
    bool learn  = false;   // nye notcher blir faste (for innringing under lydsjekk)
    bool freeze = false;   // ingen nye notcher, eksisterende står
};

class FeedbackSuppressor
{
public:
    void prepare (double sampleRate)
    {
        detector.prepare (sampleRate);
        bank.prepare (sampleRate);
    }

    void reset() noexcept { detector.reset(); }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        bank.setSettings (s.notches);
    }

    // channels: 1 eller 2 kanaler med numSamples samples, prosesseres på stedet
    void process (float* const* channels, int numChannels, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            // Detektoren lytter på mono-summen
            float mono = channels[0][i];
            if (numChannels > 1) mono = 0.5f * (mono + channels[1][i]);

            if (detector.push (mono, settings.detector))
                handleNewAnalysis();

            bank.updateCoefficients();
            for (int c = 0; c < numChannels; ++c)
                channels[c][i] = bank.process (channels[c][i], c);
        }
    }

    NotchBank&       getBank() noexcept       { return bank; }
    const NotchBank& getBank() const noexcept { return bank; }

private:
    void handleNewAnalysis() noexcept
    {
        bank.tick (detector.getFrameSeconds());
        if (settings.freeze)
            return;

        for (int h = 0; h < detector.getNumHowls(); ++h)
            bank.addHowl (detector.getHowl (h).freqHz, settings.learn);
    }

    Settings settings;
    HowlDetector detector;
    NotchBank bank;
};
} // namespace stilla
