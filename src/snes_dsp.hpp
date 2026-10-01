#pragma once

// Clean-room model of the SNES S-DSP sample path: the hardware's 4-point Gaussian
// interpolator, BRR (Bit Rate Reduction) decoding with its four predictor filters
// and block-aligned loop, a straightforward BRR encoder for importing 16-bit PCM,
// and the S-DSP echo (an 8-tap FIR into a feedback delay).
//
// The 512-entry Gaussian table and the BRR filter coefficients are fixed SNES
// hardware constants (the same values every S-DSP produces), reproduced here as
// data the way the NES period tables in dsp.hpp are. The surrounding code is an
// independent implementation from the documented hardware behaviour, not a port
// of any emulator.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace yanes::snes {

inline constexpr int kNativeRate = 32000;  // the S-DSP's output/sample-native rate
inline constexpr int kBrrBlockBytes = 9;   // one header byte + eight nibble-pair bytes
inline constexpr int kBrrBlockSamples = 16;

// The S-DSP's 4-point Gaussian interpolation table (hardware constant). Each
// fractional read position selects four weights whose sum is ~2048, so the
// >>11 in the interpolator normalises to unity gain.
inline constexpr std::array<int16_t, 512> kGauss = {{
       0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,    1,    1,    1,    1,    1,    1,    1,    1,
       1,    1,    1,    2,    2,    2,    2,    2,    2,    2,    3,    3,
       3,    3,    3,    4,    4,    4,    4,    4,    5,    5,    5,    5,
       6,    6,    6,    6,    7,    7,    7,    8,    8,    8,    9,    9,
       9,   10,   10,   10,   11,   11,   11,   12,   12,   13,   13,   14,
      14,   15,   15,   15,   16,   16,   17,   17,   18,   19,   19,   20,
      20,   21,   21,   22,   23,   23,   24,   24,   25,   26,   27,   27,
      28,   29,   29,   30,   31,   32,   32,   33,   34,   35,   36,   36,
      37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,
      49,   50,   51,   52,   53,   54,   55,   56,   58,   59,   60,   61,
      62,   64,   65,   66,   67,   69,   70,   71,   73,   74,   76,   77,
      78,   80,   81,   83,   84,   86,   87,   89,   90,   92,   94,   95,
      97,   99,  100,  102,  104,  106,  107,  109,  111,  113,  115,  117,
     118,  120,  122,  124,  126,  128,  130,  132,  134,  137,  139,  141,
     143,  145,  147,  150,  152,  154,  156,  159,  161,  163,  166,  168,
     171,  173,  175,  178,  180,  183,  186,  188,  191,  193,  196,  199,
     201,  204,  207,  210,  212,  215,  218,  221,  224,  227,  230,  233,
     236,  239,  242,  245,  248,  251,  254,  257,  260,  263,  267,  270,
     273,  276,  280,  283,  286,  290,  293,  297,  300,  304,  307,  311,
     314,  318,  321,  325,  328,  332,  336,  339,  343,  347,  351,  354,
     358,  362,  366,  370,  374,  378,  381,  385,  389,  393,  397,  401,
     405,  410,  414,  418,  422,  426,  430,  434,  439,  443,  447,  451,
     456,  460,  464,  469,  473,  477,  482,  486,  491,  495,  499,  504,
     508,  513,  517,  522,  527,  531,  536,  540,  545,  550,  554,  559,
     563,  568,  573,  577,  582,  587,  592,  596,  601,  606,  611,  615,
     620,  625,  630,  635,  640,  644,  649,  654,  659,  664,  669,  674,
     678,  683,  688,  693,  698,  703,  708,  713,  718,  723,  728,  732,
     737,  742,  747,  752,  757,  762,  767,  772,  777,  782,  787,  792,
     797,  802,  806,  811,  816,  821,  826,  831,  836,  841,  846,  851,
     855,  860,  865,  870,  875,  880,  884,  889,  894,  899,  904,  908,
     913,  918,  923,  927,  932,  937,  941,  946,  951,  955,  960,  965,
     969,  974,  978,  983,  988,  992,  997, 1001, 1005, 1010, 1014, 1019,
    1023, 1027, 1032, 1036, 1040, 1045, 1049, 1053, 1057, 1061, 1066, 1070,
    1074, 1078, 1082, 1086, 1090, 1094, 1098, 1102, 1106, 1109, 1113, 1117,
    1121, 1125, 1128, 1132, 1136, 1139, 1143, 1146, 1150, 1153, 1157, 1160,
    1164, 1167, 1170, 1174, 1177, 1180, 1183, 1186, 1190, 1193, 1196, 1199,
    1202, 1205, 1207, 1210, 1213, 1216, 1219, 1221, 1224, 1227, 1229, 1232,
    1234, 1237, 1239, 1241, 1244, 1246, 1248, 1251, 1253, 1255, 1257, 1259,
    1261, 1263, 1265, 1267, 1269, 1270, 1272, 1274, 1275, 1277, 1279, 1280,
    1282, 1283, 1284, 1286, 1287, 1288, 1290, 1291, 1292, 1293, 1294, 1295,
    1296, 1297, 1297, 1298, 1299, 1300, 1300, 1301, 1302, 1302, 1303, 1303,
    1303, 1304, 1304, 1304, 1304, 1304, 1305, 1305,
}};

