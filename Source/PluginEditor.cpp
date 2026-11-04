#include "PluginEditor.h"
#include "BinaryData.h"

using namespace juce;

// Standard sans-serif. Fontnavn som ikke finnes kan krasje tekstformingen på Linux.
static Font uiFont (float size, bool bold = false)
{
    return Font (FontOptions (size, bold ? Font::bold : Font::plain));
}

// Norske tegn må tolkes som UTF-8
static String nb (const char* text) { return String (CharPointer_UTF8 (text)); }

static String formatHz (float f)
{
    return f < 1000 ? String (roundToInt (f)) + " Hz" : String (f / 1000.0f, f < 10000 ? 2 : 1) + " kHz";
}

//==============================================================================
StillaLookAndFeel::StillaLookAndFeel()
{
    setColour (ResizableWindow::backgroundColourId, ui::bg);
    setColour (Slider::textBoxTextColourId, ui::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::rotarySliderFillColourId, ui::output);
    setColour (Label::textColourId, ui::text);
    setColour (TextButton::buttonColourId, ui::bg);
    setColour (TextButton::buttonOnColourId, ui::output);
    setColour (TextButton::textColourOffId, ui::text);
    setColour (TextButton::textColourOnId, ui::bg);
    setColour (ComboBox::outlineColourId, ui::gridHi);
}

Label* StillaLookAndFeel::createSliderTextBox (Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (uiFont (12.0f));
    l->setColour (Label::backgroundColourId, Colours::transparentBlack);
    l->setColour (Label::outlineColourId, Colours::transparentBlack);
    l->setColour (Label::textColourId, ui::text);
    return l;
}

void StillaLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s)
{
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);

    g.setColour (ui::bg);
    g.fillEllipse (c.x - r, c.y - r, r * 2, r * 2);

    Path track;
    track.addCentredArc (c.x, c.y, r - 3, r - 3, 0, start, end, true);
    g.setColour (ui::gridHi);
    g.strokePath (track, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    Path arc;
    arc.addCentredArc (c.x, c.y, r - 3, r - 3, 0, start, angle, true);
    g.setColour (s.findColour (Slider::rotarySliderFillColourId));
    g.strokePath (arc, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    g.setColour (ui::panel.brighter (0.15f));
    g.fillEllipse (c.x - r * 0.62f, c.y - r * 0.62f, r * 1.24f, r * 1.24f);
    g.setColour (ui::curve);
    g.drawLine ({ c.getPointOnCircumference (r * 0.15f, angle), c.getPointOnCircumference (r * 0.5f, angle) }, 2.0f);
}

//==============================================================================
// Spekter

SpectrumDisplay::SpectrumDisplay (StillaAudioProcessor& p) : proc (p)
{
    window.resize (kSize);
    for (int i = 0; i < kSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * MathConstants<float>::pi * (float) i / (float) kSize);
    fftData.resize (kSize * 2);
    scratch.resize (AnalyserFifo::kSize);
    inputRing.assign (kSize, 0.0f);
    outputRing.assign (kSize, 0.0f);
    setOpaque (true);
    startTimerHz (30);
}

SpectrumDisplay::~SpectrumDisplay() { stopTimer(); }

Rectangle<float> SpectrumDisplay::plot() const { return getLocalBounds().toFloat().reduced (34, 10).withTrimmedBottom (14); }

float SpectrumDisplay::freqToX (float f) const
{
    const auto r = plot();
    return r.getX() + r.getWidth() * std::log (f / 20.0f) / std::log (1000.0f);
}

// RTA: 0 dB øverst, -96 dB nederst
float SpectrumDisplay::levelToY (float db) const
{
    const auto r = plot();
    return r.getY() - db / 96.0f * r.getHeight();
}

// Notch-kurve: 0 dB litt under toppen, -36 dB nederst
float SpectrumDisplay::cutToY (float db) const
{
    const auto r = plot();
    const float top = r.getY() + 24.0f;
    return top - db / 36.0f * (r.getBottom() - top);
}

void SpectrumDisplay::resized()
{
    const int n = jmax (16, (int) plot().getWidth() / 2);
    xFreqs.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        xFreqs[(size_t) i] = 20.0f * std::pow (1000.0f, (float) i / (float) (n - 1));
    inputDisplay.assign ((size_t) n, -120.0f);
    outputDisplay.assign ((size_t) n, -120.0f);
}

// Hent nye samples, kjør FFT og glatt til ett nivå per x-punkt
void SpectrumDisplay::updateSpectrum (AnalyserFifo& fifo, std::vector<float>& ring, int& writePos, std::vector<float>& display)
{
    const int n = fifo.pop (scratch.data(), (int) scratch.size());
    for (int i = jmax (0, n - kSize); i < n; ++i)
    {
        ring[(size_t) writePos] = scratch[(size_t) i];
        writePos = (writePos + 1) & (kSize - 1);
    }

    for (int i = 0; i < kSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((writePos + i) & (kSize - 1))] * window[(size_t) i];
    std::fill (fftData.begin() + kSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const float scale = 4.0f / (float) kSize;   // Hann: en sinus på 0 dBFS vises som 0 dB
    const float binHz = (float) (proc.currentSampleRate.load() / kSize);

    for (size_t i = 0; i < xFreqs.size(); ++i)
    {
        const float f = xFreqs[i];
        // 1/6 oktav rundt frekvensen. Geometrisk snitt av gjennomsnitt og topp:
        // toner vises med riktig nivå, og støy blåses ikke opp.
        const int b0 = jlimit (1, kSize / 2, (int) (f * 0.944f / binHz));
        const int b1 = jlimit (b0, kSize / 2, (int) std::ceil (f * 1.059f / binHz));
        float sum = 0, peak = 0;
        for (int b = b0; b <= b1; ++b)
        {
            const float a = fftData[(size_t) b] * scale;
            sum += a * a;
            peak = jmax (peak, a * a);
        }
        const float power = std::sqrt (sum / (float) (b1 - b0 + 1) * peak);
        const float tilt = 4.5f * std::log2 (f / 1000.0f);   // musikk og rosa støy ser flate ut
        const float db = 10.0f * std::log10 (power + 1.0e-20f) + tilt;

        float& d = display[i];
        d += (db > d ? 0.6f : 0.15f) * (db - d);   // rask stigning, roligere fall
    }
}

void SpectrumDisplay::timerCallback()
{
    updateSpectrum (proc.inputFifo, inputRing, inputWrite, inputDisplay);
    updateSpectrum (proc.outputFifo, outputRing, outputWrite, outputDisplay);
    repaint();
}

void SpectrumDisplay::paint (Graphics& g)
{
    g.fillAll (ui::bg);
    const auto r = plot();

    // Rutenett
    g.setFont (uiFont (11.0f));
    for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
    {
        const float x = freqToX (f);
        g.setColour (ui::grid);
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
        g.setColour (ui::dim);
        g.drawText (f < 1000 ? String ((int) f) : String ((int) f / 1000) + "k", (int) x - 20, (int) r.getBottom() + 2, 40, 12, Justification::centred);
    }
    for (int db = 0; db >= -36; db -= 12)
    {
        const float y = cutToY ((float) db);
        g.setColour (ui::grid);
        g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        g.setColour (ui::dim);
        g.drawText (String (db), 0, (int) y - 6, 30, 12, Justification::centredRight);
    }

    // Skygge utenfor frekvensområdet detektoren lytter på
    const float lowX  = freqToX (proc.apvts.getRawParameterValue (ids::lowFreq)->load());
    const float highX = freqToX (proc.apvts.getRawParameterValue (ids::highFreq)->load());
    g.setColour (Colours::black.withAlpha (0.35f));
    g.fillRect (Rectangle<float> (r.getX(), r.getY(), jmax (0.0f, lowX - r.getX()), r.getHeight()));
    g.fillRect (Rectangle<float> (highX, r.getY(), jmax (0.0f, r.getRight() - highX), r.getHeight()));

    // RTA: inngang som flate, utgang som linje
    auto spectrumPath = [&] (const std::vector<float>& display, bool closed)
    {
        Path p;
        for (size_t i = 0; i < xFreqs.size(); ++i)
        {
            const float x = freqToX (xFreqs[i]);
            const float y = jlimit (r.getY(), r.getBottom(), levelToY (display[i]));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        if (closed)
        {
            p.lineTo (r.getRight(), r.getBottom());
            p.lineTo (r.getX(), r.getBottom());
            p.closeSubPath();
        }
        return p;
    };
    g.setColour (ui::dim.withAlpha (0.35f));
    g.fillPath (spectrumPath (inputDisplay, true));
    g.setColour (ui::output.withAlpha (0.9f));
    g.strokePath (spectrumPath (outputDisplay, false), PathStrokeType (1.3f));

    // Samlet notch-kurve
    const double fs = proc.currentSampleRate.load();
    const float q = proc.notchQ.load();
    Path curve;
    for (size_t i = 0; i < xFreqs.size(); ++i)
    {
        float total = 0;
        for (auto& n : proc.notches)
            if (n.kind.load() != (int) stilla::NotchKind::Free)
                total += stilla::notchResponseDb (n.freqHz.load(), n.depthDb.load(), q, xFreqs[i], fs);
        const float x = freqToX (xFreqs[i]);
        const float y = jmin (r.getBottom(), cutToY (total));
        if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
    }
    Path fill (curve);
    fill.lineTo (r.getRight(), cutToY (0));
    fill.lineTo (r.getX(), cutToY (0));
    fill.closeSubPath();
    g.setColour (ui::learn.withAlpha (0.15f));
    g.fillPath (fill);
    g.setColour (ui::curve);
    g.strokePath (curve, PathStrokeType (1.8f));

    // Markør for hver notch: trekant øverst i fargen til typen
    for (auto& n : proc.notches)
    {
        const int kind = n.kind.load();
        if (kind == (int) stilla::NotchKind::Free)
            continue;
        const float x = freqToX (n.freqHz.load());
        const auto colour = kind == (int) stilla::NotchKind::Fixed ? ui::fixed : ui::live;
        Path tri;
        tri.addTriangle (x - 5, r.getY() + 2, x + 5, r.getY() + 2, x, r.getY() + 11);
        g.setColour (colour.withAlpha (jlimit (0.3f, 1.0f, n.depthDb.load() / 6.0f)));
        g.fillPath (tri);
    }

    g.setColour (ui::gridHi);
    g.drawRect (r, 1.0f);
}

//==============================================================================
// Notch-liste

NotchList::NotchList (StillaAudioProcessor& p) : proc (p)
{
    startTimerHz (10);
}

void NotchList::paint (Graphics& g)
{
    g.fillAll (ui::panel);

    struct Row { float freq, depth, target; int kind; };
    std::vector<Row> rows;
    for (auto& n : proc.notches)
        if (n.kind.load() != (int) stilla::NotchKind::Free)
            rows.push_back ({ n.freqHz.load(), n.depthDb.load(), n.targetDb.load(), n.kind.load() });
    std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) { return a.freq < b.freq; });

    g.setColour (ui::text);
    g.setFont (uiFont (13.0f, true));
    g.drawText ("Notcher  " + String ((int) rows.size()) + "/" + String (stilla::kMaxNotches), 12, 8, getWidth() - 24, 18, Justification::left);

    g.setFont (uiFont (12.0f));
    const int rowHeight = jmax (12, (getHeight() - 36) / stilla::kMaxNotches);
    int y = 32;
    for (auto& row : rows)
    {
        const bool isFixed = row.kind == (int) stilla::NotchKind::Fixed;
        g.setColour (isFixed ? ui::fixed : ui::live);
        g.fillEllipse (12.0f, (float) y + rowHeight * 0.5f - 3.5f, 7.0f, 7.0f);
        g.setColour (ui::text);
        g.drawText (formatHz (row.freq), 26, y, 80, rowHeight, Justification::left);
        g.drawText ("-" + String (row.depth, 1) + " dB", 104, y, 60, rowHeight, Justification::right);
        g.setColour (ui::dim);
        g.drawText (isFixed ? "FAST" : "LIVE", getWidth() - 52, y, 40, rowHeight, Justification::right);
        y += rowHeight;
    }

    if (rows.empty())
    {
        g.setColour (ui::dim);
        g.drawText ("Ingen feedback funnet", 12, 32, getWidth() - 24, 18, Justification::left);
    }
}

//==============================================================================
// Hovedvindu

StillaAudioProcessorEditor::StillaAudioProcessorEditor (StillaAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p), notchList (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (spectrum);
    addAndMakeVisible (notchList);

    setupKnob (sensitivity, ids::sensitivity, "Sensitivity");
    setupKnob (detectTime,  ids::detectTime,  "Detect Time");
    setupKnob (maxDepth,    ids::maxDepth,    "Max Depth");
    setupKnob (width,       ids::width,       "Width");
    setupKnob (release,     ids::release,     "Release");
    setupKnob (lowFreq,     ids::lowFreq,     "Low");
    setupKnob (highFreq,    ids::highFreq,    "High");
    setupKnob (output,      ids::output,      "Output");

    setupToggle (learn,    ids::learn,    "Learn",     ui::learn);
    setupToggle (freeze,   ids::freeze,   "Freeze",    ui::output);
    setupToggle (holdLive, ids::holdLive, "Hold Live", ui::live);

    learn.button.setTooltip (nb ("Nye notcher blir faste. Bruk under lydsjekk for å ringe ut rommet."));
    freeze.button.setTooltip (nb ("Ingen nye notcher. De som finnes, blir stående."));
    holdLive.button.setTooltip (nb ("Live-notcher slippes aldri."));

    addAndMakeVisible (clearLive);
    addAndMakeVisible (clearAll);
    clearLive.onClick = [this] { proc.requestClear (StillaAudioProcessor::ClearLive); };
    clearAll.onClick  = [this] { proc.requestClear (StillaAudioProcessor::ClearAll); };

    setSize (1000, 620);
    startTimerHz (10);
}

StillaAudioProcessorEditor::~StillaAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void StillaAudioProcessorEditor::setupKnob (Knob& k, const String& paramId, const String& name)
{
    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 16);
    k.slider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    addAndMakeVisible (k.slider);
    k.label.setText (name, dontSendNotification);
    k.label.setJustificationType (Justification::centred);
    k.label.setFont (uiFont (12.0f));
    k.label.setColour (Label::textColourId, ui::dim);
    addAndMakeVisible (k.label);
    k.attachment = std::make_unique<SliderAttachment> (proc.apvts, paramId, k.slider);
}

