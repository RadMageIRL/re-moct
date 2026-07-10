// viz_fft_test - proves include/VizFFT.h in ISOLATION before it touches
// computeVizBins() (Slice C probe-first discipline: a from-scratch numerical
// routine never lands in the hot path unvalidated).
//
// Layers of proof:
//   1. Known-answer: DC -> all energy in bin 0; bin-centred sines -> energy in
//      round(f*N/sr); multi-tone -> both expected bins.
//   2. Sweep alias check: Hann-windowed sine swept 100 Hz - 20 kHz, peak bin
//      must track the input frequency linearly all the way to near-Nyquist.
//      This is the check the OLD stride-4 DFT fails: striding by 4 with the
//      full-N twiddle is only coherent to ~sr/8 (~5.5 kHz @ 44.1k).
//   3. Old-bug demonstration: a single 10 kHz tone puts a spurious ~full-height
//      image near 1 kHz in the strided DFT (bass bins lighting up from a
//      treble tone); the FFT shows nothing there.
//   4. Parseval: time-domain energy == frequency-domain energy (real-signal
//      half-spectrum form) on a deterministic multi-sine.
//   5. Frame-cost gate: the FFT + 64-band integration must be cheaper than the
//      old per-band strided-DFT loop it replaces (N log N vs N^2 * bins) -
//      numbers printed as evidence, regression asserted.
//
// Pure (no curses/audio/net), runs in both matrix jobs.

#include "VizFFT.h"

