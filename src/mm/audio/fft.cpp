#include "mm/audio/fft.h"

#include <algorithm>
#include <cmath>

namespace mm::dsp {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

size_t Fft::nextPowerOfTwo(size_t n) noexcept {
    if (n <= 1) return 1;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

void Fft::forward(std::vector<Complex>& data) {
    const size_t n = data.size();
    if (n <= 1) return;
    if (!isPowerOfTwo(n)) return;  // 断言由调用方保证

    // 位反转置换
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const Complex wlen(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            Complex w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const Complex u = data[i + k];
                const Complex v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void Fft::inverse(std::vector<Complex>& data) {
    const size_t n = data.size();
    if (n <= 1) return;
    for (Complex& c : data) c = std::conj(c);
    forward(data);
    const float scale = 1.0f / static_cast<float>(n);
    for (Complex& c : data) c = std::conj(c) * scale;
}

void Fft::realForward(std::vector<float>& data) {
    const size_t n = data.size();
    if (n <= 1) return;
    std::vector<Complex> buf(n);
    for (size_t i = 0; i < n; ++i) buf[i] = Complex(data[i], 0.0f);
    forward(buf);
    for (size_t i = 0; i < n; ++i) data[i] = buf[i].real();
}

std::vector<float> powerSpectrum(const std::vector<float>& frame) {
    const size_t n = Fft::nextPowerOfTwo(frame.size());
    std::vector<Complex> buf(n, Complex(0.0f, 0.0f));
    for (size_t i = 0; i < frame.size(); ++i) buf[i] = Complex(frame[i], 0.0f);
    Fft::forward(buf);
    const size_t half = n / 2 + 1;
    std::vector<float> out(half);
    for (size_t i = 0; i < half; ++i) {
        const float re = buf[i].real();
        const float im = buf[i].imag();
        out[i] = re * re + im * im;
    }
    return out;
}

std::vector<float> magnitudeSpectrum(const std::vector<float>& frame) {
    std::vector<float> p = powerSpectrum(frame);
    for (float& v : p) v = std::sqrt(v);
    return p;
}

}  // namespace mm::dsp
