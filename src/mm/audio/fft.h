// MeetMind — 快速傅里叶变换（迭代基-2 Cooley-Tukey）
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace mm::dsp {

using Complex = std::complex<float>;

class Fft {
public:
    static bool isPowerOfTwo(size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }
    static size_t nextPowerOfTwo(size_t n) noexcept;

    /// 原地正向变换；data.size() 必须是 2 的幂。
    static void forward(std::vector<Complex>& data);
    /// 原地逆向变换（含 1/N 缩放）；data.size() 必须是 2 的幂。
    static void inverse(std::vector<Complex>& data);
    /// 原地实序列变换：内部把实数按一半长度复数处理，随后还原谱。
    static void realForward(std::vector<float>& data);
};

/// 实数序列 → 幅度谱（返回 size/2 + 1 个频点，内部补零至 2 的幂）。
std::vector<float> magnitudeSpectrum(const std::vector<float>& frame);

/// 实数序列 → 功率谱（幅度平方）。
std::vector<float> powerSpectrum(const std::vector<float>& frame);

}  // namespace mm::dsp
