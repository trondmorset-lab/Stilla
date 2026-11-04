#pragma once
// Finner feedback (hyling) i et lydsignal.
//
// Feedback er en ren tone som står stille i frekvens og bygger seg opp eller holder seg
// jevn. Stemmer og instrumenter har overtoner, vibrato og varierende nivå. Detektoren
// bruker derfor tre kjennetegn fra forskningen på feedback-undertrykking:
//
//  1. Toppen stikker langt opp over spekteret rundt (peak-to-neighbour).
//  2. Toppen mangler overtoner og undertoner (peak-to-harmonic).
//  3. Toppen står på samme frekvens over tid uten å falle i nivå (persistens).
//
// Analysen kjøres med FFT i lydtråden hvert ~10 ms. Alt minne settes av i prepare().
#include "Fft.h"
#include <array>
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>

namespace stilla
{
struct DetectorSettings
{
    float sensitivity  = 50.0f;    // 0–100 %: høyere fanger feedback tidligere, men gir flere feiltreff
    float detectTimeMs = 150.0f;   // hvor lenge en tone må stå før den regnes som feedback
    float lowHz  = 80.0f;
    float highHz = 12000.0f;
};

struct Howl
{
    float freqHz  = 0;
    float levelDb = 0;
};

class HowlDetector
{
public:
    static constexpr int kMaxHowlsPerFrame = 4;

    void prepare (double sampleRate)
    {
        fs = sampleRate;

        // Ca. 85 ms analysevindu (4096 punkter ved 48 kHz) gir ~12 Hz oppløsning
        fftSize = 1024;
        while (fftSize < (int) (sampleRate * 0.085)) fftSize *= 2;
        hopSize = fftSize / 8;

        fft.prepare (fftSize);
        input.assign ((size_t) fftSize, 0.0f);
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * i / fftSize);
        spectrum.resize ((size_t) fftSize);
        levelDb.assign ((size_t) fftSize / 2 + 1, -150.0f);
        reset();
    }

    void reset() noexcept
    {
        std::fill (input.begin(), input.end(), 0.0f);
        writePos = 0;
        samplesUntilAnalysis = hopSize;
        for (auto& t : tracks) t = {};
        numHowls = 0;
    }

    // Legg inn én sample. Returnerer true når en ny analyse er gjort.
    bool push (float x, const DetectorSettings& settings) noexcept
    {
        input[(size_t) writePos] = x;
        writePos = (writePos + 1) % fftSize;

        if (--samplesUntilAnalysis > 0)
            return false;

        samplesUntilAnalysis = hopSize;
        analyse (settings);
        return true;
    }

    // Feedback funnet i siste analyse
    int getNumHowls() const noexcept           { return numHowls; }
    const Howl& getHowl (int i) const noexcept { return howls[(size_t) i]; }

    double getFrameSeconds() const noexcept { return hopSize / fs; }

private:
    struct Track
    {
        bool  active = false;
        float bin = 0;          // interpolert frekvens i bins
        float minBin = 0, maxBin = 0;   // frekvensområdet sporet har vært innom
        float maxDb = -150;
        float lastDb = -150;
        int   frames = 0;
    };

    void analyse (const DetectorSettings& s) noexcept
    {
        computeSpectrum();

        // Terskler fra Sensitivity: 0 % = strengt, 100 % = følsomt
        const float sens        = std::clamp (s.sensitivity, 0.0f, 100.0f) / 100.0f;
        const float neighbourDb = 30.0f - 16.0f * sens;   // 30 → 14 dB over naboene
        const float harmonicDb  = 30.0f - 12.0f * sens;   // 30 → 18 dB over over- og undertoner
        const float floorDb     = -60.0f - 20.0f * sens;  // ignorer svakere topper enn dette
        const int   framesNeeded = std::max (2, (int) std::lround (s.detectTimeMs * 0.001 / getFrameSeconds()));

        const float binHz = (float) (fs / fftSize);
        const int lo = std::max (12, (int) (s.lowHz / binHz));
        const int hi = std::min (fftSize / 2 - 12, (int) (s.highHz / binHz));

        // 1. Finn kandidater: lokale topper som oppfyller de spektrale kravene
        std::array<float, kMaxCandidates> candBin {};
        std::array<float, kMaxCandidates> candDb {};
        int numCand = 0;

        for (int k = lo; k <= hi && numCand < kMaxCandidates; ++k)
        {
            const float p = levelDb[(size_t) k];
            if (p < floorDb || ! isLocalPeak (k))
                continue;
            if (p - neighbourAverage (k) < neighbourDb)
                continue;
            if (p - strongestHarmonic (k) < harmonicDb)
                continue;

            candBin[(size_t) numCand] = (float) k + peakOffset (k);
            candDb[(size_t) numCand]  = p;
            ++numCand;
        }

        // 2. Følg kandidatene over tid
        for (auto& t : tracks)
            t.active = t.active && matchAndUpdate (t, candBin.data(), candDb.data(), numCand);

        for (int c = 0; c < numCand; ++c)
            if (candBin[(size_t) c] > 0)      // ikke brukt av et eksisterende spor
                startTrack (candBin[(size_t) c], candDb[(size_t) c]);

        // 3. Spor som har stått lenge nok, uten å falle og uten å flytte seg, er feedback
        numHowls = 0;
        for (auto& t : tracks)
        {
            if (! t.active || t.frames < framesNeeded)
                continue;

            // Feedback står helt stille i frekvens. Vibrato flytter seg mer enn en halv bin (~6 Hz).
            const bool steadyFrequency = t.maxBin - t.minBin < 0.5f;
            const bool notFalling      = t.lastDb > t.maxDb - 3.0f;

            if (steadyFrequency && notFalling && numHowls < kMaxHowlsPerFrame)
                howls[(size_t) numHowls++] = { t.bin * binHz, t.lastDb };

            // Start tellingen på nytt. Står tonen fortsatt, meldes den igjen om detectTime.
            t.frames = 0;
            t.minBin = t.maxBin = t.bin;
            t.maxDb = t.lastDb;
        }
    }

