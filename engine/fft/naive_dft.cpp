#include "naive_dft.hpp"

#include <cmath>

namespace aud::fft::testing {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Textbook O(N^2) real DFT/inverse. Not fast, not meant to be — only used by tests at sizes small
// enough that this is instant (see the M06 unit test list's N in {16, 64, 256, 1024, 2048, 4096}).
class NaiveDft final : public RealFft {
public:
    explicit NaiveDft(std::size_t n) : m_n(n) {}

    [[nodiscard]] std::size_t size() const noexcept override { return m_n; }

    void forward(std::span<const float> in, std::span<std::complex<float>> out) override {
        const std::size_t bins = m_n / 2 + 1;
        for (std::size_t k = 0; k < bins; ++k) {
            double re = 0.0;
            double im = 0.0;
            for (std::size_t n = 0; n < m_n; ++n) {
                const double angle =
                    -2.0 * kPi * static_cast<double>(k) * static_cast<double>(n) / static_cast<double>(m_n);
                re += static_cast<double>(in[n]) * std::cos(angle);
                im += static_cast<double>(in[n]) * std::sin(angle);
            }
            out[k] = std::complex<float>(static_cast<float>(re), static_cast<float>(im));
        }
    }

    void inverse(std::span<const std::complex<float>> in, std::span<float> out) override {
        const std::size_t bins = m_n / 2 + 1;
        for (std::size_t n = 0; n < m_n; ++n) {
            double acc = 0.0;
            for (std::size_t k = 0; k < m_n; ++k) {
                // Reconstruct the full complex spectrum from the half-spectrum via Hermitian
                // symmetry: X[N-k] = conj(X[k]).
                double re;
                double im;
                if (k < bins) {
                    re = in[k].real();
                    im = in[k].imag();
                } else {
                    const std::size_t mirror = m_n - k;
                    re                       = in[mirror].real();
                    im                       = -in[mirror].imag();
                }
                const double angle =
                    2.0 * kPi * static_cast<double>(k) * static_cast<double>(n) / static_cast<double>(m_n);
                acc += re * std::cos(angle) - im * std::sin(angle);
            }
            out[n] = static_cast<float>(acc / static_cast<double>(m_n));
        }
    }

private:
    std::size_t m_n;
};

}  // namespace

std::unique_ptr<RealFft> createNaiveDft(std::size_t n) {
    return std::make_unique<NaiveDft>(n);
}

}  // namespace aud::fft::testing
