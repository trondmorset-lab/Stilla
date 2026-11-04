#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

namespace ui
{
    const juce::Colour bg      { 0xff0f1215 };
    const juce::Colour panel   { 0xff171b1f };
    const juce::Colour grid    { 0xff252b31 };
    const juce::Colour gridHi  { 0xff323a42 };
    const juce::Colour text    { 0xffc5cdd4 };
    const juce::Colour dim     { 0xff6d7781 };
    const juce::Colour curve   { 0xffeef3f6 };
    const juce::Colour output  { 0xff4fb7c9 };   // utgangsspekter
    const juce::Colour live    { 0xffe8923a };   // live-notcher
    const juce::Colour fixed   { 0xff7aa8ff };   // faste notcher
    const juce::Colour learn   { 0xffe2645a };   // Learn-modus
}

class StillaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StillaLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
// Sanntids-RTA (inn og ut), samlet notch-kurve og markører for hver notch
class SpectrumDisplay : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumDisplay (StillaAudioProcessor&);
    ~SpectrumDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateSpectrum (AnalyserFifo&, std::vector<float>& ring, int& writePos, std::vector<float>& display);

    juce::Rectangle<float> plot() const;
    float freqToX (float f) const;
    float levelToY (float db) const;   // RTA-skala
    float cutToY (float db) const;     // notch-skala

    StillaAudioProcessor& proc;

    static constexpr int kOrder = 12, kSize = 1 << kOrder;
    juce::dsp::FFT fft { kOrder };
    std::vector<float> window, fftData, scratch;
    std::vector<float> inputRing, outputRing;
    int inputWrite = 0, outputWrite = 0;
    std::vector<float> xFreqs, inputDisplay, outputDisplay;   // ett punkt per 2 piksler

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
};

//==============================================================================
// Liste over aktive notcher, sortert på frekvens
class NotchList : public juce::Component, private juce::Timer
{
public:
    explicit NotchList (StillaAudioProcessor&);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }
    StillaAudioProcessor& proc;
};

//==============================================================================
class StillaAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit StillaAudioProcessorEditor (StillaAudioProcessor&);
    ~StillaAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    struct Toggle
    {
        juce::TextButton button;
        std::unique_ptr<ButtonAttachment> attachment;
    };

    void timerCallback() override { repaint (0, 0, getWidth(), 40); }
    void setupKnob (Knob&, const juce::String& paramId, const juce::String& name);
    void setupToggle (Toggle&, const juce::String& paramId, const juce::String& name, juce::Colour onColour);

    StillaAudioProcessor& proc;
    StillaLookAndFeel lookAndFeel;
    SpectrumDisplay spectrum;
    NotchList notchList;

    Knob sensitivity, detectTime, maxDepth, width, release, lowFreq, highFreq, output;
    Toggle learn, freeze, holdLive;
    juce::TextButton clearLive { "Clear Live" }, clearAll { "Clear All" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StillaAudioProcessorEditor)
};
