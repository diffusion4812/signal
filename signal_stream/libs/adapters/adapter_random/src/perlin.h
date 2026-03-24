#pragma once
#include <vector>
#include <numeric>
#include <algorithm>
#include <random>
#include <cmath>

namespace signal_stream::detail {

    class Perlin1D {
    public:
        explicit Perlin1D(uint32_t seed = 0) {
            std::vector<int> permutation(256);
            std::iota(permutation.begin(), permutation.end(), 0);
            std::default_random_engine engine(seed);
            std::shuffle(permutation.begin(), permutation.end(), engine);

            for (int i = 0; i < 256; i++) {
                p_[256 + i] = p_[i] = permutation[i];
            }
        }

        double noise(double x) const {
            int X = static_cast<int>(std::floor(x)) & 255;
            double x_frac = x - std::floor(x);
            double u = fade(x_frac);
            double g0 = grad(p_[X], x_frac);
            double g1 = grad(p_[X + 1], x_frac - 1.0);
            return lerp(u, g0, g1);
        }

        double fractalNoise(double x, int octaves = 4) const {
            double total = 0;
            double frequency = 1.0;
            double amplitude = 1.0;
            double maxValue = 0;

            for (int i = 0; i < octaves; i++) {
                total += noise(x * frequency) * amplitude;
                maxValue += 0.5 * amplitude;
                amplitude *= 0.5;
                frequency *= 2.0;
            }

            return total / maxValue;
        }

    private:
        int p_[512]{};

        static double fade(double t) {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        static double lerp(double t, double a, double b) {
            return a + t * (b - a);
        }

        static double grad(int hash, double x) {
            return (hash & 1) == 0 ? x : -x;
        }
    };

} // namespace signal_stream::detail