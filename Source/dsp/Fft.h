#pragma once
// Enkel radix-2 FFT (kompleks, på stedet).
// Tabellene lages i prepare(), så forward() allokerer aldri og kan kjøres i lydtråden.
#include "Common.h"
#include <complex>
#include <vector>
#include <cmath>

namespace stilla
{
class Fft
{
public:
    // size må være en potens av 2
    void prepare (int size)
    {
        n = size;
        twiddles.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
        {
            const double angle = -2.0 * kPi * i / n;
            twiddles[(size_t) i] = { (float) std::cos (angle), (float) std::sin (angle) };
        }

        // Bit-reversert rekkefølge for ombytting før butterfly-stegene
        bitReversed.resize ((size_t) n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitReversed[(size_t) i] = r;
        }
    }

    int size() const noexcept { return n; }

    void forward (std::complex<float>* data) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = bitReversed[(size_t) i];
            if (j > i) std::swap (data[i], data[j]);
        }

        for (int len = 2; len <= n; len *= 2)
        {
            const int half = len / 2;
            const int step = n / len;
            for (int start = 0; start < n; start += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    const auto t = twiddles[(size_t) (k * step)] * data[start + k + half];
                    data[start + k + half] = data[start + k] - t;
                    data[start + k] += t;
                }
            }
        }
    }

private:
    int n = 0;
    std::vector<std::complex<float>> twiddles;
    std::vector<int> bitReversed;
};
} // namespace stilla
