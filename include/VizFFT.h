#pragma once
// VizFFT - self-contained radix-2 iterative complex FFT for the spectrum
// visualizer (Slice C). Replaces the stride-4 DFT in computeVizBins(), which
// aliased everything above ~sr/8 (~5.5 kHz @ 44.1k) - the top ~10 of 64 bars
// were noise-derived, not signal. One N-point FFT per frame gives honest,
// alias-free bins clear to Nyquist AND is far cheaper (N log N vs N^2 * bins).
//
// Deliberately dependency-free (miniaudio/FLAC/FDK are not for this) and
// allocation-free after construction: bit-reversal table, twiddle factors,
// and work buffers are all fixed-size members, because magnitude() runs on
// the UI thread once per frame (hot path - no per-frame allocation allowed).
// Proven in isolation by tests/viz_fft_test.cpp (DC / pure-sine bin placement /
// 100 Hz-20 kHz sweep alias check / Parseval) BEFORE it touched the viz.
//
// Plain complex FFT of a real signal (imaginary part zero) - correct, and
// already orders of magnitude cheaper than the old DFT; the packed real-FFT
// halving is a deliberate non-goal until the frame budget asks for it.

#include <array>
#include <cmath>

template <int N>
class VizFFT {
    static_assert(N > 1 && (N & (N - 1)) == 0,
                  "radix-2 FFT requires a power-of-two size");

public:
    static constexpr int kBins = N / 2;   // usable spectrum: k in [0, N/2)

    VizFFT() {
        // Bit count for N (N = 2^bits).
        int bits = 0;
        while ((1 << bits) < N) ++bits;

        // Bit-reversal permutation (input reorder for in-place DIT).
        for (int i = 0; i < N; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            brev_[i] = static_cast<unsigned short>(r);
        }

        // Twiddle factors e^(-2*pi*i*k/N), k in [0, N/2). Computed once in
        // double, stored as float - the table is the precision anchor for
        // every butterfly, so don't accumulate float error building it.
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        for (int k = 0; k < N / 2; ++k) {
            const double a = kTwoPi * k / N;
            tw_re_[k] = static_cast<float>(std::cos(a));
            tw_im_[k] = static_cast<float>(-std::sin(a));
        }
    }

    // Magnitude spectrum of a real input block: out[k] = |X[k]| for k in
    // [0, N/2), where bin k is frequency k * sr / N. Window the input before
    // calling if leakage control is wanted (the viz applies Hann upstream).
    // No allocation; safe to call every frame.
    void magnitude(const float* in, float* out /* kBins */) {
        // Load in bit-reversed order (imaginary part zero: real signal).
        for (int i = 0; i < N; ++i) {
            re_[i] = in[brev_[i]];
            im_[i] = 0.0f;
        }

        // Iterative DIT butterflies: stages len = 2, 4, ..., N.
        for (int len = 2; len <= N; len <<= 1) {
            const int half = len >> 1;
            const int step = N / len;   // twiddle-table stride for this stage
            for (int base = 0; base < N; base += len) {
                for (int j = 0; j < half; ++j) {
                    const int   t  = j * step;
                    const float wr = tw_re_[t], wi = tw_im_[t];
                    const int   a  = base + j, b = a + half;
                    const float tr = re_[b] * wr - im_[b] * wi;
                    const float ti = re_[b] * wi + im_[b] * wr;
                    re_[b] = re_[a] - tr;
                    im_[b] = im_[a] - ti;
                    re_[a] += tr;
                    im_[a] += ti;
                }
            }
        }

        for (int k = 0; k < kBins; ++k)
            out[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
    }

private:
    std::array<unsigned short, N> brev_;      // bit-reversal permutation
    std::array<float, N / 2>      tw_re_, tw_im_;   // twiddles e^(-2*pi*i*k/N)
    std::array<float, N>          re_, im_;   // in-place work buffers
};