#include <chrono>
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, ...) do {                                                 \
    if (!(cond)) {                                                            \
        ++g_fail;                                                             \
        std::printf("FAIL %s:%d  %s\n  ", __FILE__, __LINE__, #cond);         \
        std::printf(__VA_ARGS__);                                             \
        std::printf("\n");                                                    \
    }                                                                         \
} while (0)

static constexpr int   N  = 2048;      // == AudioManager::VIZ_BUF_SIZE
static constexpr float SR = 44100.0f;
static constexpr float PI = 3.14159265f;

static VizFFT<N> fft;
static float in_buf[N];
static float mag_buf[VizFFT<N>::kBins];

static void fillSine(float* buf, float freq_hz, float amp = 1.0f) {
    for (int n = 0; n < N; ++n)
        buf[n] = amp * std::sin(2.0f * PI * freq_hz * n / SR);
}

static void hann(float* buf) {   // same window shape production applies
    for (int i = 0; i < N; ++i)
        buf[i] *= 0.5f * (1.0f - std::cos(2.0f * PI * i / (N - 1)));
}

static int argmax(const float* m, int lo, int hi) {
    int best = lo;
    for (int k = lo; k < hi; ++k)
        if (m[k] > m[best]) best = k;
    return best;
}

// The OLD magnitude source, verbatim shape from computeVizBins() pre-Slice-C:
// per-k strided (n += 4) DFT with the full-N twiddle. Kept here as the
// reference the FFT is proven against (alias demo + frame-cost comparison).
static float stridedDftMag(const float* s, int k) {
    float re = 0.0f, im = 0.0f;
    const float ang = -2.0f * PI * k / N;
    for (int n = 0; n < N; n += 4) {
        re += s[n] * std::cos(ang * n);
        im += s[n] * std::sin(ang * n);
    }
    return std::sqrt(re * re + im * im);
}

int main() {
    // ── 1a. DC: all energy in bin 0 ─────────────────────────────────────────
    for (int n = 0; n < N; ++n) in_buf[n] = 1.0f;
    fft.magnitude(in_buf, mag_buf);
    CHECK(std::fabs(mag_buf[0] - (float)N) < 0.01f * N,
          "DC bin 0 = %g, want ~%d", mag_buf[0], N);
    for (int k = 1; k < VizFFT<N>::kBins; ++k)
        if (mag_buf[k] > 0.001f * N) {
            CHECK(false, "DC leaked into bin %d (mag %g)", k, mag_buf[k]);
            break;
        }

    // ── 1b. Bin-centred pure sines: energy at round(f*N/sr), height N/2 ─────
    for (int k0 : {5, 50, 200, 500, 900}) {
        const float f = (float)k0 * SR / N;
        fillSine(in_buf, f);
        fft.magnitude(in_buf, mag_buf);
        const int pk = argmax(mag_buf, 1, VizFFT<N>::kBins);
        CHECK(pk == k0, "sine %.1f Hz: peak bin %d, want %d", f, pk, k0);
        CHECK(std::fabs(mag_buf[k0] - N / 2.0f) < 0.02f * (N / 2.0f),
              "sine %.1f Hz: |X[%d]| = %g, want ~%g", f, k0, mag_buf[k0], N / 2.0f);
    }

    // ── 1c. Multi-tone: both expected bins present, elsewhere quiet ─────────
    {
        fillSine(in_buf, 100.0f * SR / N, 1.0f);
        for (int n = 0; n < N; ++n)
            in_buf[n] += 0.5f * std::sin(2.0f * PI * (700.0f * SR / N) * n / SR);
        fft.magnitude(in_buf, mag_buf);
        CHECK(std::fabs(mag_buf[100] - N / 2.0f)  < 0.02f * (N / 2.0f),
              "multitone bin 100 = %g", mag_buf[100]);
        CHECK(std::fabs(mag_buf[700] - N / 4.0f)  < 0.02f * (N / 4.0f),
              "multitone bin 700 = %g", mag_buf[700]);
        CHECK(mag_buf[400] < 0.01f * (N / 2.0f),
              "multitone off-bin 400 = %g, want ~0", mag_buf[400]);
    }

    // ── 2. Sweep 100 Hz - 20 kHz: peak bin tracks f linearly to Nyquist ─────
    // Hann-windowed (production shape). The old strided DFT folds above
    // ~sr/8 = 5512 Hz; the FFT must NOT.
    {
        int worst_err = 0;
        for (float f = 100.0f; f <= 20000.0f; f *= 1.07f) {
            fillSine(in_buf, f);
            hann(in_buf);
            fft.magnitude(in_buf, mag_buf);
            const int pk   = argmax(mag_buf, 1, VizFFT<N>::kBins);
            const int want = (int)std::lround(f * N / SR);
            const int err  = std::abs(pk - want);
            if (err > worst_err) worst_err = err;
            CHECK(err <= 2, "sweep %.0f Hz: peak bin %d, want %d (+/-2)", f, pk, want);
        }
        std::printf("sweep 100 Hz - 20 kHz: worst peak-bin error = %d bins\n",
                    worst_err);
    }

    // ── 3. Old-bug demo: 10 kHz tone -> spurious bass image in strided DFT ──
    // Stride 4 decimates to sr/4 = 11025 Hz, so 10 kHz folds to 1025 Hz: the
    // strided DFT shows a ~full-height image near bin 48 (~1 kHz). The FFT
    // must show nothing there.
    {
        fillSine(in_buf, 10000.0f);
        hann(in_buf);
        fft.magnitude(in_buf, mag_buf);
        const int true_bin  = (int)std::lround(10000.0f * N / SR);   // ~464
        const int image_bin = (int)std::lround(1025.0f  * N / SR);   // ~48

        float fft_true = 0, fft_img = 0, dft_true = 0, dft_img = 0;
        for (int k = image_bin - 2; k <= image_bin + 2; ++k) {
            fft_img = std::fmax(fft_img, mag_buf[k]);
            dft_img = std::fmax(dft_img, stridedDftMag(in_buf, k));
        }
        for (int k = true_bin - 2; k <= true_bin + 2; ++k) {
            fft_true = std::fmax(fft_true, mag_buf[k]);
            dft_true = std::fmax(dft_true, stridedDftMag(in_buf, k));
        }
        std::printf("10 kHz tone, ~1 kHz image/true ratio: strided DFT %.3f, "
                    "FFT %.6f\n", dft_img / dft_true, fft_img / fft_true);
        CHECK(dft_img > 0.5f * dft_true,
              "expected the OLD strided DFT to alias (image %g vs true %g) - "
              "if this fails the premise of Slice C needs re-checking",
              dft_img, dft_true);
        CHECK(fft_img < 0.01f * fft_true,
              "FFT has spurious 1 kHz energy from a 10 kHz tone (image %g, "
              "true %g)", fft_img, fft_true);
    }

    // ── 4. Parseval: sum x^2 == (|X0|^2 + 2*sum|Xk|^2) / N ──────────────────
    // Deterministic multi-sine, all components < N/4 bins so the (unreported)
    // Nyquist bin holds no energy. Double accumulators; FFT itself is float.
    {
        unsigned s = 12345u;                       // tiny deterministic LCG
        for (int n = 0; n < N; ++n) in_buf[n] = 0.0f;
        for (int c = 0; c < 50; ++c) {
            s = s * 1664525u + 1013904223u;
            const int   k   = 1 + (int)(s % (N / 4));
            const float amp = 0.02f + (float)((s >> 16) % 100) / 1000.0f;
            for (int n = 0; n < N; ++n)
                in_buf[n] += amp * std::sin(2.0f * PI * k * n / (float)N);
        }
        double e_time = 0.0;
        for (int n = 0; n < N; ++n) e_time += (double)in_buf[n] * in_buf[n];

        fft.magnitude(in_buf, mag_buf);
        double e_freq = (double)mag_buf[0] * mag_buf[0];
        for (int k = 1; k < VizFFT<N>::kBins; ++k)
            e_freq += 2.0 * (double)mag_buf[k] * mag_buf[k];
        e_freq /= N;

        CHECK(std::fabs(e_freq - e_time) < 0.01 * e_time,
              "Parseval: time %.6f vs freq %.6f", e_time, e_freq);
        std::printf("Parseval: time %.6f, freq %.6f (rel err %.2e)\n",
                    e_time, e_freq, std::fabs(e_freq - e_time) / e_time);
    }

    // ── 5. Frame-cost gate: FFT + 64-band integrate vs old strided-DFT loop ─
    // Reproduces the production band structure (64 log bands, 20 Hz - 18 kHz,
    // same k_lo/k_hi mapping) so both sides do the work computeVizBins() does.
    {
        constexpr int VIZ_BINS = 64;
        const float log_min = std::log(20.0f), log_max = std::log(18000.0f);
        int klo[VIZ_BINS], khi[VIZ_BINS];
        for (int b = 0; b < VIZ_BINS; ++b) {
            const float f_lo = std::exp(log_min + (log_max - log_min) *  b      / VIZ_BINS);
            const float f_hi = std::exp(log_min + (log_max - log_min) * (b + 1) / VIZ_BINS);
            int k_lo = (int)(f_lo / SR * N), k_hi = (int)(f_hi / SR * N);
            if (k_lo < 1) k_lo = 1;
            if (k_hi <= k_lo) k_hi = k_lo + 1;
            klo[b] = k_lo; khi[b] = k_hi;
        }
        fillSine(in_buf, 1000.0f);
        hann(in_buf);

        using clk = std::chrono::steady_clock;
        volatile float sink = 0.0f;   // keep the loops honest

        const int OLD_REPS = 5;
        const auto t0 = clk::now();
        for (int r = 0; r < OLD_REPS; ++r)
            for (int b = 0; b < VIZ_BINS; ++b) {
                float m = 0.0f;
                for (int k = klo[b]; k < khi[b]; ++k)
                    m += stridedDftMag(in_buf, k);
                sink += m / ((float)(N / 4) * (khi[b] - klo[b]));
            }
        const auto t1 = clk::now();

        const int NEW_REPS = 500;
        for (int r = 0; r < NEW_REPS; ++r) {
            fft.magnitude(in_buf, mag_buf);
            for (int b = 0; b < VIZ_BINS; ++b) {
                float m = 0.0f;
                for (int k = klo[b]; k < khi[b]; ++k) m += mag_buf[k];
                sink += m / (float)(khi[b] - klo[b]);
            }
        }
        const auto t2 = clk::now();

        const double us_old = std::chrono::duration<double, std::micro>(t1 - t0).count() / OLD_REPS;
        const double us_new = std::chrono::duration<double, std::micro>(t2 - t1).count() / NEW_REPS;
        std::printf("frame cost: old strided DFT %.1f us, FFT+integrate %.1f us "
                    "(%.0fx)\n", us_old, us_new, us_old / us_new);
        CHECK(us_new < us_old,
              "FFT path (%.1f us) not cheaper than old DFT (%.1f us)",
              us_new, us_old);
        (void)sink;
    }

    if (g_fail == 0) std::printf("viz_fft_test: all checks passed\n");
    else             std::printf("viz_fft_test: %d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
