// Tester for DSP-kjernen (uten JUCE). Bygg: g++ -O2 -std=c++17 -I Source Tests/DspTest.cpp
//
// Viktigste test: en simulert feedback-sløyfe (mikrofon → plugin → høyttaler → rom → mikrofon).
// Den måler hvor mye mer gain man får før feedback med Stilla enn uten.
#include "dsp/FeedbackSuppressor.h"
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>
#include <functional>

using namespace stilla;

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  %s %s\n", ok ? "[OK]  " : "[FEIL]", what);
    if (! ok) ++failures;
}

static float fromDb (float db) { return std::pow (10.0f, db / 20.0f); }

// Rosa støy (Paul Kellets filter), ca. -10 dBFS RMS ved amplitude 1
struct Pink
{
    std::mt19937 rng { 1 };
    std::uniform_real_distribution<float> uni { -1.0f, 1.0f };
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    float next()
    {
        const float w = uni (rng);
        b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
        const float out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
        b6 = w * 0.115926f;
        return out * 0.11f;
    }
};

static int countNotches (const NotchBank& bank, NotchKind kind)
{
    int n = 0;
    for (int i = 0; i < kMaxNotches; ++i) n += bank.getKind (i) == kind;
    return n;
}

static int countActive (const NotchBank& bank)
{
    return countNotches (bank, NotchKind::Live) + countNotches (bank, NotchKind::Fixed);
}

// Kjører et monosignal gjennom pluginen i blokker på 64 samples
static void runMono (FeedbackSuppressor& fs, double seconds, double rate, const std::function<float (long)>& source)
{
    float block[64];
    float* ch[1] = { block };
    const long total = (long) (seconds * rate);
    for (long start = 0; start < total; start += 64)
    {
        for (int i = 0; i < 64; ++i) block[i] = source (start + i);
        fs.process (ch, 1, 64);
    }
}

//==============================================================================
// Feedback-sløyfe: høyttaler → (forsinkelse + romrespons) × gain → mikrofon
struct FeedbackLoop
{
    static constexpr double fs = 48000.0;
    static constexpr int delaySamples = 480;   // 10 ms: ca. 3,4 m fra høyttaler til mikrofon
    static constexpr int irLength = 2048;
    std::vector<float> ir;                     // romrespons, normalisert til topp 0 dB
    std::vector<float> history;                // høyttalersignal, dobbel lengde for rask konvolusjon

    explicit FeedbackLoop (unsigned seed)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<float> gauss;
        ir.resize (irLength);
        for (int k = 0; k < irLength; ++k)
            ir[(size_t) k] = gauss (rng) * std::exp (-(float) k / (0.015f * (float) fs));
        ir[0] += 3.0f;   // direktelyd

        // Høyttaler, mikrofon og luft demper diskant og dypbass: lavpass ~5 kHz og høypass ~120 Hz
        const float lp = std::exp (-2.0f * (float) kPi * 5000.0f / (float) fs);
        const float hp = std::exp (-2.0f * (float) kPi * 120.0f / (float) fs);
        float lpState = 0, hpState = 0;
        for (auto& v : ir)
        {
            lpState = v + lp * (lpState - v);
            hpState = lpState + hp * (hpState - lpState);
            v = lpState - hpState;
        }

