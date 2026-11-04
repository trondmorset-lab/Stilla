#pragma once
// Låsfri ringbuffer fra lydtråden (skriver) til GUI-et (leser).
// Er bufferen full fordi GUI-et er lukket, kastes nye samples i stedet for å vente.
#include <array>
#include <atomic>
#include <algorithm>

class AnalyserFifo
{
public:
    static constexpr int kSize = 1 << 15;

    void push (const float* data, int n) noexcept
    {
        const int w = write.load (std::memory_order_relaxed);
        const int r = read.load (std::memory_order_acquire);
        const int free = kSize - 1 - ((w - r + kSize) & (kSize - 1));
        n = std::min (n, free);
        for (int i = 0; i < n; ++i)
            buffer[(size_t) ((w + i) & (kSize - 1))] = data[i];
        write.store ((w + n) & (kSize - 1), std::memory_order_release);
    }

    int pop (float* dest, int maxN) noexcept
    {
        const int r = read.load (std::memory_order_relaxed);
        const int w = write.load (std::memory_order_acquire);
        const int n = std::min (maxN, (w - r + kSize) & (kSize - 1));
        for (int i = 0; i < n; ++i)
            dest[i] = buffer[(size_t) ((r + i) & (kSize - 1))];
        read.store ((r + n) & (kSize - 1), std::memory_order_release);
        return n;
    }

private:
    std::array<float, kSize> buffer {};
    std::atomic<int> write { 0 }, read { 0 };
};