inline int clamp16(int value) { return std::clamp(value, -32768, 32767); }

// One interpolated output between samples s1 and s2 (s0..s3 are four consecutive
// samples), at a fractional position whose upper eight bits index the table.
// This reproduces the S-DSP exactly, including the deliberate 16-bit wrap after
// the third tap and the cleared low output bit.
inline int interpolate(int s0, int s1, int s2, int s3, int frac12) {
  const int offset = (frac12 >> 4) & 0xFF;
  int out = (kGauss[static_cast<size_t>(255 - offset)] * s0) >> 11;
  out += (kGauss[static_cast<size_t>(511 - offset)] * s1) >> 11;
  out += (kGauss[static_cast<size_t>(256 + offset)] * s2) >> 11;
  out = static_cast<int16_t>(out);
  out += (kGauss[static_cast<size_t>(offset)] * s3) >> 11;
  return clamp16(out) & ~1;
}

// A decoded BRR sample stream plus its loop point (in samples), or -1 when the
// sample does not loop.
struct BrrSample {
  std::vector<int16_t> pcm;
  int loop{-1};
  bool empty() const { return pcm.empty(); }
};

// Decode a BRR byte stream (nine-byte blocks) to 16-bit PCM, applying the four
// predictor filters and reading the per-block loop/end flags. When the sample
// loops, its loop point is loop_block (block-aligned); loop_block < 0 means loop
// to the start, which is what a bare block stream implies.
inline BrrSample brr_decode(const uint8_t* brr, size_t bytes, int loop_block = -1) {
  BrrSample out;
  if (!brr || bytes < static_cast<size_t>(kBrrBlockBytes)) return out;
  const size_t blocks = bytes / kBrrBlockBytes;
  out.pcm.reserve(blocks * kBrrBlockSamples);
  int p1 = 0, p2 = 0;  // the two most recent decoded samples (stored doubled)
  bool loops = false;
  for (size_t block = 0; block < blocks; ++block) {
    const uint8_t* b = brr + block * kBrrBlockBytes;
    const int header = b[0];
    const int shift = header >> 4;
    const int filter = (header >> 2) & 3;
    const bool loop = (header & 0x02) != 0;
    const bool end = (header & 0x01) != 0;
    for (int n = 0; n < kBrrBlockSamples; ++n) {
      int nibble = (b[1 + (n >> 1)] >> (n & 1 ? 0 : 4)) & 0x0F;
      int s = static_cast<int16_t>(nibble << 12) >> 12;  // sign-extend 4 bits
      if (shift <= 12) s = (s << shift) >> 1; else s = (s < 0 ? -0x800 : 0);
      const int q1 = p1, q2 = p2 >> 1;
      switch (filter) {
        case 1: s += q1 >> 1; s += (-q1) >> 5; break;
        case 2: s += q1; s -= q2; s += q2 >> 4; s += (q1 * -3) >> 6; break;
        case 3: s += q1; s -= q2; s += (q1 * -13) >> 7; s += (q2 * 3) >> 4; break;
        default: break;
      }
      s = clamp16(s);
      s = static_cast<int16_t>(s * 2);
      p2 = p1;
      p1 = s;
      out.pcm.push_back(static_cast<int16_t>(s >> 1));  // store at unit scale
    }
    // The end block terminates the stream; its loop flag means the sample loops
    // back to its loop point rather than stopping.
    if (end) { loops = loop; break; }
  }
  out.loop = loops ? std::max(0, loop_block) * kBrrBlockSamples : -1;
  return out;
}

// Decode a `.brr` file image: the common convention prefixes a two-byte,
// little-endian loop-offset (in bytes into the block stream); a bare block
// stream has none and loops to the start.
inline BrrSample brr_decode_file(const uint8_t* data, size_t bytes) {
  if (bytes >= 11 && (bytes - 2) % kBrrBlockBytes == 0) {
    const int loop_offset = data[0] | (data[1] << 8);
    return brr_decode(data + 2, bytes - 2, loop_offset / kBrrBlockBytes);
  }
  return brr_decode(data, bytes, -1);
}

