#pragma once
// En bank med smale kutt-filtre som settes automatisk der feedback er funnet.
//
//  * Live-notcher flytter seg etter feedbacken og slippes igjen etter Release-tiden.
//  * Faste notcher (fra Learn under lydsjekk) blir stående til de slettes.
//  * Kommer feedbacken tilbake på samme sted, blir notchen dypere (opptil Max Depth).
//  * Dybden glir mykt inn og ut, så notcher kommer og går uten klikk.
#include "NotchFilter.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace stilla
{
constexpr int kMaxNotches  = 24;
constexpr int kMaxChannels = 2;

enum class NotchKind { Free = 0, Live = 1, Fixed = 2 };

struct NotchSettings
{
    float maxDepthDb  = 18.0f;
    float widthOct    = 0.1f;     // båndbredde i oktaver
    float releaseSec  = 10.0f;    // live-notcher slippes etter så lang tid uten ny feedback
    bool  holdLive    = false;    // true = live-notcher slippes aldri
};

class NotchBank
{
public:
    static constexpr float kFirstCutDb = 6.0f;   // første kutt
    static constexpr float kStepDb     = 3.0f;   // ekstra kutt hver gang feedbacken kommer tilbake

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        cutSmoothing     = smoothingCoeff (0.005);   // kutt kommer på 5 ms
        releaseSmoothing = smoothingCoeff (0.3);     // og slipper over ca. 1,5 s

        // Behold faste notcher (f.eks. etter endret sample rate), fjern live-notcher
        for (auto& n : notches)
        {
            if (n.kind == NotchKind::Live) n.kind = NotchKind::Free;
            n.currentDb = n.kind == NotchKind::Fixed ? n.targetDb : 0.0f;
            n.coeffs.setFrequency (n.freqHz, fs);
            n.coeffsDb = -1.0f;
            for (auto& st : n.state) st.reset();
        }
    }

    void setSettings (const NotchSettings& s) noexcept
    {
        if (s.widthOct != settings.widthOct)
            for (auto& n : notches) n.coeffsDb = -1.0f;   // tving ny beregning
        settings = s;
        q = qFromOctaves (s.widthOct);
    }

    // Kalles når detektoren har funnet feedback
    void addHowl (float freqHz, bool asFixed) noexcept
    {
        if (Notch* existing = findNotchNear (freqHz))
        {
            existing->targetDb = std::min (existing->targetDb + kStepDb, settings.maxDepthDb);
            existing->secondsSinceHit = 0;
            existing->releasing = false;
            if (asFixed) existing->kind = NotchKind::Fixed;
            return;
        }

        if (Notch* slot = findFreeSlot())
            startNotch (*slot, freqHz, asFixed ? NotchKind::Fixed : NotchKind::Live, std::min (kFirstCutDb, settings.maxDepthDb));
        // Ingen plass: banken er full av faste notcher
    }

    // Legg inn en fast notch direkte (fra lagret prosjekt). Dybden gjelder med en gang.
    void addFixed (float freqHz, float depthDb) noexcept
    {
        if (Notch* slot = findFreeSlot())
        {
            startNotch (*slot, freqHz, NotchKind::Fixed, std::clamp (depthDb, 0.0f, 30.0f));
            slot->currentDb = slot->targetDb;
        }
    }

    void clear (bool includeFixed) noexcept
    {
        for (auto& n : notches)
            if (n.kind == NotchKind::Live || includeFixed)
                n.kind = NotchKind::Free;
    }

    // Kalles én gang per detektor-ramme (~10 ms): teller ned til release
    void tick (double seconds) noexcept
    {
        for (auto& n : notches)
        {
            if (n.kind != NotchKind::Live || settings.holdLive)
                continue;

            n.secondsSinceHit += seconds;
            if (n.secondsSinceHit > settings.releaseSec)
                n.releasing = true;
        }
    }

    // Prosesser én sample i én kanal. Kall updateCoefficients() først for hver sample.
    float process (float x, int channel) noexcept
    {
        for (auto& n : notches)
            if (n.kind != NotchKind::Free)
                x = n.state[(size_t) channel].process (x, n.coeffs);
        return x;
    }

    // Glatter dybden og regner nye koeffisienter når den har endret seg
    void updateCoefficients() noexcept
    {
        for (auto& n : notches)
        {
            if (n.kind == NotchKind::Free)
                continue;

            const float target = n.releasing ? 0.0f : n.targetDb;
            const float coeff  = target > n.currentDb ? cutSmoothing : releaseSmoothing;
            n.currentDb = target + coeff * (n.currentDb - target);

            if (n.releasing && n.currentDb < 0.05f)
            {
                n.kind = NotchKind::Free;   // ferdig sluppet
                continue;
            }

            if (std::abs (n.currentDb - n.coeffsDb) > 0.01f)
            {
                n.coeffs.setDepth (n.currentDb, q);
                n.coeffsDb = n.currentDb;
            }
        }
    }

    // Til GUI og lagring
    NotchKind getKind (int i) const noexcept    { return notches[(size_t) i].kind; }
    float getFreqHz (int i) const noexcept      { return notches[(size_t) i].freqHz; }
    float getDepthDb (int i) const noexcept     { return notches[(size_t) i].currentDb; }
    float getTargetDb (int i) const noexcept    { return notches[(size_t) i].targetDb; }
    float getQ() const noexcept                 { return q; }

private:
    struct Notch
    {
        NotchKind kind = NotchKind::Free;
        float freqHz = 1000;
        float targetDb = 0;      // ønsket kutt
        float currentDb = 0;     // glattet kutt som brukes nå
        float coeffsDb = -1;     // kuttet koeffisientene sist ble regnet for
        double secondsSinceHit = 0;
        bool releasing = false;
        NotchCoeffs coeffs;
        std::array<NotchState, kMaxChannels> state;
    };

    void startNotch (Notch& n, float freqHz, NotchKind kind, float depthDb) noexcept
    {
        n.kind = kind;
        n.freqHz = freqHz;
        n.targetDb = depthDb;
        n.currentDb = 0;
        n.secondsSinceHit = 0;
        n.releasing = false;
        n.coeffs.setFrequency (freqHz, fs);
        n.coeffsDb = -1.0f;
        for (auto& st : n.state) st.reset();
    }

    float smoothingCoeff (double seconds) const noexcept
    {
        return (float) std::exp (-1.0 / (seconds * fs));
    }

    // En aktiv notch som allerede dekker frekvensen (innenfor halve båndbredden)
    Notch* findNotchNear (float freqHz) noexcept
    {
        for (auto& n : notches)
            if (n.kind != NotchKind::Free && std::abs (std::log2 (freqHz / n.freqHz)) < settings.widthOct * 0.5f)
                return &n;
        return nullptr;
    }

    // Ledig plass, ellers den live-notchen som har vært stille lengst
    Notch* findFreeSlot() noexcept
    {
        Notch* oldestLive = nullptr;
        for (auto& n : notches)
        {
            if (n.kind == NotchKind::Free)
                return &n;
            if (n.kind == NotchKind::Live && (oldestLive == nullptr || n.secondsSinceHit > oldestLive->secondsSinceHit))
                oldestLive = &n;
        }
        return oldestLive;
    }

    double fs = 48000.0;
    float cutSmoothing = 0, releaseSmoothing = 0;
    NotchSettings settings;
    float q = qFromOctaves (0.1f);
    std::array<Notch, kMaxNotches> notches {};
};
} // namespace stilla
