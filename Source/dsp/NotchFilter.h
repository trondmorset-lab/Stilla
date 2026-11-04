#pragma once
// Smalt kutt-filter (bell med negativ gain) basert på Simpers trapesintegrerte SVF.
// Filteret tåler at dybden endres hver sample uten klikk, så notcher kan tones inn og ut mykt.
#include "Common.h"
#include <cmath>
#include <algorithm>

namespace stilla
{
// Q fra båndbredde i oktaver (f.eks. 0,1 = 1/10 oktav)
inline float qFromOctaves (float octaves) noexcept
{
    const double r = std::pow (2.0, (double) octaves);
    return (float) (std::sqrt (r) / (r - 1.0));
}

struct NotchCoeffs
{
    float g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0, m1 = 0;

    // Frekvens og Q endres sjelden, dybden ofte. Derfor to funksjoner.
    void setFrequency (float freqHz, double sampleRate) noexcept
    {
        g = (float) std::tan (kPi * std::min ((double) freqHz, sampleRate * 0.49) / sampleRate);
    }

    void setDepth (float cutDb, float q) noexcept   // cutDb er positiv: 12 = 12 dB kutt
    {
        const float A = std::pow (10.0f, -cutDb / 40.0f);   // A² = lineær gain i senter
        k  = 1.0f / (q * A);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        m1 = k * (A * A - 1.0f);
    }
};

struct NotchState
{
    float ic1 = 0, ic2 = 0;

    void reset() noexcept { ic1 = ic2 = 0; }

    float process (float x, const NotchCoeffs& c) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;      // båndpass
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return x + c.m1 * v1;                         // inngang minus en del av båndpasset
    }
};

// Eksakt respons i dB for GUI-kurven og testene.
// SVF-en tilsvarer den analoge prototypen ved s = j·tan(πf/fs)/tan(πfc/fs).
inline float notchResponseDb (float fc, float cutDb, float q, float f, double fs) noexcept
{
    const double w = std::tan (kPi * std::min ((double) f,  fs * 0.49) / fs)
                   / std::tan (kPi * std::min ((double) fc, fs * 0.49) / fs);
    const double A = std::pow (10.0, -cutDb / 40.0);
    const double k = 1.0 / q;
    const double re = 1.0 - w * w;
    const double num = re * re + (w * A * k) * (w * A * k);
    const double den = re * re + (w * k / A) * (w * k / A);
    return (float) (10.0 * std::log10 (num / den));
}
} // namespace stilla