        // Finn største forsterkning i frekvens (8192-punkts FFT) og normaliser til 1
        Fft fft; fft.prepare (8192);
        std::vector<std::complex<float>> spec (8192);
        for (int k = 0; k < irLength; ++k) spec[(size_t) k] = ir[(size_t) k];
        fft.forward (spec.data());
        float maxMag = 0;
        for (int k = 0; k < 4096; ++k) maxMag = std::max (maxMag, std::abs (spec[(size_t) k]));
        for (auto& v : ir) v /= maxMag;
    }

    // Kjører sløyfen. Returnerer høyttalernivå de siste 2 sekundene relativt til kilden (dB).
    float run (float loopGainDb, FeedbackSuppressor* plugin, double seconds, int* notchesOut = nullptr)
    {
        const int span = delaySamples + irLength;
        history.assign ((size_t) span * 2, 0.0f);
        int pos = 0;   // skriveposisjon i history

        Pink pink;
        const float g = fromDb (loopGainDb);
        float block[64];
        float* ch[1] = { block };
        double srcPower = 0, outPower = 0;
        const long total = (long) (seconds * fs), measureFrom = total - (long) (2 * fs);

        for (long start = 0; start < total; start += 64)
        {
            // Mikrofon = kilde (snakk: rosa støy med stavelsesrytme) + tilbakekobling
            for (int i = 0; i < 64; ++i)
            {
                const double t = (double) (start + i) / fs;
                const float syllables = 0.5f + 0.5f * (float) std::sin (2 * kPi * 4.0 * t);
                const float src = 0.1f * pink.next() * syllables;

                // Konvolusjon med romresponsen. Siste høyttalersample ligger på history[pos + span - 1].
                const float* h = &history[(size_t) (pos + i + span - delaySamples - 1)];
                float fb = 0;
                for (int k = 0; k < irLength; ++k) fb += ir[(size_t) k] * h[-k];

                block[i] = src + g * fb;
                if (start + i >= measureFrom) srcPower += (double) src * src;
            }

            if (plugin) plugin->process (ch, 1, 64);

            // Høyttaler med myk metning (som en forsterker)
            for (int i = 0; i < 64; ++i)
            {
                const float out = std::tanh (block[i]);
                const int p = (pos + i) % span;
                history[(size_t) p] = history[(size_t) (p + span)] = out;
                if (start + i >= measureFrom) outPower += (double) out * out;
            }
            pos = (pos + 64) % span;
        }

        if (notchesOut && plugin) *notchesOut = countActive (plugin->getBank());
        return 10.0f * (float) std::log10 (outPower / srcPower);
    }
};

// Høyeste stabile gain (1 dB-steg): høyttalernivået holder seg under +12 dB over kilden
static float maxStableGain (FeedbackLoop& loop, const Settings* settings, int* notches = nullptr)
{
    float lastStable = -99;
    for (float gain = -6.0f; gain <= 24.0f; gain += 1.0f)
    {
        FeedbackSuppressor fs;
        FeedbackSuppressor* plugin = nullptr;
        if (settings)
        {
            fs.prepare (FeedbackLoop::fs);
            fs.setSettings (*settings);
            plugin = &fs;
        }
        int n = 0;
        const float level = loop.run (gain, plugin, 8.0, &n);
        if (level > 12.0f) break;
        lastStable = gain;
        if (notches) *notches = n;
    }
    return lastStable;
}