    void computeSpectrum() noexcept
    {
        // Eldste sample først, med Hann-vindu
        for (int i = 0; i < fftSize; ++i)
            spectrum[(size_t) i] = { input[(size_t) ((writePos + i) % fftSize)] * window[(size_t) i], 0.0f };

        fft.forward (spectrum.data());

        // Nivå i dBFS: en sinus med amplitude 1 gir 0 dB (Hann-vinduet halverer amplituden)
        const float scale = 4.0f / (float) fftSize;
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            const float amplitude = std::abs (spectrum[(size_t) k]) * scale;
            levelDb[(size_t) k] = 20.0f * std::log10 (amplitude + 1.0e-9f);
        }
    }

    bool isLocalPeak (int k) const noexcept
    {
        const float p = levelDb[(size_t) k];
        return p > levelDb[(size_t) k - 1] && p >= levelDb[(size_t) k + 1]
            && p > levelDb[(size_t) k - 2] && p >= levelDb[(size_t) k + 2];
    }

    // Snittnivå 3–10 bins unna (nærmeste bins er en del av selve toppen pga. vinduet)
    float neighbourAverage (int k) const noexcept
    {
        float sum = 0;
        for (int d = 3; d <= 10; ++d)
            sum += levelDb[(size_t) (k - d)] + levelDb[(size_t) (k + d)];
        return sum / 16.0f;
    }

    // Sterkeste nivå ved 0,5×, 1,5× og 2× frekvensen.
    // 3× er utelatt med vilje: feedback som metter en forsterker får 3. harmonisk,
    // og skal fortsatt kjennes igjen.
    float strongestHarmonic (int k) const noexcept
    {
        float strongest = -150.0f;
        for (float ratio : { 0.5f, 1.5f, 2.0f })
        {
            const int centre = (int) std::lround (k * ratio);
            for (int b = centre - 1; b <= centre + 1; ++b)
                if (b > 1 && b < fftSize / 2)
                    strongest = std::max (strongest, levelDb[(size_t) b]);
        }
        return strongest;
    }

    // Parabolsk interpolasjon gir frekvensen mellom to bins (-0,5 til 0,5)
    float peakOffset (int k) const noexcept
    {
        const float a = levelDb[(size_t) k - 1], b = levelDb[(size_t) k], c = levelDb[(size_t) k + 1];
        const float denom = a - 2.0f * b + c;
        return denom < 0.0f ? std::clamp (0.5f * (a - c) / denom, -0.5f, 0.5f) : 0.0f;
    }

    // Finner kandidaten nærmest sporet. Brukte kandidater merkes med bin = 0.
    static bool matchAndUpdate (Track& t, float* candBin, const float* candDb, int numCand) noexcept
    {
        int best = -1;
        float bestDist = 1.5f;
        for (int c = 0; c < numCand; ++c)
        {
            const float d = std::abs (candBin[c] - t.bin);
            if (candBin[c] > 0 && d < bestDist) { best = c; bestDist = d; }
        }
        if (best < 0)
            return false;   // tonen forsvant, sporet avsluttes

        t.bin = candBin[best];
        t.minBin = std::min (t.minBin, t.bin);
        t.maxBin = std::max (t.maxBin, t.bin);
        t.lastDb = candDb[best];
        t.maxDb = std::max (t.maxDb, t.lastDb);
        ++t.frames;
        candBin[best] = 0;
        return true;
    }

    void startTrack (float bin, float db) noexcept
    {
        for (auto& t : tracks)
        {
            if (! t.active)
            {
                t = { true, bin, bin, bin, db, db, 1 };
                return;
            }
        }
    }

    static constexpr int kMaxCandidates = 24;
    static constexpr int kMaxTracks = 32;

    double fs = 48000.0;
    int fftSize = 4096, hopSize = 512;
    int writePos = 0, samplesUntilAnalysis = 512;

    Fft fft;
    std::vector<float> input, window, levelDb;
    std::vector<std::complex<float>> spectrum;

    std::array<Track, kMaxTracks> tracks {};
    std::array<Howl, kMaxHowlsPerFrame> howls {};
    int numHowls = 0;
};
} // namespace stilla
