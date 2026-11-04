// Rendrer plugin-editoren til PNG med noen notcher og lyd i analysatoren
#include "../Source/PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    auto* p = dynamic_cast<StillaAudioProcessor*> (proc.get());

    auto set = [&] (const juce::String& id, float v)
    {
        if (auto* prm = p->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };

    const double fs = 48000; const int bs = 512;
    proc->setPlayConfigDetails (2, 2, fs, bs);
    proc->prepareToPlay (fs, bs);
    juce::AudioBuffer<float> buf (2, bs); juce::MidiBuffer midi; juce::Random rng (5);
    long n = 0;

    // tones: frekvenser som "hyler" akkurat nå
    auto feed = [&] (std::initializer_list<float> tones, float toneGain)
    {
        for (int i = 0; i < bs; ++i, ++n)
        {
            const double t = (double) n / fs;
            const double f0 = 165 + 25 * std::sin (t * 2.1);   // "tale" med glidende tonehøyde
            float v = 0.01f * (rng.nextFloat() * 2 - 1);
            for (int h = 1; h <= 14; ++h)
                v += 0.05f / (float) h * (float) std::sin (2 * juce::MathConstants<double>::pi * f0 * h * t);
            for (float f : tones)
                v += toneGain * (float) std::sin (2 * juce::MathConstants<double>::pi * f * t);
            buf.setSample (0, i, v); buf.setSample (1, i, v);
        }
        proc->processBlock (buf, midi);
    };

    // Lydsjekk med Learn: to faste notcher
    set ("learn", 1);
    for (int i = 0; i < 100; ++i) feed ({ 250.0f, 1010.0f }, 0.05f);
    set ("learn", 0);
    // Under konserten: tre live-notcher, én av dem flere ganger (dypere)
    for (int i = 0; i < 160; ++i) feed ({ 2870.0f, 4230.0f, 640.0f }, 0.04f);
    for (int i = 0; i < 40; ++i) feed ({}, 0);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
    for (int i = 0; i < 60; ++i) { feed ({}, 0); juce::MessageManager::getInstance()->runDispatchLoopUntil (16); }
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File out (argc > 1 ? argv[1] : "snapshot.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
    proc->editorBeingDeleted (ed.get());
    ed.reset();
    return 0;
}