//==============================================================================
int main()
{
    const double fs = 48000.0;
    char msg[256];

    std::printf ("\nNotch-filter\n");
    {
        // Mål responsen ved senter og ved en oktav unna og sammenlign med formelen
        const float fc = 1000, cut = 12, q = qFromOctaves (0.1f);
        float maxErr = 0;
        for (float f : { 500.0f, 950.0f, 1000.0f, 1035.0f, 2000.0f })
        {
            NotchCoeffs c; c.setFrequency (fc, fs); c.setDepth (cut, q);
            NotchState st;
            double inP = 0, outP = 0;
            for (int i = 0; i < 96000; ++i)
            {
                const float x = (float) std::sin (2 * kPi * f * i / fs);
                const float y = st.process (x, c);
                if (i > 48000) { inP += x * x; outP += y * y; }
            }
            const float measured = 10.0f * (float) std::log10 (outP / inP);
            maxErr = std::max (maxErr, std::abs (measured - notchResponseDb (fc, cut, q, f, fs)));
        }
        std::snprintf (msg, sizeof msg, "Målt respons = formel (maks avvik %.3f dB), senter -12 dB, Q %.1f for 1/10 oktav", maxErr, q);
        check (maxErr < 0.05f && std::abs (notchResponseDb (fc, cut, q, fc, fs) + 12.0f) < 0.01f, msg);
    }

    std::printf ("\nTransparens\n");
    {
        FeedbackSuppressor s; s.prepare (fs); s.setSettings ({});
        float x[256], ref[256]; float* ch[1] = { x };
        std::mt19937 rng (3); std::uniform_real_distribution<float> uni (-0.1f, 0.1f);
        float maxErr = 0;
        for (int b = 0; b < 200; ++b)
        {
            for (int i = 0; i < 256; ++i) x[i] = ref[i] = uni (rng);
            s.process (ch, 1, 256);
            for (int i = 0; i < 256; ++i) maxErr = std::max (maxErr, std::abs (x[i] - ref[i]));
        }
        check (maxErr == 0.0f && countActive (s.getBank()) == 0, "Uten feedback: utgang er bit-identisk med inngang, 0 samples latens");
    }

    std::printf ("\nDeteksjon av voksende tone\n");
    {
        FeedbackSuppressor s; s.prepare (fs); s.setSettings ({});
        Pink pink;
        const float howlHz = 2345.0f;
        // Tonen vokser 40 dB/s fra -70 dBFS i rosa støy på ca. -40 dBFS
        long detectedAt = -1;
        float block[64]; float* ch[1] = { block };
        for (long start = 0; start < (long) (3 * fs) && detectedAt < 0; start += 64)
        {
            for (int i = 0; i < 64; ++i)
            {
                const double t = (double) (start + i) / fs;
                block[i] = 0.03f * pink.next() + fromDb (-70.0f + 40.0f * (float) t) * (float) std::sin (2 * kPi * howlHz * t);
            }
            s.process (ch, 1, 64);
            if (countActive (s.getBank()) > 0) detectedAt = start;
        }
        float freqErr = 100;
        for (int i = 0; i < kMaxNotches; ++i)
            if (s.getBank().getKind (i) != NotchKind::Free)
                freqErr = 100.0f * std::abs (s.getBank().getFreqHz (i) - howlHz) / howlHz;
        const float levelAtDetect = -70.0f + 40.0f * (float) (detectedAt / fs);
        std::snprintf (msg, sizeof msg, "Funnet etter %.0f ms ved %.1f dBFS, frekvensfeil %.2f %%",
                       detectedAt / fs * 1000.0, levelAtDetect, freqErr);
        check (detectedAt > 0 && freqErr < 0.3f && levelAtDetect < -30.0f, msg);
    }

    std::printf ("\nIngen feiltreff på vanlig lyd (20 s hver)\n");
    {
        auto falsePositives = [&] (const std::function<float (long)>& source)
        {
            FeedbackSuppressor s; s.prepare (fs); s.setSettings ({});
            int created = 0;
            float block[64]; float* ch[1] = { block };
            for (long start = 0; start < (long) (20 * fs); start += 64)
            {
                for (int i = 0; i < 64; ++i) block[i] = source (start + i);
                const int before = countActive (s.getBank());
                s.process (ch, 1, 64);
                created += std::max (0, countActive (s.getBank()) - before);
            }
            return created;
        };

        Pink pink;
        int n = falsePositives ([&] (long) { return 0.3f * pink.next(); });
        std::snprintf (msg, sizeof msg, "Rosa støy: %d notcher", n);
        check (n == 0, msg);

        // Sang: grunntone med 12 overtoner, vibrato (5,5 Hz, ±30 cent) og nye toner hvert 0,6 s
        double phase = 0;
        n = falsePositives ([&] (long i)
        {
            const double t = i / fs;
            const double notes[] = { 196, 220, 247, 262, 294, 330, 349, 392 };
            const double f0 = notes[(long) (t / 0.6) % 8] * std::pow (2.0, 0.025 * std::sin (2 * kPi * 5.5 * t));
            phase += 2 * kPi * f0 / fs;
            float v = 0;
            for (int h = 1; h <= 12; ++h) v += (float) std::sin (h * phase) * 0.3f / (float) h;
            return v + 0.01f * pink.next();
        });
        std::snprintf (msg, sizeof msg, "Sang med vibrato: %d notcher", n);
        check (n == 0, msg);

        // Tale: overtonerik lyd med glidende tonehøyde og stavelser
        phase = 0;
        n = falsePositives ([&] (long i)
        {
            const double t = i / fs;
            const double f0 = 140 + 40 * std::sin (2 * kPi * 0.7 * t) + 15 * std::sin (2 * kPi * 3.1 * t);
            phase += 2 * kPi * f0 / fs;
            const float syll = (float) std::max (0.0, std::sin (2 * kPi * 4.0 * t));
            float v = 0;
            for (int h = 1; h <= 20; ++h) v += (float) std::sin (h * phase) * 0.25f / (float) h;
            return syll * v + 0.05f * pink.next() * (1.0f - syll);
        });
        std::snprintf (msg, sizeof msg, "Tale med glidende tonehøyde: %d notcher", n);
        check (n == 0, msg);
    }

    std::printf ("\nLearn, release og sletting\n");
    {
        Settings st; st.notches.releaseSec = 2.0f;
        FeedbackSuppressor s; s.prepare (fs);
        auto tone = [&] (float hz, float db) { return [=] (long i) { return fromDb (db) * (float) std::sin (2 * kPi * hz * i / fs); }; };

        st.learn = true;  s.setSettings (st);
        runMono (s, 1.0, fs, tone (500, -20));      // innringing under lydsjekk
        st.learn = false; s.setSettings (st);
        runMono (s, 1.0, fs, tone (3150, -20));     // feedback under konserten
        const int fixedBefore = countNotches (s.getBank(), NotchKind::Fixed);
        const int liveBefore  = countNotches (s.getBank(), NotchKind::Live);
        runMono (s, 5.0, fs, [] (long) { return 0.0f; });   // stille i 5 s
        const int fixedAfter = countNotches (s.getBank(), NotchKind::Fixed);
        const int liveAfter  = countNotches (s.getBank(), NotchKind::Live);
        std::snprintf (msg, sizeof msg, "Fast notch står (%d → %d), live-notch slippes etter release (%d → %d)",
                       fixedBefore, fixedAfter, liveBefore, liveAfter);
        check (fixedBefore == 1 && fixedAfter == 1 && liveBefore == 1 && liveAfter == 0, msg);

        st.freeze = true; s.setSettings (st);
        runMono (s, 1.0, fs, tone (5000, -20));
        check (countActive (s.getBank()) == 1, "Freeze: ingen nye notcher");

        s.getBank().clear (false);
        const bool keptFixed = countNotches (s.getBank(), NotchKind::Fixed) == 1;
        s.getBank().clear (true);
        check (keptFixed && countActive (s.getBank()) == 0, "Clear Live beholder faste, Clear All fjerner alt");

        // Fast notch fra lagret prosjekt
        s.getBank().addFixed (812.0f, 9.0f);
        bool found = false;
        for (int i = 0; i < kMaxNotches; ++i)
            found = found || (s.getBank().getKind (i) == NotchKind::Fixed && s.getBank().getFreqHz (i) == 812.0f
                              && s.getBank().getDepthDb (i) == 9.0f);
        check (found, "Fast notch lastes inn med riktig frekvens og dybde");
    }

    std::printf ("\nFeedback-sløyfe: gain før feedback (8 s per steg)\n");
    {
        float improvementSum = 0;
        float worst = 99;
        for (unsigned room : { 11u, 22u, 33u })
        {
            FeedbackLoop loop (room);
            const float without = maxStableGain (loop, nullptr);
            Settings st;
            int notches = 0;
            const float with = maxStableGain (loop, &st, &notches);
            std::printf ("    Rom %u: uten %+.0f dB, med Stilla %+.0f dB (%d notcher) → %+.0f dB\n",
                         room, without, with, notches, with - without);
            improvementSum += with - without;
            worst = std::min (worst, with - without);
        }
        std::snprintf (msg, sizeof msg, "Snitt %.1f dB mer gain før feedback (minst %.0f dB)", improvementSum / 3, worst);
        check (worst >= 4.0f, msg);
    }

    std::printf ("\nCPU\n");
    {
        FeedbackSuppressor s; s.prepare (fs); s.setSettings ({});
        for (int i = 0; i < kMaxNotches; ++i) s.getBank().addFixed (200.0f * (float) (i + 1), 12.0f);
        std::vector<float> l (512), r (512);
        float* ch[2] = { l.data(), r.data() };
        Pink pink;
        const int blocks = (int) (20 * fs / 512);
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 512; ++i) l[(size_t) i] = r[(size_t) i] = pink.next();
            s.process (ch, 2, 512);
        }
        const double used = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::snprintf (msg, sizeof msg, "Stereo, 48 kHz, alle %d notcher aktive: %.1f %% av én kjerne", kMaxNotches, 100.0 * used / 20.0);
        check (used / 20.0 < 0.05, msg);
    }

    std::printf (failures == 0 ? "\nALLE TESTER BESTÅTT\n" : "\n%d TESTER FEILET\n", failures);
    return failures == 0 ? 0 : 1;
}
