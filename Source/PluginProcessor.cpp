#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

//==============================================================================
// Parametere

static NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float s, float e, float t) { return s * std::pow (e / s, t); },
             [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
             [] (float s, float e, float v) { return jlimit (s, e, v); } };
}

static AudioParameterFloatAttributes withText (std::function<String (float, int)> toText)
{
    return AudioParameterFloatAttributes().withStringFromValueFunction (std::move (toText));
}

static String hzText (float v, int)      { return v < 1000 ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 2) + " kHz"; }
static String dbText (float v, int)      { return String (v, 1) + " dB"; }
static String msText (float v, int)      { return String (roundToInt (v)) + " ms"; }
static String secText (float v, int)     { return v < 10 ? String (v, 1) + " s" : String (roundToInt (v)) + " s"; }
static String percentText (float v, int) { return String (roundToInt (v)) + " %"; }
static String octaveText (float v, int)  { return "1/" + String (roundToInt (1.0f / v)) + " oct"; }

AudioProcessorValueTreeState::ParameterLayout StillaAudioProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto add = [&] (const String& id, const String& name, NormalisableRange<float> range, float def, String (*text) (float, int))
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def, withText (text)));
    };
    auto addBool = [&] (const String& id, const String& name, bool def)
    {
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };

    add (ids::sensitivity, "Sensitivity", { 0, 100, 1 },   50,    percentText);
    add (ids::detectTime,  "Detect Time", logRange (50, 1000), 150, msText);
    add (ids::maxDepth,    "Max Depth",   { 6, 30, 0.5f },  18,    dbText);
    add (ids::width,       "Width",       logRange (1.0f / 60, 1.0f / 3), 0.1f, octaveText);
    add (ids::release,     "Release",     logRange (1, 60), 10,    secText);
    add (ids::lowFreq,     "Low",         logRange (20, 2000),   80,    hzText);
    add (ids::highFreq,    "High",        logRange (1000, 20000), 12000, hzText);
    add (ids::output,      "Output",      { -24, 12, 0.1f }, 0,     dbText);
    addBool (ids::holdLive, "Hold Live", false);
    addBool (ids::learn,    "Learn",     false);
    addBool (ids::freeze,   "Freeze",    false);
    return layout;
}

StillaAudioProcessor::StillaAudioProcessor()
    : AudioProcessor (BusesProperties().withInput  ("Input",  AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Stilla", createLayout())
{
    // Pekerne hentes én gang. I lydtråden leses de som atomics.
    sensitivity = apvts.getRawParameterValue (ids::sensitivity);
    detectTime  = apvts.getRawParameterValue (ids::detectTime);
    maxDepth    = apvts.getRawParameterValue (ids::maxDepth);
    width       = apvts.getRawParameterValue (ids::width);
    release     = apvts.getRawParameterValue (ids::release);
    holdLive    = apvts.getRawParameterValue (ids::holdLive);
    lowFreq     = apvts.getRawParameterValue (ids::lowFreq);
    highFreq    = apvts.getRawParameterValue (ids::highFreq);
    learn       = apvts.getRawParameterValue (ids::learn);
    freeze      = apvts.getRawParameterValue (ids::freeze);
    output      = apvts.getRawParameterValue (ids::output);
}

bool StillaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == AudioChannelSet::mono() || out == AudioChannelSet::stereo())
        && out == layouts.getMainInputChannelSet();
}

stilla::Settings StillaAudioProcessor::readSettings() const noexcept
{
    stilla::Settings s;
    s.detector.sensitivity  = sensitivity->load();
    s.detector.detectTimeMs = detectTime->load();
    s.detector.lowHz        = lowFreq->load();
    s.detector.highHz       = jmax (lowFreq->load() * 2.0f, highFreq->load());
    s.notches.maxDepthDb    = maxDepth->load();
    s.notches.widthOct      = width->load();
    s.notches.releaseSec    = release->load();
    s.notches.holdLive      = holdLive->load() > 0.5f;
    s.learn                 = learn->load() > 0.5f;
    s.freeze                = freeze->load() > 0.5f;
    return s;
}

//==============================================================================
// Lyd

void StillaAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate.store (sampleRate);

    suppressor.prepare (sampleRate);
    suppressor.setSettings (readSettings());
    applyPendingNotches();

    outputGain.prepare ({ sampleRate, (uint32) jmax (1, samplesPerBlock), 2 });
    outputGain.setRampDurationSeconds (0.05);
    outputGain.setGainDecibels (output->load());
    outputGain.reset();

    monoScratch.assign ((size_t) jmax (512, samplesPerBlock), 0.0f);

    // Alt er IIR: ingen latens, så vertens standard-bypass (ren kopi) er riktig
    setLatencySamples (0);
    publishNotches();
}

void StillaAudioProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int numChannels = jmin (2, getTotalNumInputChannels());
    const int numSamples = buffer.getNumSamples();
    for (int c = numChannels; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, numSamples);
    if (numSamples == 0 || numChannels == 0)
        return;

    // Forespørsler fra GUI og lagret prosjekt
    applyPendingNotches();
    switch (clearRequest.exchange (ClearNone))
    {
        case ClearLive: suppressor.getBank().clear (false); break;
        case ClearAll:  suppressor.getBank().clear (true);  break;
        default: break;
    }

    suppressor.setSettings (readSettings());
    pushMono (inputFifo, buffer, numChannels);

    suppressor.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);

    outputGain.setGainDecibels (output->load());
    dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numChannels, (size_t) numSamples);
    outputGain.process (dsp::ProcessContextReplacing<float> (block));

    pushMono (outputFifo, buffer, numChannels);
    publishNotches();
}

// Mono-sum til analysatoren, i biter så scratch-bufferen aldri blir for liten
void StillaAudioProcessor::pushMono (AnalyserFifo& fifo, const AudioBuffer<float>& buffer, int numChannels) noexcept
{
    const int chunk = (int) monoScratch.size();
    for (int start = 0; start < buffer.getNumSamples(); start += chunk)
    {
        const int len = jmin (chunk, buffer.getNumSamples() - start);
        FloatVectorOperations::copy (monoScratch.data(), buffer.getReadPointer (0, start), len);
        if (numChannels > 1)
        {
            FloatVectorOperations::add (monoScratch.data(), buffer.getReadPointer (1, start), len);
            FloatVectorOperations::multiply (monoScratch.data(), 0.5f, len);
        }
        fifo.push (monoScratch.data(), len);
    }
}

void StillaAudioProcessor::publishNotches() noexcept
{
    const auto& bank = suppressor.getBank();
    for (int i = 0; i < stilla::kMaxNotches; ++i)
    {
        auto& n = notches[(size_t) i];
        n.kind.store ((int) bank.getKind (i), std::memory_order_relaxed);
        n.freqHz.store (bank.getFreqHz (i), std::memory_order_relaxed);
        n.depthDb.store (bank.getDepthDb (i), std::memory_order_relaxed);
        n.targetDb.store (bank.getTargetDb (i), std::memory_order_relaxed);
    }
    notchQ.store (bank.getQ(), std::memory_order_relaxed);
}

void StillaAudioProcessor::applyPendingNotches() noexcept
{
    if (! hasPendingNotches.load (std::memory_order_acquire))
        return;

    auto& bank = suppressor.getBank();
    bank.clear (true);
    for (int i = 0; i < numPendingNotches; ++i)
        bank.addFixed (pendingNotches[(size_t) i].freqHz, pendingNotches[(size_t) i].depthDb);
    hasPendingNotches.store (false, std::memory_order_release);
}

AudioProcessorEditor* StillaAudioProcessor::createEditor() { return new StillaAudioProcessorEditor (*this); }

//==============================================================================
// Lagring: parametere + faste notcher (fra Learn), så innringingen følger prosjektet

void StillaAudioProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", 1, nullptr);
    state.removeChild (state.getChildWithName ("FixedNotches"), nullptr);

    ValueTree fixed ("FixedNotches");
    auto addNotch = [&] (float freq, float depth)
    {
        ValueTree n ("Notch");
        n.setProperty ("freq", freq, nullptr);
        n.setProperty ("depth", depth, nullptr);
        fixed.appendChild (n, nullptr);
    };

    if (hasPendingNotches.load())
    {
        // Prosjekt lastet, men lydtråden har ikke tatt det i bruk ennå
        for (int i = 0; i < numPendingNotches; ++i)
            addNotch (pendingNotches[(size_t) i].freqHz, pendingNotches[(size_t) i].depthDb);
    }
    else
    {
        for (auto& n : notches)
            if (n.kind.load() == (int) stilla::NotchKind::Fixed)
                addNotch (n.freqHz.load(), n.targetDb.load());
    }
    state.appendChild (fixed, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void StillaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = ValueTree::fromXml (*xml);
    auto fixed = state.getChildWithName ("FixedNotches");
    state.removeChild (fixed, nullptr);
    apvts.replaceState (state);

    numPendingNotches = 0;
    for (const auto& n : fixed)
        if (numPendingNotches < stilla::kMaxNotches)
            pendingNotches[(size_t) numPendingNotches++] = { (float) n["freq"], (float) n["depth"] };
    hasPendingNotches.store (true, std::memory_order_release);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StillaAudioProcessor(); }
