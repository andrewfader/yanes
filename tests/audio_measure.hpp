// Measurements in the units a parameter is labelled in: Hz, cents, dB, ms, duty,
// harmonic levels. Used by tests/advertised_tests.cpp to prove each knob against
// its label from rendered audio. Everything here works on mono float buffers at
// a known sample rate.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numeric>
#include <vector>

namespace measure {

constexpr double kPi = 3.14159265358979323846;

inline double db(double linear) { return 20.0 * std::log10(std::max(linear, 1e-12)); }
inline double cents(double hz, double reference_hz) { return 1200.0 * std::log2(hz / reference_hz); }

// Sub-range [begin, begin + length) clipped to the buffer.
struct Span {
  const std::vector<float>* x;
  size_t begin, end;
  Span(const std::vector<float>& v, size_t b = 0, size_t n = static_cast<size_t>(-1))
      : x(&v), begin(std::min(b, v.size())), end(n == static_cast<size_t>(-1) ? v.size() : std::min(v.size(), b + n)) {}
  size_t size() const { return end - begin; }
  float operator[](size_t i) const { return (*x)[begin + i]; }
};

inline double mean(const Span& s) {
  double sum = 0;
  for (size_t i = 0; i < s.size(); ++i) sum += s[i];
  return s.size() ? sum / static_cast<double>(s.size()) : 0.0;
}

// RMS with the mean removed: DC offsets are not loudness.
inline double rms(const Span& s) {
  if (!s.size()) return 0;
  const double m = mean(s);
  double sq = 0;
  for (size_t i = 0; i < s.size(); ++i) sq += (s[i] - m) * (s[i] - m);
  return std::sqrt(sq / static_cast<double>(s.size()));
}

inline double peak(const Span& s) {
  double p = 0;
  for (size_t i = 0; i < s.size(); ++i) p = std::max(p, static_cast<double>(std::abs(s[i])));
  return p;
}

// RMS contour in dB on a fixed hop, one value per hop.
inline std::vector<double> contour_db(const std::vector<float>& x, double rate, double hop_s = 0.005,
                                      double window_s = 0.010) {
  const size_t hop = std::max<size_t>(1, static_cast<size_t>(rate * hop_s));
  const size_t win = std::max<size_t>(1, static_cast<size_t>(rate * window_s));
  std::vector<double> out;
  for (size_t p = 0; p + win <= x.size(); p += hop) out.push_back(db(rms(Span(x, p, win))));
  return out;
}

// YIN fundamental estimate over a span, with parabolic refinement. Returns 0 when
// no period clears the aperiodicity threshold (noise, silence).
inline double pitch_hz(const Span& s, double rate, double fmin = 30.0, double fmax = 4000.0,
                       double threshold = 0.15) {
  const size_t max_lag = std::min(s.size() / 2, static_cast<size_t>(rate / fmin));
  const size_t min_lag = std::max<size_t>(2, static_cast<size_t>(rate / fmax));
  if (max_lag <= min_lag + 2) return 0;
  const size_t n = s.size() - max_lag;
  std::vector<double> d(max_lag + 1, 0.0);
  for (size_t lag = 1; lag <= max_lag; ++lag) {
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
      const double diff = static_cast<double>(s[i]) - s[i + lag];
      sum += diff * diff;
    }
    d[lag] = sum;
  }
  std::vector<double> cmnd(max_lag + 1, 1.0);
  double running = 0;
  for (size_t lag = 1; lag <= max_lag; ++lag) {
    running += d[lag];
    cmnd[lag] = running > 0 ? d[lag] * static_cast<double>(lag) / running : 1.0;
  }
  size_t best = 0;
  for (size_t lag = min_lag; lag < max_lag; ++lag) {
    if (cmnd[lag] < threshold) {
      while (lag + 1 < max_lag && cmnd[lag + 1] < cmnd[lag]) ++lag;
      best = lag;
      break;
    }
  }
  if (!best) return 0;
  const double a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
  const double denom = a - 2 * b + c;
  const double shift = std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
  return rate / (static_cast<double>(best) + std::clamp(shift, -1.0, 1.0));
}

// Pitch every `hop_s` seconds over `window_s` windows; 0 where unvoiced.
inline std::vector<double> pitch_track(const std::vector<float>& x, double rate, double hop_s,
                                       double window_s, double fmin = 30.0, double fmax = 4000.0) {
  const size_t hop = static_cast<size_t>(rate * hop_s), win = static_cast<size_t>(rate * window_s);
  std::vector<double> out;
  for (size_t p = 0; p + win <= x.size(); p += hop) out.push_back(pitch_hz(Span(x, p, win), rate, fmin, fmax));
  return out;
}

// Amplitude of the component at `hz` (Goertzel over a Hann window), scaled so a
// unit sine reads 1.0.
inline double tone_amplitude(const Span& s, double rate, double hz) {
  const size_t n = s.size();
  if (n < 2) return 0;
  const double w = 2.0 * kPi * hz / rate;
  double re = 0, im = 0, norm = 0;
  for (size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
    re += s[i] * win * std::cos(w * static_cast<double>(i));
    im -= s[i] * win * std::sin(w * static_cast<double>(i));
    norm += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / norm;
}

inline std::vector<double> harmonics(const Span& s, double rate, double f0, int count) {
  std::vector<double> out;
  for (int k = 1; k <= count; ++k) out.push_back(tone_amplitude(s, rate, f0 * k));
  return out;
}

inline void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Averaged Hann-windowed power spectrum (2^order points) over a span.
inline std::vector<double> power_spectrum(const Span& s, int order = 13) {
  const size_t n = size_t{1} << order;
  std::vector<double> power(n / 2, 0.0);
  int frames = 0;
  for (size_t at = 0; at + n <= s.size(); at += n / 2, ++frames) {
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i)
      a[i] = s[at + i] * (0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1)));
    fft(a);
    for (size_t k = 0; k < n / 2; ++k) power[k] += std::norm(a[k]);
  }
  if (frames) for (double& p : power) p /= frames;
  return power;
}

