#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
  if (source.mono.empty()) return {};
  std::vector<double> out(frames);
  for (size_t i = 0; i < frames; ++i) {
    const double pos = i * static_cast<double>(source.rate) / rate;
    const size_t a = std::min(source.mono.size() - 1, static_cast<size_t>(pos)),
                 b = std::min(source.mono.size() - 1, a + 1);
    out[i] = source.mono[a] * (1 - (pos - a)) + source.mono[b] * (pos - a);
  }
  return out;
}
static std::vector<double> envelope(const std::vector<double> &x,
                                    size_t block) {
  std::vector<double> out;
  for (size_t p = 0; p + block <= x.size(); p += block) {
    double e = 0;
    for (size_t i = 0; i < block; ++i)
      e += x[p + i] * x[p + i];
    out.push_back(std::sqrt(e / block));
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
  ma /= n;
  mb /= n;
  double aa = 0, bb = 0, ab = 0;
  for (size_t i = 0; i < n; ++i) {
    const double x = a[i] - ma, y = b[i] - mb;
    aa += x * x;
    bb += y * y;
    ab += x * y;
  }
  return ab / std::sqrt(std::max(1e-30, aa * bb));
}
struct Active {
  size_t first{}, last{};
};
static Active active(const std::vector<double> &e) {
  if (e.empty()) return {};
  const double peak = *std::max_element(e.begin(), e.end()),
               threshold = peak * 0.05;
  Active a{};
  while (a.first < e.size() && e[a.first] < threshold)
    ++a.first;
  a.last = e.size();
  while (a.last > a.first && e[a.last - 1] < threshold)
    --a.last;
  return a;
}
static double pitch(const std::vector<double> &x, uint32_t rate, size_t begin,
                    size_t end) {
  constexpr size_t n = 4096;
  const size_t available = end > begin ? end - begin : 0;
  if (available < n)
    return 0;
  const size_t at = begin + (available - n) / 2;
  std::vector<double> magnitude;
  constexpr double pitch_step = 0.25;
  for (double freq = 40; freq <= 4000; freq += pitch_step) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
      const double win =
                       0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (n - 1)),
                   phase = 2 * std::numbers::pi * freq * i / rate;
      re += x[at + i] * win * std::cos(phase);
      im -= x[at + i] * win * std::sin(phase);
    }
    magnitude.push_back(std::sqrt(re * re + im * im));
  }
  const auto peak = std::max_element(magnitude.begin(), magnitude.end());
  return peak == magnitude.end() ? 0 : 40.0 + std::distance(magnitude.begin(), peak) * pitch_step;
}
static double cosine_similarity(const std::vector<double> &a,
                                const std::vector<double> &b) {
  const size_t n = std::min(a.size(), b.size());
  if (!n) return 0;
  double aa = 0, bb = 0, ab = 0;
  for (size_t i = 0; i < n; ++i) {
    aa += a[i] * a[i]; bb += b[i] * b[i]; ab += a[i] * b[i];
  }
  return ab / std::sqrt(std::max(1e-30, aa * bb));
}
static double magnitude(const std::vector<double> &x, size_t at, size_t n,
                        uint32_t rate, double freq) {
  double re = 0, im = 0;
  for (size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (n - 1)),
                 phase = 2 * std::numbers::pi * freq * i / rate;
    re += x[at + i] * win * std::cos(phase);
    im -= x[at + i] * win * std::sin(phase);
  }
  return std::sqrt(re * re + im * im);
}
static std::vector<double> tonal_spectrum(const std::vector<double> &x,
                                          size_t begin, size_t end,
                                          uint32_t rate, double fundamental) {
  constexpr size_t n = 4096;
  std::vector<double> out;
  if (end - begin < n || fundamental <= 0)
    return out;
  const size_t at = begin + (end - begin - n) / 2;
  for (int harmonic = 1; harmonic <= 32 && fundamental * harmonic < rate * 0.45;
       ++harmonic) {
    const double center = fundamental * harmonic;
    double energy = 0;
    for (double detune : {-2.0, -1.0, 0.0, 1.0, 2.0}) {
      const double m = magnitude(x, at, n, rate, center + detune);
      energy += m * m;
    }
    out.push_back(std::sqrt(energy));
  }
  return out;
}
static std::vector<double> noise_spectrum(const std::vector<double> &x,
                                          size_t begin, size_t end,
                                          uint32_t rate) {
  constexpr size_t n = 8192, bands = 16, probes = 12, windows = 5;
  std::vector<double> out(bands);
  if (end - begin < n)
    return out;
  for (size_t band = 0; band < bands; ++band) {
    const double lo = 40 * std::pow(16000.0 / 40.0,
                                    static_cast<double>(band) / bands),
                 hi = 40 * std::pow(16000.0 / 40.0,
                                    static_cast<double>(band + 1) / bands);
    double energy = 0;
    for (size_t window = 0; window < windows; ++window) {
      const size_t span = end - begin - n;
      const size_t at = begin + (windows == 1 ? span / 2 : span * window / (windows - 1));
      for (size_t probe = 0; probe < probes; ++probe) {
        const double freq = lo * std::pow(hi / lo, (probe + 0.5) / probes),
                     m = magnitude(x, at, n, rate, freq);
        energy += m * m;
      }
    }
    out[band] = std::log10(1e-12 + energy / (probes * windows));
  }
  return out;
}
int main(int argc, char **argv) {
  if (argc != 5) {
    std::cerr << "usage: yanes-parity-compare reference.wav candidate.wav "
                 "tonal|noise minimum-similarity\n";
    return 2;
  }
  const bool noise = std::strcmp(argv[3], "noise") == 0;
  if (!noise && std::strcmp(argv[3], "tonal"))
    return 2;
  char *threshold_end = nullptr;
  const double minimum = std::strtod(argv[4], &threshold_end);
  if (!threshold_end || *threshold_end || minimum < 0 || minimum > 1) return 2;
  Wav a = load(argv[1]), raw = load(argv[2]);
  if (!a.rate || !raw.rate || a.mono.empty() || raw.mono.empty())
    return 1;
  const size_t frames =
      std::min(a.mono.size(),
               static_cast<size_t>(raw.mono.size() *
                                   static_cast<double>(a.rate) / raw.rate));
  a.mono.resize(frames);
  const auto b = resample(raw, a.rate, frames);
  const auto ea = envelope(a.mono, 256), eb = envelope(b, 256);
  const Active aa = active(ea), ab = active(eb);
  const double env = correlation(ea, eb);
  const double onset = std::abs(static_cast<double>(aa.first) - ab.first) *
                       256 / a.rate,
               offset = std::abs(static_cast<double>(aa.last) - ab.last) * 256 /
                        a.rate;
  const size_t ba = aa.first * 256, eaEnd = std::min(frames, aa.last * 256),
               bb = ab.first * 256, ebEnd = std::min(frames, ab.last * 256);
  const double pa = noise ? 0 : pitch(a.mono, a.rate, ba, eaEnd),
               pb = noise ? 0 : pitch(b, a.rate, bb, ebEnd);
  const auto sa = noise ? noise_spectrum(a.mono, ba, eaEnd, a.rate)
                        : tonal_spectrum(a.mono, ba, eaEnd, a.rate, pa);
  const auto sb = noise ? noise_spectrum(b, bb, ebEnd, a.rate)
                        : tonal_spectrum(b, bb, ebEnd, a.rate, pb);
  const double spectral = noise ? correlation(sa, sb) : cosine_similarity(sa, sb);
  const double cents =
      noise
          ? 0
          : 1200 * std::abs(std::log2(std::max(1e-9, pb) / std::max(1e-9, pa)));
  const bool pass = env >= minimum && spectral >= minimum && onset <= 0.03 &&
                    offset <= 0.03 && (noise || cents <= 20);
  std::cout << "envelope=" << env << " spectrum=" << spectral
            << " onset_delta_ms=" << onset * 1000
            << " offset_delta_ms=" << offset * 1000;
  if (!noise)
    std::cout << " reference_hz=" << pa << " candidate_hz=" << pb
              << " pitch_cents=" << cents;
  std::cout << " result=" << (pass ? "pass" : "fail") << '\n';
  return pass ? 0 : 1;
}