// Encode 16-bit PCM to BRR, choosing the filter and shift that minimise each
// block's reconstruction error, and tagging the stream so it loops the whole
// sample or stops at the end.
inline std::vector<uint8_t> brr_encode(const int16_t* pcm, size_t count, bool loop) {
  std::vector<uint8_t> out;
  if (!pcm || count == 0) return out;
  const size_t blocks = (count + kBrrBlockSamples - 1) / kBrrBlockSamples;
  out.reserve(blocks * kBrrBlockBytes);
  int p1 = 0, p2 = 0;  // decoder-state prediction carried between blocks
  for (size_t block = 0; block < blocks; ++block) {
    int16_t samples[kBrrBlockSamples] = {};
    for (int n = 0; n < kBrrBlockSamples; ++n) {
      const size_t index = block * kBrrBlockSamples + static_cast<size_t>(n);
      samples[n] = index < count ? pcm[index] : 0;
    }
    int best_filter = 0, best_shift = 0;
    long long best_error = -1;
    uint8_t best_nibbles[kBrrBlockSamples] = {};
    int best_p1 = p1, best_p2 = p2;
    const int filters = block == 0 ? 1 : 4;  // the first block must use filter 0
    for (int filter = 0; filter < filters; ++filter) {
      for (int shift = 0; shift <= 12; ++shift) {
        long long error = 0;
        int q1 = p1, q2 = p2;
        uint8_t nibbles[kBrrBlockSamples] = {};
        for (int n = 0; n < kBrrBlockSamples; ++n) {
          int predict = 0;
          const int d1 = q1, d2 = q2 >> 1;
          switch (filter) {
            case 1: predict = (d1 >> 1) + ((-d1) >> 5); break;
            case 2: predict = d1 - d2 + (d2 >> 4) + ((d1 * -3) >> 6); break;
            case 3: predict = d1 - d2 + ((d1 * -13) >> 7) + ((d2 * 3) >> 4); break;
            default: break;
          }
          const int target = samples[n];
          // Quantise (target - predict) into a 4-bit code at this shift.
          int delta = ((target - predict) << 1) >> shift;  // undo the decoder's <<shift>>1
          int code = std::clamp((delta + (delta < 0 ? -1 : 1)) / 2, -8, 7);
          int s = static_cast<int16_t>((code & 0x0F) << 12) >> 12;
          s = (s << shift) >> 1;
          int reconstructed = clamp16(s + predict);
          reconstructed = static_cast<int16_t>(reconstructed * 2) >> 1;
          const long long e = static_cast<long long>(reconstructed) - target;
          error += e * e;
          nibbles[n] = static_cast<uint8_t>(code & 0x0F);
          q2 = q1;
          q1 = reconstructed * 2;
        }
        if (best_error < 0 || error < best_error) {
          best_error = error;
          best_filter = filter;
          best_shift = shift;
          std::copy(std::begin(nibbles), std::end(nibbles), std::begin(best_nibbles));
          best_p1 = q1;
          best_p2 = q2;
        }
      }
    }
    const bool last = block + 1 == blocks;
    uint8_t header = static_cast<uint8_t>((best_shift << 4) | (best_filter << 2));
    if (last) header |= 0x01;                 // end flag
    if (last && loop) header |= 0x02;         // loop flag: whole-sample loop
    out.push_back(header);
    for (int n = 0; n < kBrrBlockSamples; n += 2)
      out.push_back(static_cast<uint8_t>((best_nibbles[n] << 4) | best_nibbles[n + 1]));
    p1 = best_p1;
    p2 = best_p2;
  }
  return out;
}

// The S-DSP echo: an eight-tap FIR whose output feeds a delay line that is mixed
// back in. Coefficients and feedback are the register values (signed bytes / 128).
struct Echo {
  std::vector<float> buffer;
  size_t position{0};
  std::array<float, 8> fir{{0.5f, 0, 0, 0, 0, 0, 0, 0}};
  float feedback{0.0f};
  float wet{0.0f};

  void configure(int delay_samples, const std::array<int8_t, 8>& coefficients, float feedback_norm, float wet_norm) {
    buffer.assign(static_cast<size_t>(std::max(8, delay_samples)), 0.0f);
    for (size_t i = 0; i < fir.size(); ++i) fir[i] = coefficients[i] / 128.0f;
    feedback = feedback_norm;
    wet = wet_norm;
    position = 0;
  }
  // Process one input sample, returning the echo send to add to the dry signal.
  float process(float input) {
    if (buffer.size() < 8) return 0.0f;
    float filtered = 0.0f;
    for (size_t tap = 0; tap < fir.size(); ++tap)
      filtered += fir[tap] * buffer[(position + tap) % buffer.size()];
    buffer[position] = input + filtered * feedback;
    position = (position + 1) % buffer.size();
    return filtered * wet;
  }
};

}  // namespace yanes::snes