inline double spectral_centroid(const Span& s, double rate, int order = 13) {
  const auto p = power_spectrum(s, order);
  double num = 0, den = 0;
  for (size_t k = 1; k < p.size(); ++k) {
    const double hz = static_cast<double>(k) * rate / static_cast<double>(p.size() * 2);
    num += hz * p[k];
    den += p[k];
  }
  return den > 0 ? num / den : 0.0;
}

// Fraction of power in [lo, hi) Hz.
inline double band_fraction(const Span& s, double rate, double lo, double hi, int order = 13) {
  const auto p = power_spectrum(s, order);
  double in = 0, all = 0;
  for (size_t k = 1; k < p.size(); ++k) {
    const double hz = static_cast<double>(k) * rate / static_cast<double>(p.size() * 2);
    all += p[k];
    if (hz >= lo && hz < hi) in += p[k];
  }
  return all > 0 ? in / all : 0.0;
}

// High-time fraction of a two-level signal, thresholded at its midpoint.
inline double duty(const Span& s) {
  if (!s.size()) return 0;
  double lo = s[0], hi = s[0];
  for (size_t i = 0; i < s.size(); ++i) { lo = std::min<double>(lo, s[i]); hi = std::max<double>(hi, s[i]); }
  const double mid = (lo + hi) / 2;
  size_t high = 0;
  for (size_t i = 0; i < s.size(); ++i) high += s[i] > mid;
  return static_cast<double>(high) / static_cast<double>(s.size());
}

// Normalized autocorrelation at `lag` samples (1 = the span repeats exactly).
inline double autocorrelation(const Span& s, size_t lag) {
  if (lag >= s.size()) return 0;
  const double m = mean(s);
  double num = 0, den = 0;
  for (size_t i = 0; i + lag < s.size(); ++i) {
    num += (s[i] - m) * (s[i + lag] - m);
    den += (s[i] - m) * (s[i] - m);
  }
  return den > 0 ? num / den : 0.0;
}

// Pearson correlation of two equal-length spans.
inline double correlation(const Span& a, const Span& b) {
  const size_t n = std::min(a.size(), b.size());
  const double ma = mean(a), mb = mean(b);
  double num = 0, da = 0, dbb = 0;
  for (size_t i = 0; i < n; ++i) {
    num += (a[i] - ma) * (b[i] - mb);
    da += (a[i] - ma) * (a[i] - ma);
    dbb += (b[i] - mb) * (b[i] - mb);
  }
  return da > 0 && dbb > 0 ? num / std::sqrt(da * dbb) : 0.0;
}

// First/last contour index at or above `threshold_db`; -1 if none.
inline long first_at_or_above(const std::vector<double>& c, double threshold_db, size_t from = 0) {
  for (size_t i = from; i < c.size(); ++i) if (c[i] >= threshold_db) return static_cast<long>(i);
  return -1;
}
inline long last_at_or_above(const std::vector<double>& c, double threshold_db) {
  for (size_t i = c.size(); i > 0; --i) if (c[i - 1] >= threshold_db) return static_cast<long>(i - 1);
  return -1;
}

// Dominant modulation frequency of a series sampled every `hop_s` seconds, by
// the autocorrelation peak of its mean-removed values (for vibrato and LFO rates).
inline double modulation_hz(const std::vector<double>& series, double hop_s, double fmin, double fmax) {
  std::vector<double> x;
  for (double v : series) if (v > 0) x.push_back(v);
  if (x.size() < 8) return 0;
  const double m = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
  for (double& v : x) v -= m;
  const size_t min_lag = std::max<size_t>(1, static_cast<size_t>(1.0 / (fmax * hop_s)));
  const size_t max_lag = std::min(x.size() / 2, static_cast<size_t>(1.0 / (fmin * hop_s)) + 1);
  std::vector<double> r(max_lag + 2, -2.0);
  for (size_t lag = 1; lag <= max_lag + 1 && lag < x.size(); ++lag) {
    double num = 0, den = 0;
    for (size_t i = 0; i + lag < x.size(); ++i) { num += x[i] * x[i + lag]; den += x[i] * x[i]; }
    r[lag] = den > 0 ? num / den : 0;
  }
  // A period peak only counts once the autocorrelation has swung negative (past
  // the half-period trough); before that, jitter makes local bumps.
  size_t start = 1;
  while (start <= max_lag && r[start] >= 0) ++start;
  start = std::max(start, min_lag);
  double best = -2;
  for (size_t lag = start; lag <= max_lag; ++lag) best = std::max(best, r[lag]);
  if (best < 0.3) return 0;
  // The first strong peak is the period; its multiples score as high.
  for (size_t lag = std::max<size_t>(start, 1); lag <= max_lag; ++lag) {
    if (r[lag] >= 0.85 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
      const double a = r[lag - 1], b = r[lag], c = r[lag + 1], denom = a - 2 * b + c;
      const double shift = std::abs(denom) > 1e-12 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
      return 1.0 / ((static_cast<double>(lag) + shift) * hop_s);
    }
  }
  return 0;
}

}  // namespace measure