void StillaAudioProcessorEditor::setupToggle (Toggle& t, const String& paramId, const String& name, Colour onColour)
{
    t.button.setButtonText (name);
    t.button.setClickingTogglesState (true);
    t.button.setColour (TextButton::buttonOnColourId, onColour);
    addAndMakeVisible (t.button);
    t.attachment = std::make_unique<ButtonAttachment> (proc.apvts, paramId, t.button);
}

void StillaAudioProcessorEditor::paint (Graphics& g)
{
    g.fillAll (ui::bg);

    static const Image logo = ImageCache::getFromMemory (BinaryData::stillalogo256_png, BinaryData::stillalogo256_pngSize);
    g.drawImage (logo, Rectangle<float> (12, 7, 26, 26), RectanglePlacement::centred);

    g.setColour (ui::curve);
    g.setFont (uiFont (19.0f, true));
    g.drawText ("Stilla", 46, 6, 80, 28, Justification::left);
    g.setColour (ui::dim);
    g.setFont (uiFont (12.0f));
    g.drawText (nb ("Feedback-demper  ·  0 ms latens"), 112, 6, 260, 28, Justification::left);

    // Status øverst til høyre
    int numLive = 0, numFixed = 0;
    for (auto& n : proc.notches)
    {
        numLive  += n.kind.load() == (int) stilla::NotchKind::Live;
        numFixed += n.kind.load() == (int) stilla::NotchKind::Fixed;
    }
    String status = String (numLive) + " live  ·  " + String (numFixed) + " faste";
    if (proc.apvts.getRawParameterValue (ids::learn)->load() > 0.5f)
    {
        g.setColour (ui::learn);
        g.setFont (uiFont (12.0f, true));
        g.drawText (nb ("LEARN: nye notcher blir faste"), getWidth() - 470, 6, 220, 28, Justification::right);
    }
    g.setColour (ui::text);
    g.setFont (uiFont (12.0f));
    g.drawText (status, getWidth() - 240, 6, 228, 28, Justification::right);

    // Panel bak kontrollene
    g.setColour (ui::panel);
    g.fillRoundedRectangle (Rectangle<float> (8, (float) getHeight() - 152, (float) getWidth() - 16, 144), 6);
}

void StillaAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (40);

    auto controls = area.removeFromBottom (160).reduced (16, 14);
    auto top = area.reduced (8, 0);
    notchList.setBounds (top.removeFromRight (220));
    top.removeFromRight (8);
    spectrum.setBounds (top);

    // Knapper til høyre, knotter til venstre
    auto buttons = controls.removeFromRight (220);
    const int bh = 30;
    auto row1 = buttons.removeFromTop (bh);
    learn.button.setBounds (row1.removeFromLeft (106));
    row1.removeFromLeft (8);
    freeze.button.setBounds (row1);
    buttons.removeFromTop (10);
    holdLive.button.setBounds (buttons.removeFromTop (bh));
    buttons.removeFromTop (10);
    auto row3 = buttons.removeFromTop (bh);
    clearLive.setBounds (row3.removeFromLeft (106));
    row3.removeFromLeft (8);
    clearAll.setBounds (row3);

    Knob* knobs[] = { &sensitivity, &detectTime, &maxDepth, &width, &release, &lowFreq, &highFreq, &output };
    const int w = controls.getWidth() / 8;
    for (auto* k : knobs)
    {
        auto cell = controls.removeFromLeft (w);
        k->label.setBounds (cell.removeFromTop (16));
        k->slider.setBounds (cell.reduced (4, 0));
    }
}
