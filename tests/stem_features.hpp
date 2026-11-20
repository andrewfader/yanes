// The song corpus's stem features (song-corpus/scripts/features.py), ported so a render can be held
// to the records' numbers inside the suite: spectral centroid, 85% rolloff and share above 3 kHz of
// the mean active power spectrum (8192-point Hann STFT, hop 2048), the left/right correlation, and
// the tape wobble from librosa's pYIN (65-1050 Hz at 22.05 kHz, 2048-sample frames, hop 256): the
// median, over sustained pitch runs, of the frame-to-frame cents movement left after a 21-frame
// moving average. pYIN reads pitch on a 10-cent grid through a Viterbi path, which is why a few
// cents of real wobble can measure as none; the port keeps that, so the gate sees what the
// extractor sees.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

#include "audio_measure.hpp"

namespace stem {

constexpr double kRate = 44100.0;

struct Features {
  double centroid_hz, rolloff85_hz, hf_share_db, corr, wobble_cents;
};

inline double db10(double x) { return 10.0 * std::log10(std::max(x, 1e-20)); }

inline void spectral(const std::vector<float>& y, Features& f) {
  constexpr size_t n = 8192, hop = 2048;
  std::vector<double> padded(y.size() + n, 0.0);
  std::copy(y.begin(), y.end(), padded.begin() + n / 2);
  const size_t frames = 1 + (padded.size() - n) / hop;
  std::vector<std::vector<double>> power(frames, std::vector<double>(n / 2 + 1));
  std::vector<double> energy(frames, 0.0);
  std::vector<std::complex<double>> a(n);
  for (size_t t = 0; t < frames; ++t) {
    for (size_t i = 0; i < n; ++i)
      a[i] = padded[t * hop + i] * (0.5 - 0.5 * std::cos(2.0 * measure::kPi * static_cast<double>(i) / n));
    measure::fft(a);
    for (size_t k = 0; k <= n / 2; ++k) energy[t] += power[t][k] = std::norm(a[k]);
  }
  const double loudest = *std::max_element(energy.begin(), energy.end());
  std::vector<double> mean(n / 2 + 1, 0.0);
  for (size_t t = 0; t < frames; ++t) {
    if (energy[t] <= loudest * 1e-4) continue;
    for (size_t k = 0; k <= n / 2; ++k) mean[k] += power[t][k];
  }
  double total = 0, moment = 0, high = 0;
  const double bin_hz = kRate / n;
  for (size_t k = 0; k <= n / 2; ++k) {
    const double hz = k * bin_hz;
    if (hz < 20.0) continue;
    total += mean[k];
    moment += hz * mean[k];
    if (hz >= 3000.0) high += mean[k];
  }
  f.centroid_hz = moment / total;
  f.hf_share_db = db10(high / total);
  double running = 0;
  for (size_t k = 0; k <= n / 2; ++k) {
    if (k * bin_hz < 20.0) continue;
    running += mean[k];
    if (running / total >= 0.85) { f.rolloff85_hz = k * bin_hz; break; }
  }
}

inline double correlation(const std::vector<float>& left, const std::vector<float>& right) {
  double lr = 0, ll = 0, rr = 0;
  for (size_t i = 0; i < left.size(); ++i) {
    lr += static_cast<double>(left[i]) * right[i];
    ll += static_cast<double>(left[i]) * left[i];
    rr += static_cast<double>(right[i]) * right[i];
  }
  return lr / (std::sqrt(ll * rr) + 1e-20);
}

// Halves the rate with a linear-phase Kaiser-windowed sinc, passband to 10 kHz.
inline std::vector<double> decimate(const std::vector<float>& y) {
  constexpr int half = 128;
  constexpr double beta = 9.0, cutoff = 0.5 * 21050.0 / 22050.0;
  const auto bessel_i0 = [](double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 40; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
    return sum;
  };
  std::array<double, 2 * half + 1> h{};
  for (int k = -half; k <= half; ++k) {
    const double r = static_cast<double>(k) / half;
    const double window = bessel_i0(beta * std::sqrt(1 - r * r)) / bessel_i0(beta);
    const double sinc = k == 0 ? 1.0 : std::sin(measure::kPi * cutoff * k) / (measure::kPi * cutoff * k);
    h[static_cast<size_t>(k + half)] = cutoff * sinc * window;
  }
  std::vector<double> out((y.size() + 1) / 2);
  for (size_t m = 0; m < out.size(); ++m) {
    double acc = 0;
    for (int k = -half; k <= half; ++k) {
      const long i = static_cast<long>(2 * m) + k;
      if (i >= 0 && i < static_cast<long>(y.size())) acc += h[static_cast<size_t>(k + half)] * y[static_cast<size_t>(i)];
    }
    out[m] = acc;
  }
  return out;
}

// librosa.pyin with features.py's arguments; NaN where unvoiced.
inline std::vector<double> pyin(const std::vector<double>& y) {
  constexpr double sr = 22050.0, fmin = 65.0, fmax = 1050.0;
  constexpr size_t frame = 2048, hop = 256, fft_size = 4096;
  constexpr int min_period = 21, max_period = 340;  // floor(sr / fmax), ceil(sr / fmin)
  constexpr int lags = max_period - min_period + 1;
  constexpr int bins_per_semitone = 10;
  const int bins = static_cast<int>(std::floor(12 * bins_per_semitone * std::log2(fmax / fmin))) + 1;

  std::vector<double> padded(y.size() + frame, 0.0);
  std::copy(y.begin(), y.end(), padded.begin() + frame / 2);
  const size_t frames = 1 + (padded.size() - frame) / hop;

  std::array<double, 101> thresholds{};
  for (int j = 0; j <= 100; ++j) thresholds[static_cast<size_t>(j)] = j / 100.0;
  // Beta(2, 18) CDF in closed form.
  std::array<double, 100> beta{};
  const auto cdf = [](double x) { return 1.0 - std::pow(1.0 - x, 18) * (1.0 + 18.0 * x); };
  for (size_t j = 0; j < 100; ++j) beta[j] = cdf(thresholds[j + 1]) - cdf(thresholds[j]);
  const auto boltzmann = [](int k, int count) {
    constexpr double lambda = 2.0;
    return (1 - std::exp(-lambda)) / (1 - std::exp(-lambda * count)) * std::exp(-lambda * k);
  };

  const size_t states = 2 * static_cast<size_t>(bins);
  std::vector<std::vector<double>> observation(frames, std::vector<double>(states, 0.0));
  std::vector<std::complex<double>> a(fft_size);
  for (size_t t = 0; t < frames; ++t) {
    const double* x = padded.data() + t * hop;
    std::fill(a.begin(), a.end(), 0.0);
    for (size_t i = 0; i < frame; ++i) a[i] = x[i];
    measure::fft(a);
    for (auto& v : a) v = std::norm(v);
    measure::fft(a);
    std::array<double, max_period + 1> difference{}, energy{};
    double running = 0;
    for (int i = 0; i <= max_period; ++i) { running += x[i] * x[i]; energy[static_cast<size_t>(i)] = running; }
    const double acf0 = a[0].real() / fft_size;
    for (int k = 1; k <= max_period; ++k)
      difference[static_cast<size_t>(k)] = 2 * (acf0 - a[static_cast<size_t>(k)].real() / fft_size) - energy[static_cast<size_t>(k - 1)];
    std::array<double, lags> cmnd{};
    double cumulative = 0;
    for (int k = 1; k <= max_period; ++k) {
      cumulative += difference[static_cast<size_t>(k)];
      if (k >= min_period)
        cmnd[static_cast<size_t>(k - min_period)] =
            difference[static_cast<size_t>(k)] / (cumulative / k + std::numeric_limits<double>::min());
    }
    std::array<double, lags> shift{};
    for (int i = 1; i + 1 < lags; ++i) {
      const double curve = cmnd[i + 1] + cmnd[i - 1] - 2 * cmnd[i], slope = (cmnd[i + 1] - cmnd[i - 1]) / 2;
      shift[static_cast<size_t>(i)] = std::abs(slope) >= std::abs(curve) ? 0.0 : -slope / curve;
    }
    std::vector<int> troughs;
    for (int i = 0; i < lags; ++i) {
      const bool trough = i == 0 ? cmnd[0] < cmnd[1]
                        : i == lags - 1 ? cmnd[i] < cmnd[i - 1]
                        : cmnd[i] < cmnd[i - 1] && cmnd[i] <= cmnd[i + 1];
      if (trough) troughs.push_back(i);
    }
    std::vector<double> probability(troughs.size(), 0.0);
    std::array<int, 100> below{};
    for (int i : troughs)
      for (size_t j = 0; j < 100; ++j) below[j] += cmnd[static_cast<size_t>(i)] < thresholds[j + 1];
    std::array<int, 100> position{};
    for (size_t n = 0; n < troughs.size(); ++n) {
      const double height = cmnd[static_cast<size_t>(troughs[n])];
      for (size_t j = 0; j < 100; ++j) {
        if (!(height < thresholds[j + 1])) continue;
        probability[n] += boltzmann(position[j]++, below[j]) * beta[j];
      }
    }
    if (!troughs.empty()) {
      size_t lowest = 0;
      for (size_t n = 1; n < troughs.size(); ++n)
        if (cmnd[static_cast<size_t>(troughs[n])] < cmnd[static_cast<size_t>(troughs[lowest])]) lowest = n;
      double unclaimed = 0;
      for (size_t j = 0; j < 100; ++j)
        if (!(cmnd[static_cast<size_t>(troughs[lowest])] < thresholds[j + 1])) unclaimed += beta[j];
      probability[lowest] += 0.01 * unclaimed;
    }
    for (size_t n = 0; n < troughs.size(); ++n) {
      if (probability[n] == 0.0) continue;
      const double period = min_period + troughs[n] + shift[static_cast<size_t>(troughs[n])];
      const double bin = std::nearbyint(12 * bins_per_semitone * std::log2(sr / period / fmin));
      observation[t][static_cast<size_t>(std::clamp(bin, 0.0, static_cast<double>(bins)))] = probability[n];
    }
    double voiced = 0;
    for (int b = 0; b < bins; ++b) voiced += observation[t][static_cast<size_t>(b)];
    voiced = std::clamp(voiced, 0.0, 1.0);
    for (int b = 0; b < bins; ++b) observation[t][static_cast<size_t>(bins + b)] = (1 - voiced) / bins;
  }

  // Transitions: a 51-bin triangle within the voiced and unvoiced halves (row-normalised at the
  // edges), a 1% chance of switching halves, and nothing under 1e-4 considered.
  constexpr int width = 25;
  const double tiny = std::numeric_limits<double>::min();
  std::vector<double> row_sum(static_cast<size_t>(bins), 0.0);
  for (int i = 0; i < bins; ++i)
    for (int j = std::max(0, i - width); j <= std::min(bins - 1, i + width); ++j)
      row_sum[static_cast<size_t>(i)] += (26.0 - std::abs(j - i)) / 26.0;
  struct Edge { int from; double log_p; };
  std::vector<std::vector<Edge>> into(states);
  const double floor = std::log(1e-4 + tiny);
  for (size_t j = 0; j < states; ++j) {
    const int jb = static_cast<int>(j) % bins;
    for (size_t k = 0; k < states; ++k) {
      const int kb = static_cast<int>(k) % bins;
      if (std::abs(kb - jb) > width) continue;
      const double local = (26.0 - std::abs(kb - jb)) / 26.0 / row_sum[static_cast<size_t>(kb)];
      const double p = ((k < static_cast<size_t>(bins)) == (j < static_cast<size_t>(bins)) ? 0.99 : 0.01) * local;
      const double log_p = std::log(p + tiny);
      if (log_p >= floor) into[j].push_back({static_cast<int>(k), log_p});
    }
  }
  std::vector<double> value(states), next(states);
  std::vector<std::vector<int>> from(frames, std::vector<int>(states, 0));
  for (size_t s = 0; s < states; ++s) value[s] = std::log(observation[0][s] + tiny) + std::log(1.0 / states + tiny);
  for (size_t t = 1; t < frames; ++t) {
    for (size_t j = 0; j < states; ++j) {
      double best = -std::numeric_limits<double>::infinity();
      for (const Edge& e : into[j]) {
        const double cost = value[static_cast<size_t>(e.from)] + e.log_p;
        if (cost > best) { best = cost; from[t][j] = e.from; }
      }
      next[j] = std::log(observation[t][j] + tiny) + best;
    }
    std::swap(value, next);
  }
  std::vector<double> f0(frames);
  size_t state = static_cast<size_t>(std::max_element(value.begin(), value.end()) - value.begin());
  for (size_t t = frames; t-- > 0;) {
    f0[t] = state < static_cast<size_t>(bins) ? fmin * std::exp2(static_cast<double>(state) / (12 * bins_per_semitone))
                                              : std::numeric_limits<double>::quiet_NaN();
    if (t > 0) state = static_cast<size_t>(from[t][state]);
  }
  return f0;
}

// features.py's wobble: runs of voiced frames that move less than 60 cents per frame, 30 or more
// frames long; each run's spread around its 21-frame moving average; the median of the runs.
inline double wobble_cents(const std::vector<double>& f0) {
  int voiced = 0;
  for (double hz : f0) voiced += std::isfinite(hz);
  if (voiced < 40) return std::numeric_limits<double>::quiet_NaN();
  std::vector<double> spreads, run;
  const auto close = [&] {
    constexpr size_t k = 21;
    if (run.size() < 30) return;
    std::vector<double> residual;
    for (size_t i = 0; i + k <= run.size(); ++i) {
      double mean = 0;
      for (size_t j = i; j < i + k; ++j) mean += run[j];
      residual.push_back(run[i + k / 2] - mean / k);
    }
    double mean = 0, squares = 0;
    for (double r : residual) mean += r;
    mean /= static_cast<double>(residual.size());
    for (double r : residual) squares += (r - mean) * (r - mean);
    spreads.push_back(std::sqrt(squares / static_cast<double>(residual.size())));
  };
  for (double hz : f0) {
    const double c = 1200.0 * std::log2(hz / 440.0);
    if (std::isfinite(c) && (run.empty() || std::abs(c - run.back()) < 60)) {
      run.push_back(c);
    } else {
      close();
      run.clear();
      if (std::isfinite(c)) run.push_back(c);
    }
  }
  if (spreads.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(spreads.begin(), spreads.end());
  const size_t m = spreads.size();
  return m % 2 ? spreads[m / 2] : 0.5 * (spreads[m / 2 - 1] + spreads[m / 2]);
}

// A stereo render at 44.1 kHz, measured as features.py measures a stem.
inline Features measure(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> mono(left.size());
  for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (left[i] + right[i]);
  Features f{};
  spectral(mono, f);
  f.corr = correlation(left, right);
  f.wobble_cents = wobble_cents(pyin(decimate(mono)));
  return f;
}

}  // namespace stem
