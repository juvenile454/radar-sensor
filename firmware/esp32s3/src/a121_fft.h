/**
 * @file a121_fft.h
 * @brief Real FFT (radix 2, float) for the spectra of the vital DSP (V2.2, B13); complex FFT for V3.3.
 *
 * Portable and library-free, so that ESP32-S3, PC replay and phone (NDK) run the same computation. Replaces the
 * per-frequency Goertzel sums: N real values (zero-padded) → complex FFT of length N/2 via z[n] = x[2n] + j x[2n+1],
 * then split into the spectrum of the real sequence. Power |X_k|² without normalization (like Goertzel).
 * Twiddle factors and bit reversal are computed once in the constructor; no dynamic memory.
 */
#ifndef A121_FFT_H
#define A121_FFT_H

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace a121_fft {

template <uint16_t N>
class RealFft {
    static_assert(N >= 8 && (N & (N - 1U)) == 0, "N muss eine Zweierpotenz sein");
    static constexpr uint16_t kHalf = N / 2U;

public:
    RealFft() {
        for (uint16_t k = 0; k < kHalf; k++) {
            const double a = -2.0 * M_PI * (double)k / (double)N;  /* once, double only here */
            tw_re_[k] = (float)cos(a);
            tw_im_[k] = (float)sin(a);
        }
        uint16_t bits = 0;
        while ((1U << bits) < kHalf) bits++;
        for (uint16_t i = 0; i < kHalf; i++) {
            uint16_t r = 0;
            for (uint16_t b = 0; b < bits; b++)
                if (i & (1U << b)) r |= (uint16_t)(1U << (bits - 1U - b));
            rev_[i] = r;
        }
    }

    /**
     * Power |X_k|² of the zero-padded sequence x[0 .. len) for k = 0 .. n_out − 1 (n_out ≤ N/2 + 1, len ≤ N).
     */
    void power(const float *x, uint16_t len, float *out, uint16_t n_out) {
        if (len > N) len = N;
        if (n_out > kHalf + 1U) n_out = kHalf + 1U;
        for (uint16_t i = 0; i < kHalf; i++) {
            const uint16_t r = rev_[i];
            const uint16_t n0 = (uint16_t)(2U * i);
            re_[r] = (n0 < len) ? x[n0] : 0.0f;
            im_[r] = (n0 + 1U < len) ? x[n0 + 1U] : 0.0f;
        }
        /* complex FFT of length N/2 (decimation in time); twiddle factor e^{-j2πk/(N/2)} = tw[2k] */
        for (uint16_t size = 2; size <= kHalf; size = (uint16_t)(size * 2U)) {
            const uint16_t h = size / 2U, step = (uint16_t)(N / size);
            for (uint16_t i = 0; i < kHalf; i = (uint16_t)(i + size)) {
                for (uint16_t j = 0; j < h; j++) {
                    const float wr = tw_re_[j * step], wi = tw_im_[j * step];
                    const uint16_t a = (uint16_t)(i + j), b = (uint16_t)(a + h);
                    const float vr = re_[b] * wr - im_[b] * wi;
                    const float vi = re_[b] * wi + im_[b] * wr;
                    re_[b] = re_[a] - vr;
                    im_[b] = im_[a] - vi;
                    re_[a] += vr;
                    im_[a] += vi;
                }
            }
        }
        /* X_k = (Z_k + Z*_{N/2−k}) / 2 − j/2 · e^{-j2πk/N} · (Z_k − Z*_{N/2−k}), Z_{N/2} = Z_0 */
        for (uint16_t k = 0; k < n_out; k++) {
            const uint16_t k1 = (k == kHalf) ? 0U : k;
            const uint16_t k2 = (k == 0 || k == kHalf) ? 0U : (uint16_t)(kHalf - k);
            const float zr = re_[k1], zi = im_[k1], cr = re_[k2], ci = -im_[k2];
            const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);  /* even sequence */
            const float dr = 0.5f * (zr - cr), di = 0.5f * (zi - ci);
            /* odd sequence: −j · (dr + j di) = di − j dr, then × twiddle factor */
            const float or_ = di, oi = -dr;
            float wr, wi;
            if (k < kHalf) {
                wr = tw_re_[k];
                wi = tw_im_[k];
            } else {
                wr = -1.0f;  /* e^{-jπ} */
                wi = 0.0f;
            }
            const float xr = er + (or_ * wr - oi * wi);
            const float xi = ei + (or_ * wi + oi * wr);
            out[k] = xr * xr + xi * xi;
        }
    }

private:
    float tw_re_[kHalf], tw_im_[kHalf];
    uint16_t rev_[kHalf];
    float re_[kHalf], im_[kHalf];
};

/**
 * Complex FFT in place (radix 2, decimation in time) for the analytic signal of the harmonic tracking (V3.3).
 * Forward with e^{-j2πkn/N}, backward with e^{+j2πkn/N} and 1/N. Twiddle factors and bit reversal in the constructor.
 */
template <uint16_t N>
class ComplexFft {
    static_assert(N >= 4 && (N & (N - 1U)) == 0, "N muss eine Zweierpotenz sein");

public:
    ComplexFft() {
        for (uint16_t k = 0; k < N / 2U; k++) {
            const double a = -2.0 * M_PI * (double)k / (double)N;
            tw_re_[k] = (float)cos(a);
            tw_im_[k] = (float)sin(a);
        }
        uint16_t bits = 0;
        while ((1U << bits) < N) bits++;
        for (uint16_t i = 0; i < N; i++) {
            uint16_t r = 0;
            for (uint16_t b = 0; b < bits; b++)
                if (i & (1U << b)) r |= (uint16_t)(1U << (bits - 1U - b));
            rev_[i] = r;
        }
    }

    void transform(float *re, float *im, bool inverse) const {
        for (uint16_t i = 0; i < N; i++) {
            const uint16_t r = rev_[i];
            if (r > i) {
                const float tr = re[i], ti = im[i];
                re[i] = re[r];
                im[i] = im[r];
                re[r] = tr;
                im[r] = ti;
            }
        }
        const float sgn = inverse ? -1.0f : 1.0f;
        for (uint16_t size = 2; size <= N; size = (uint16_t)(size * 2U)) {
            const uint16_t h = size / 2U, step = (uint16_t)(N / size);
            for (uint16_t i = 0; i < N; i = (uint16_t)(i + size)) {
                for (uint16_t j = 0; j < h; j++) {
                    const float wr = tw_re_[j * step], wi = sgn * tw_im_[j * step];
                    const uint16_t a = (uint16_t)(i + j), b = (uint16_t)(a + h);
                    const float vr = re[b] * wr - im[b] * wi;
                    const float vi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - vr;
                    im[b] = im[a] - vi;
                    re[a] += vr;
                    im[a] += vi;
                }
            }
        }
        if (inverse) {
            const float s = 1.0f / (float)N;
            for (uint16_t i = 0; i < N; i++) {
                re[i] *= s;
                im[i] *= s;
            }
        }
    }

private:
    float tw_re_[N / 2U], tw_im_[N / 2U];
    uint16_t rev_[N];
};

}  // namespace a121_fft

#endif /* A121_FFT_H */
