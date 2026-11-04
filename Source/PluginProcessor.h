#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "dsp/FeedbackSuppressor.h"
#include "AnalyserFifo.h"

// Alle parameter-ID-er samlet ett sted
namespace ids
{
    inline const juce::String sensitivity = "sensitivity";
    inline const juce::String detectTime  = "detect_time";
    inline const juce::String maxDepth    = "max_depth";
    inline const juce::String width       = "width";
    inline const juce::String release     = "release";
    inline const juce::String holdLive    = "hold_live";
    inline const juce::String lowFreq     = "low_freq";
    inline const juce::String highFreq    = "high_freq";
    inline const juce::String learn       = "learn";
    inline const juce::String freeze      = "freeze";
    inline const juce::String output      = "output";
}

// Øyeblikksbilde av én notch, publisert fra lydtråden til GUI og lagring
struct NotchSnapshot
{
    std::atomic<int>   kind { 0 };     // stilla::NotchKind
    std::atomic<float> freqHz { 0 };
    std::atomic<float> depthDb { 0 };  // nåværende kutt
    std::atomic<float> targetDb { 0 }; // kuttet den er på vei mot
};

class StillaAudioProcessor : public juce::AudioProcessor
{
public:
    StillaAudioProcessor();
    ~StillaAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Stilla"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Fra GUI-et: slett notcher (utføres i lydtråden ved neste blokk)
    enum ClearRequest { ClearNone = 0, ClearLive = 1, ClearAll = 2 };
    void requestClear (ClearRequest r) noexcept { clearRequest.store (r); }

    // Til GUI-et
    AnalyserFifo inputFifo, outputFifo;
    std::array<NotchSnapshot, stilla::kMaxNotches> notches;
    std::atomic<float> notchQ { 14.4f };
    std::atomic<double> currentSampleRate { 48000.0 };

private:
    struct StoredNotch { float freqHz = 0, depthDb = 0; };

    void applyPendingNotches() noexcept;
    void publishNotches() noexcept;
    stilla::Settings readSettings() const noexcept;
    void pushMono (AnalyserFifo&, const juce::AudioBuffer<float>&, int numChannels) noexcept;

    std::atomic<float>* sensitivity = nullptr;
    std::atomic<float>* detectTime = nullptr;
    std::atomic<float>* maxDepth = nullptr;
    std::atomic<float>* width = nullptr;
    std::atomic<float>* release = nullptr;
    std::atomic<float>* holdLive = nullptr;
    std::atomic<float>* lowFreq = nullptr;
    std::atomic<float>* highFreq = nullptr;
    std::atomic<float>* learn = nullptr;
    std::atomic<float>* freeze = nullptr;
    std::atomic<float>* output = nullptr;

    stilla::FeedbackSuppressor suppressor;
    juce::dsp::Gain<float> outputGain;
    std::vector<float> monoScratch;
    std::atomic<int> clearRequest { ClearNone };

    // Faste notcher fra et lagret prosjekt. Skrives av setState, leses av lydtråden.
    std::array<StoredNotch, stilla::kMaxNotches> pendingNotches {};
    int numPendingNotches = 0;
    std::atomic<bool> hasPendingNotches { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StillaAudioProcessor)
};
