#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <numbers>
#include <vector>

static uint16_t u16(const std::vector<uint8_t> &b, size_t p) {
  return static_cast<uint16_t>(b[p] | (b[p + 1] << 8U));
}
static uint32_t u32(const std::vector<uint8_t> &b, size_t p) {
  return static_cast<uint32_t>(u16(b, p) | (u16(b, p + 2) << 16U));
}

struct Wav {
  uint32_t rate{};
  std::vector<double> mono;
};

static Wav load(const char *path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
  Wav w;
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) ||
      std::memcmp(b.data() + 8, "WAVE", 4))
    return w;
  uint16_t format = 0, bits = 0, chans = 0;
  size_t at = 0, n = 0;
  for (size_t p = 12; p + 8 <= b.size();) {
    const uint32_t z = u32(b, p + 4);
    if (p + 8U + z > b.size())
      return {};
    if (!std::memcmp(b.data() + p, "fmt ", 4) && z >= 16) {
      format = u16(b, p + 8);
      chans = u16(b, p + 10);
      w.rate = u32(b, p + 12);
      bits = u16(b, p + 22);
    }
    if (!std::memcmp(b.data() + p, "data", 4)) {
      at = p + 8;
      n = z;
    }
    p += 8U + z + (z & 1U);
  }
  if (format != 1 || bits != 16 || !chans || !w.rate || !at)
    return {};
  const size_t frames = n / (2U * chans);
  w.mono.reserve(frames);
  for (size_t f = 0; f < frames; ++f) {
    double x = 0;
    for (uint16_t c = 0; c < chans; ++c)
      x += static_cast<int16_t>(u16(b, at + (f * chans + c) * 2U)) / 32768.0;
    w.mono.push_back(x / chans);
  }
  return w;
}

static std::vector<double> resample(const Wav &source, uint32_t rate,
                                    size_t frames) {
  if (source.mono.empty())
    return {};
  std::vector<double> out(frames);
  for (size_t i = 0; i < frames; ++i) {
    const double pos = i * static_cast<double>(source.rate) / rate;
    const size_t a = std::min(source.mono.size() - 1, static_cast<size_t>(pos)),
                 b = std::min(source.mono.size() - 1, a + 1);
    out[i] = source.mono[a] * (1 - (pos - a)) + source.mono[b] * (pos - a);
  }
  return out;
}

static void dc_block(std::vector<double> &x) {
  // Per-channel emulator captures can carry the Game Boy DAC's static bias.
  // Starting the one-pole blocker at zero turns that harmless bias into a fake
  // onset; center the whole excerpt first so silent isolated channels remain
  // silent while the blocker still removes time-varying coupling drift.
  if (!x.empty()) {
    const double mean = std::accumulate(x.begin(), x.end(), 0.0) /
                        static_cast<double>(x.size());
    for (double& s : x) s -= mean;
  }
  double prev_x = 0, prev_y = 0;
  for (double &s : x) {
    const double y = s - prev_x + 0.995 * prev_y;
    prev_x = s;
    prev_y = y;
    s = y;
  }
}

static std::vector<double> envelope(const std::vector<double> &x, size_t hop) {
  std::vector<double> out;
  if (!hop)
    return out;
  for (size_t p = 0; p + hop <= x.size(); p += hop) {
    double e = 0;
    for (size_t i = 0; i < hop; ++i)
      e += x[p + i] * x[p + i];
    out.push_back(std::sqrt(e / static_cast<double>(hop)));
  }
  return out;
}

// Centred moving average over 2*half+1 envelope blocks.
static std::vector<double> smooth(const std::vector<double> &x, size_t half) {
  if (!half || x.size() < 2 * half + 1)
    return x;
  std::vector<double> out(x.size());
  std::vector<double> prefix(x.size() + 1);
  std::partial_sum(x.begin(), x.end(), prefix.begin() + 1);
  for (size_t i = 0; i < x.size(); ++i) {
    const size_t lo = i > half ? i - half : 0,
                 hi = std::min(x.size() - 1, i + half);
    out[i] = (prefix[hi + 1] - prefix[lo]) /
             static_cast<double>(hi - lo + 1);
  }
  return out;
}

static double correlation(const std::vector<double> &a,
                          const std::vector<double> &b) {
  const size_t n = std::min(a.size(), b.size());
  if (n < 2)
    return 0;
  double ma = 0, mb = 0;
  for (size_t i = 0; i < n; ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<double>(n);
  mb /= static_cast<double>(n);
  double aa = 0, bb = 0, ab = 0;
  for (size_t i = 0; i < n; ++i) {
    const double x = a[i] - ma, y = b[i] - mb;
    aa += x * x;
    bb += y * y;
    ab += x * y;
  }
  const double var = std::sqrt(std::max(1e-30, aa * bb));
  // Flat envelopes (steady tones) are not a shape mismatch.
  if (aa < 1e-18 && bb < 1e-18)
    return 1;
  if (aa < 1e-18 || bb < 1e-18)
    return 0;
  return ab / var;
}

struct Active {
  size_t first{}, last{};
};

static Active active(const std::vector<double> &e) {
  if (e.empty())
    return {};
  const double peak = *std::max_element(e.begin(), e.end()),
               threshold = peak * 0.08;
  Active a{};
  while (a.first < e.size() && e[a.first] < threshold)
    ++a.first;
  a.last = e.size();
  while (a.last > a.first && e[a.last - 1] < threshold)
    --a.last;
  return a;
}

static void shift_pair(std::vector<double> &a, std::vector<double> &b,
                       int lag_samples) {
  if (lag_samples > 0) {
    const size_t lag = static_cast<size_t>(lag_samples);
    if (lag >= b.size())
      return;
    b.erase(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(lag));
  } else if (lag_samples < 0) {
    const size_t lag = static_cast<size_t>(-lag_samples);
    if (lag >= a.size())
      return;
    a.erase(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(lag));
  }
  const size_t n = std::min(a.size(), b.size());
  a.resize(n);
  b.resize(n);
}

static void fft(std::vector<std::complex<double>> &x) {
  const size_t n = x.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(x[i], x[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> wlen(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const auto u = x[i + j], v = x[i + j + len / 2] * w;
        x[i + j] = u + v;
        x[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

// Largest power of two that fits the span, capped at 4096. Short fixtures (the
// SID blips are under 100 ms) still have to yield a spectrum; a fixed 4096-point
// window silently returned nothing for them and scored the pair as a mismatch.
static size_t window_for(size_t span) {
  size_t n = 512;
  while (n * 2 <= span && n < 4096)
    n *= 2;
  return n <= span ? n : 0;
}

static std::vector<double> log_bands(const std::vector<double> &x, size_t begin,
                                     size_t end, uint32_t rate) {
  constexpr size_t bands = 24, hops = 6;
  std::vector<double> energy(bands);
  if (end <= begin)
    return {};
  const size_t n = window_for(end - begin);
  if (!n)
    return {};
  const double lo = 40, hi = 16000;
  const size_t span = end - begin - n;
  size_t used = 0;
  for (size_t hop = 0; hop < hops; ++hop) {
    const size_t at = begin + (hops == 1 ? 0 : span * hop / (hops - 1));
    std::vector<std::complex<double>> spec(n);
    for (size_t i = 0; i < n; ++i) {
      const double win =
          0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) /
                               static_cast<double>(n - 1));
      spec[i] = x[at + i] * win;
    }
    fft(spec);
    for (size_t k = 1; k < n / 2; ++k) {
      const double freq = static_cast<double>(k) * rate / static_cast<double>(n);
      if (freq < lo || freq >= hi)
        continue;
      const double pos = static_cast<double>(bands) * std::log(freq / lo) /
                         std::log(hi / lo);
      const size_t band = std::min(bands - 1, static_cast<size_t>(pos));
      energy[band] += std::norm(spec[k]);
    }
    ++used;
  }
  if (!used)
    return {};
  for (double &e : energy)
    e = std::log10(1e-12 + e / static_cast<double>(used));
  // Floor each spectrum 35 dB under its own strongest band. Below that a band
  // holds nothing but whichever renderer's own noise floor, and comparing two
  // noise floors is not a comparison of timbre: it let bands with no signal in
  // them outvote the harmonics that carry the sound.
  const double floor = *std::max_element(energy.begin(), energy.end()) - 3.5;
  for (double &e : energy)
    e = std::max(e, floor);
  return energy;
}

// Two box passes, first null near 8 kHz. Pitch detection only needs the low end,
// and wide-band chip artifacts (the N163 multiplexes its channels up around
// 15 kHz) otherwise drag YIN's minimum off the true period by tens of cents.
static std::vector<double> pitch_prefilter(const std::vector<double> &x,
                                           uint32_t rate, size_t at, size_t n) {
  const size_t taps = std::max<size_t>(rate / 8000U, 1);
  std::vector<double> out(x.begin() + static_cast<std::ptrdiff_t>(at),
                          x.begin() + static_cast<std::ptrdiff_t>(at + n));
  if (taps < 2)
    return out;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<double> next(out.size());
    double sum = 0;
    for (size_t i = 0; i < out.size(); ++i) {
      sum += out[i];
      if (i >= taps)
        sum -= out[i - taps];
      next[i] = sum / static_cast<double>(std::min(i + 1, taps));
    }
    out.swap(next);
  }
  return out;
}

static double yin_hz(const std::vector<double> &raw, uint32_t rate, size_t begin,
                     size_t end) {
  if (end <= begin)
    return 0;
  const size_t n = window_for(end - begin);
  if (!n)
    return 0;
  const std::vector<double> x =
      pitch_prefilter(raw, rate, begin + (end - begin - n) / 2, n);
  const size_t at = 0;
  // Fixtures reach ~2 kHz an octave up, so a 1500 Hz ceiling forced YIN to
  // report a subharmonic for the highest notes.
  const size_t min_lag = std::max<size_t>(rate / 5000U, 2),
               max_lag = std::min<size_t>(n / 2, rate / 40U);
  if (max_lag <= min_lag)
    return 0;
  std::vector<double> d(max_lag + 1);
  for (size_t tau = 1; tau <= max_lag; ++tau) {
    double s = 0;
    for (size_t i = 0; i + tau < n; ++i) {
      const double diff = x[at + i] - x[at + i + tau];
      s += diff * diff;
    }
    d[tau] = s;
  }
  double running = 0;
  std::vector<double> cmnd(max_lag + 1, 1);
  for (size_t tau = 1; tau <= max_lag; ++tau) {
    running += d[tau];
    cmnd[tau] = d[tau] * static_cast<double>(tau) / std::max(1e-30, running);
  }
  size_t tau = min_lag;
  constexpr double threshold = 0.15;
  for (; tau + 1 < max_lag; ++tau) {
    if (cmnd[tau] < threshold && cmnd[tau] <= cmnd[tau + 1] &&
        cmnd[tau] <= cmnd[tau - 1])
      break;
  }
  if (tau + 1 >= max_lag) {
    tau = static_cast<size_t>(
        std::min_element(cmnd.begin() + static_cast<std::ptrdiff_t>(min_lag),
                         cmnd.end()) -
        cmnd.begin());
  }
  if (tau <= 1 || tau >= max_lag)
    return 0;
  // Octave correction. A wavetable voice whose period is not bit-identical from
  // one cycle to the next (the N163 time-multiplexes its channels) dips harder
  // at twice or three times the true period, which reads back as a spurious
  // pitch error. Prefer the shortest submultiple that is nearly as periodic.
  for (size_t divisor = 8; divisor >= 2; --divisor) {
    const size_t target = tau / divisor;
    size_t best_cand = 0;
    const size_t lo = target > 2 ? std::max(min_lag, target - 2) : min_lag;
    const size_t hi = std::min(max_lag - 1, target + 3);
    for (size_t cand = lo; cand < hi; ++cand) {
      if (cand > 1 && cand + 1 < max_lag) {
        if (cmnd[cand] <= cmnd[cand - 1] && cmnd[cand] <= cmnd[cand + 1]) {
          if (!best_cand || cmnd[cand] < cmnd[best_cand])
            best_cand = cand;
        }
      }
    }
    if (best_cand) {
      if (cmnd[best_cand] < std::max(0.4, cmnd[tau] * 1.5 + 0.1)) {
        tau = best_cand;
        break;
      }
    } else {
      const size_t candidate = target;
      if (candidate >= min_lag && cmnd[candidate] < 0.4 &&
          cmnd[candidate] < cmnd[tau] * 2 + 0.05) {
        tau = candidate;
        break;
      }
    }
  }
  const double s0 = cmnd[tau - 1], s1 = cmnd[tau], s2 = cmnd[tau + 1];
  const double den = 2 * (s0 - 2 * s1 + s2);
  const double adj = std::abs(den) < 1e-12 ? 0 : (s0 - s2) / den;
  const double period = static_cast<double>(tau) + adj;
  if (period <= 1)
    return 0;
  return static_cast<double>(rate) / period;
}

struct Report {
  double env{};
  double spectral{};
  double onset{};
  double offset{};
  double cents{};
  double align_ms{};
  double ref_hz{};
  double cand_hz{};
  bool ok{};
};

static Report compare(std::vector<double> a, std::vector<double> b,
                      uint32_t rate, bool noise, double minimum, bool rom = false, bool allow_silent = false) {
  Report r{};
  dc_block(a);
  dc_block(b);
  const auto rms = [](const std::vector<double>& x) {
    double sum = 0;
    for (double value : x) sum += value * value;
    return x.empty() ? 0.0 : std::sqrt(sum / x.size());
  };
  // Muted emulator channels often retain a constant DAC bias. Once centered,
  // two genuinely silent sides are a valid isolated-channel pass.
  if (rms(a) < 1e-4 && rms(b) < 1e-4) {
    r.env = r.spectral = 1.0;
    r.ok = allow_silent;
    return r;
  }
  constexpr size_t hop = 256;
  const Active pre_a = active(envelope(a, hop)), pre_b = active(envelope(b, hop));
  int lag = static_cast<int>(pre_b.first) - static_cast<int>(pre_a.first);
  const int max_lag =
      static_cast<int>(std::max(1.0, 0.08 * static_cast<double>(rate) / hop));
  lag = std::clamp(lag, -max_lag, max_lag);
  r.align_ms = static_cast<double>(lag) * static_cast<double>(hop) /
               static_cast<double>(rate) * 1000.0;
  shift_pair(a, b, lag * static_cast<int>(hop));
  const auto ea = envelope(a, hop), eb = envelope(b, hop);
  // Correlate the amplitude contour, not the noise realisation. Two independent
  // LFSRs agree on the shape of a note but their block-to-block RMS jitter is
  // uncorrelated by construction, so a raw 5 ms envelope scores a perfectly good
  // noise channel near zero. Onset and offset still use the unsmoothed envelope.
  // ROM mixes also contain phase beating between several channels.  That is
  // not a gain-envelope difference and will vary between two otherwise-correct
  // oscillators whose reset phases are not observable in the register stream.
  // A ~370 ms contour retains musical dynamics while averaging that beating.
  const size_t envelope_half_window = rom ? 32 : (noise ? 8 : 2);
  r.env = correlation(smooth(ea, envelope_half_window),
                      smooth(eb, envelope_half_window));
  const Active aa = active(ea), ab = active(eb);
  r.onset = std::abs(static_cast<double>(aa.first) - static_cast<double>(ab.first)) *
            static_cast<double>(hop) / static_cast<double>(rate);
  r.offset = std::abs(static_cast<double>(aa.last) - static_cast<double>(ab.last)) *
             static_cast<double>(hop) / static_cast<double>(rate);
  const size_t ba = aa.first * hop, ea_end = std::min(a.size(), aa.last * hop),
               bb = ab.first * hop, eb_end = std::min(b.size(), ab.last * hop);
  const size_t sa0 = std::max(ba, bb), se0 = std::min(ea_end, eb_end);
  const auto spec_a = log_bands(a, sa0, se0, rate);
  const auto spec_b = log_bands(b, sa0, se0, rate);
  r.spectral = correlation(spec_a, spec_b);
  if (!noise) {
    r.ref_hz = yin_hz(a, rate, ba, ea_end);
    r.cand_hz = yin_hz(b, rate, bb, eb_end);
    if (r.ref_hz >= 40 && r.cand_hz >= 40) {
      const double ratio = r.cand_hz / r.ref_hz;
      const double oct = std::round(std::log2(ratio));
      r.cents = 1200 * std::abs(std::log2(ratio / std::pow(2.0, oct)));
    } else {
      r.cents = 999;
    }
  }
  // A ROM excerpt is a continuous multi-voice program, not a single-note
  // fixture: one side can legitimately be active at both file boundaries, so
  // onset/offset and monophonic YIN are undefined. It still has to clear both
  // the time-varying energy contour and log-band spectrum gates.
  r.ok = r.env >= minimum && r.spectral >= minimum &&
         (rom || (r.onset <= 0.05 && r.offset <= 0.05 &&
                  (noise || r.cents <= 20)));
  return r;
}

static void print_report(const Report &r, bool noise) {
  std::cout << "envelope=" << r.env << " spectrum=" << r.spectral
            << " onset_delta_ms=" << r.onset * 1000
            << " offset_delta_ms=" << r.offset * 1000
            << " align_ms=" << r.align_ms;
  if (!noise)
    std::cout << " reference_hz=" << r.ref_hz << " candidate_hz=" << r.cand_hz
              << " pitch_cents=" << r.cents;
  std::cout << " result=" << (r.ok ? "pass" : "fail") << '\n';
}

static std::vector<double> tone(uint32_t rate, double hz, double seconds,
                                double phase, int shape, double extra_ms) {
  const size_t n = static_cast<size_t>(seconds * rate);
  std::vector<double> x(n);
  const size_t attack = rate / 50, rel = rate / 10;
  const size_t note_end =
      n - rel - static_cast<size_t>(std::max(0.0, extra_ms) * 1e-3 * rate);
  for (size_t i = 0; i < n; ++i) {
    double env = 0;
    if (i < attack)
      env = static_cast<double>(i) / attack;
    else if (i < note_end)
      env = 1;
    else if (i < note_end + rel)
      env = 1 - static_cast<double>(i - note_end) / rel;
    const double t = static_cast<double>(i) / rate;
    const double ph = 2 * std::numbers::pi * hz * t + phase;
    double s = std::sin(ph);
    if (shape == 1)
      s = s >= 0 ? 1 : -1;
    x[i] = env * s * 0.25;
  }
  return x;
}

// White noise with the same note contour as tone(), from a caller-chosen seed
// so two "independent chips" can be simulated. decay_per_second > 0 fades the
// sustain, which a fair gate still has to notice.
static std::vector<double> noise_burst(uint32_t rate, double seconds,
                                       uint32_t seed, double decay_per_second) {
  const size_t n = static_cast<size_t>(seconds * rate);
  std::vector<double> x(n);
  const size_t attack = rate / 50, rel = rate / 10, note_end = n - rel;
  uint32_t state = seed;
  for (size_t i = 0; i < n; ++i) {
    state = state * 1664525U + 1013904223U;
    double env = 0;
    if (i < attack)
      env = static_cast<double>(i) / attack;
    else if (i < note_end)
      env = 1;
    else
      env = 1 - static_cast<double>(i - note_end) / rel;
    const double t = static_cast<double>(i) / rate;
    env *= std::exp(-decay_per_second * t);
    x[i] = env * ((state >> 16U) & 1U ? 0.25 : -0.25);
  }
  return x;
}

static int self_test() {
  constexpr uint32_t rate = 48000;
  const auto ref = tone(rate, 440, 1.2, 0, 0, 0);
  struct Case {
    const char *name;
    std::vector<double> cand;
    bool noise;
    bool expect;
  };
  const Case cases[] = {
      {"delay_40ms",
       [] {
         auto x = tone(rate, 440, 1.2, 0, 0, 0);
         x.insert(x.begin(), static_cast<size_t>(0.04 * rate), 0);
         x.resize(static_cast<size_t>(1.2 * rate), 0);
         return x;
       }(),
       false, true},
      {"phase", tone(rate, 440, 1.2, 1.2, 0, 0), false, true},
      {"loud",
       [] {
         auto x = tone(rate, 440, 1.2, 0, 0, 0);
         for (double &s : x)
           s *= 4;
         return x;
       }(),
       false, true},
      {"square", tone(rate, 440, 1.2, 0, 1, 0), false, false},
      {"sharp_50c", tone(rate, 440 * std::pow(2.0, 50.0 / 1200.0), 1.2, 0, 0, 0),
       false, false},
      {"short_80ms", tone(rate, 440, 1.2, 0, 0, 80), false, false},
      // Two chips never share an LFSR seed, so a different noise realisation
      // with the same contour has to pass...
      {"noise_other_seed", noise_burst(rate, 1.2, 0x1234, 0), true, true},
      // ...but a sustain that fades away is a real envelope difference.
      {"noise_decaying", noise_burst(rate, 1.2, 0x1234, 3.0), true, false},
  };
  const auto noise_reference = noise_burst(rate, 1.2, 0x9e37, 0);
  int failed = 0;
  const std::vector<double> silence(rate, 0.0);
  if (compare(silence, silence, rate, false, 0.8).ok) ++failed;
  if (compare(silence, silence, rate, true, 0.8, true).ok) ++failed;
  if (!compare(silence, silence, rate, true, 0.8, true, true).ok) ++failed;
  for (const auto &c : cases) {
    const Report r =
        compare(c.noise ? noise_reference : ref, c.cand, rate, c.noise, 0.8);
    const bool ok = r.ok == c.expect;
    std::cout << "self-test " << c.name << " " << (ok ? "ok" : "BAD")
              << " (expected " << (c.expect ? "pass" : "fail") << " got "
              << (r.ok ? "pass" : "fail") << ") ";
    print_report(r, c.noise);
    if (!ok)
      ++failed;
  }
  return failed ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
    return self_test();
  if (argc != 5) {
    std::cerr << "usage: yanes-parity-compare reference.wav candidate.wav "
                 "tonal|noise|rom|rom-mix minimum-similarity\n"
              << "       yanes-parity-compare --self-test\n";
    return 2;
  }
  const bool isolated_rom = std::strcmp(argv[3], "rom") == 0;
  const bool rom = isolated_rom || std::strcmp(argv[3], "rom-mix") == 0;
  const bool noise = rom || std::strcmp(argv[3], "noise") == 0;
  if (!noise && std::strcmp(argv[3], "tonal"))
    return 2;
  char *threshold_end = nullptr;
  const double minimum = std::strtod(argv[4], &threshold_end);
  if (threshold_end == argv[4] || *threshold_end || !std::isfinite(minimum) || minimum < 0 || minimum > 1)
    return 2;
  Wav a = load(argv[1]), raw = load(argv[2]);
  if (!a.rate || !raw.rate || a.mono.empty() || raw.mono.empty())
    return 1;
  const size_t frames = std::min(
      a.mono.size(), static_cast<size_t>(raw.mono.size() *
                                         static_cast<double>(a.rate) / raw.rate));
  a.mono.resize(frames);
  auto b = resample(raw, a.rate, frames);
  const Report r = compare(std::move(a.mono), std::move(b), a.rate, noise, minimum, rom, isolated_rom);
  print_report(r, noise);
  return r.ok ? 0 : 1;
}
