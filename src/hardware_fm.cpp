#include "hardware_fm.hpp"
#include <ymfm_opn.h>
#include <ymfm_opl.h>
#include <ymfm_opm.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace yanes {

// Carriers per algorithm, as a mask over the operator index used by write_operators. That
// index addresses the chip's register slots, which on the OPN family run 1, 3, 2, 4 rather
// than in operator order, so algorithm 4 (two parallel pairs, carriers OP2 and OP4) is
// indices 2 and 3. It read 0x0a here, which named OP3 — a modulator — as the carrier and
// left the real second carrier at modulator attenuation, so algorithm 4 lost a voice and
// over-modulated the one it kept. YM2151 lays its four slots out as M1, M2, C1, C2, where
// the same algorithm's carriers are again indices 2 and 3, so the mask suits both families.
constexpr uint8_t kFourOpCarriers[8] = {0x08, 0x08, 0x08, 0x08, 0x0c, 0x0e, 0x0e, 0x0f};
constexpr uint8_t operator_level(int algorithm, int op, int carrier_level) {
  const bool carrier = ((kFourOpCarriers[algorithm & 7] >> op) & 1U) != 0;
  return static_cast<uint8_t>(carrier ? carrier_level : 20 + op * 7);
}
constexpr uint8_t opl_level(int algorithm, int op, int carrier_level) {
  return static_cast<uint8_t>(op == 1 || (algorithm & 1) ? carrier_level : 18);
}
// Per-chip value that one voice's loudest output maps to, so every chip arrives at the
// mixer on the same scale. The YM2612 entry read 6600, which its own DAC cannot reach: the
// channel clamps at 256, the ladder-effect offset lifts that to 260, the five idle channels
// each add 4, and ymfm's final (x * 128 * 64) / (6 * 65) turns the resulting 280 into 5881.
// Normalising by 6600 therefore capped the chip at 0.89 of full scale where every sibling
// reaches 1.0 and beyond, leaving Genesis FM permanently short of the rest of the plug-in.
constexpr double kChipFullScale[] = {5881.0, 11000.0, 5530.0, 2670.0, 5320.0, 10850.0, 4800.0};

// Brightness only ever reaches these chips as carrier total level, which is pure output
// attenuation: the modulator levels below are fixed, so nothing about the tone changes with
// it. A linear (1 - brightness) * 32 therefore parked the default 0.65 at eleven steps of
// attenuation, holding every hardware FM voice 8 dB under the plug-in's other chips for no
// audible return. Cubing puts the default within one step of fully open while keeping the
// full 24 dB of range underneath it, so turning brightness down still attenuates as before.
constexpr int carrier_attenuation(double brightness) {
  const double closed = 1.0 - std::clamp(brightness, 0.0, 1.0);
  return static_cast<int>(closed * closed * closed * 32.0 + 0.5);
}
constexpr uint8_t opl_operator_flags(const FmControls& c, int op) {
  return static_cast<uint8_t>((c.am_depth ? 0x80 : 0) | (c.pm_depth ? 0x40 : 0) | 0x20 |
                              (c.key_scale ? 0x10 : 0) | (op + 1));
}
constexpr uint8_t opl_slots_two[] = {0, 3};
constexpr uint8_t opl_slots_four[] = {0, 3, 8, 11};

struct HardwareFmVoice::Impl : ymfm::ymfm_interface {
  static constexpr uint32_t opn2_clock = 7670454, opn_clock = 4000000, opna_clock = 8000000;
  static constexpr uint32_t opl_clock = 3579545, opl3_clock = 14318180, opm_clock = 3579545;
  std::unique_ptr<ymfm::ym2612> opn;
  std::unique_ptr<ymfm::ym2203> opn1;
  std::unique_ptr<ymfm::ym2608> opna;
  std::unique_ptr<ymfm::ym3812> opl;
  std::unique_ptr<ymfm::ymf262> opl3;
  std::unique_ptr<ymfm::ym2151> opm;
  HardwareFmVoice::Kind kind{HardwareFmVoice::Kind::Ym2612};
  double accumulator{};
  float last{};
  int last_pitch_a{-1}, last_pitch_b{-1};
  double last_frequency{-1};
  bool programmed{};
  bool keyed{};

  template<class Chip> static void write(Chip& chip, uint16_t reg, uint8_t value) {
    chip.write((reg >> 8U) * 2U, static_cast<uint8_t>(reg));
    chip.write((reg >> 8U) * 2U + 1U, value);
  }
  template<class Chip>
  static void write_operators(Chip& chip, const uint8_t* slots, int count, const FmControls& c,
                              int alg, int carrier_level, bool four_op) {
    for (int op = 0; op < count; ++op) {
      const uint8_t s = slots[op];
      write(chip, static_cast<uint16_t>(0x20 + s), opl_operator_flags(c, op));
      const uint8_t level = four_op
          ? static_cast<uint8_t>(op == count - 1 || ((alg & 1) && op >= 2) || ((alg & 2) && op == 0)
                                     ? carrier_level : 18 + op * 6)
          : opl_level(alg, op, carrier_level);
      write(chip, static_cast<uint16_t>(0x40 + s), level);
      write(chip, static_cast<uint16_t>(0x60 + s),
            static_cast<uint8_t>((std::clamp(c.attack, 0, 15) << 4) | std::clamp(c.decay, 0, 15)));
      write(chip, static_cast<uint16_t>(0x80 + s),
            static_cast<uint8_t>((std::clamp(c.sustain_level, 0, 15) << 4) | std::clamp(c.release, 0, 15)));
      write(chip, static_cast<uint16_t>(0xe0 + s), static_cast<uint8_t>(op & 3));
    }
  }
  static void fnum(double frequency, int& block, int& fn) {
    double value = 0;
    block = 0;
    for (; block <= 7; ++block) {
      value = frequency * 72.0 * 1048576.0 / (opl_clock * std::pow(2.0, block));
      if (value < 1024.0 || block == 7) break;
    }
    fn = std::clamp(static_cast<int>(std::round(value)), 0, 1023);
  }

  void ensure(HardwareFmVoice::Kind next) {
    if (next == HardwareFmVoice::Kind::Ym2612 && !opn) opn = std::make_unique<ymfm::ym2612>(*this);
    else if (next == HardwareFmVoice::Kind::Opn && !opn1) {
      opn1 = std::make_unique<ymfm::ym2203>(*this);
      opn1->set_fidelity(ymfm::OPN_FIDELITY_MIN);
    } else if (next == HardwareFmVoice::Kind::Opna && !opna) {
      opna = std::make_unique<ymfm::ym2608>(*this);
      opna->set_fidelity(ymfm::OPN_FIDELITY_MIN);
    } else if (next == HardwareFmVoice::Kind::Opl2 && !opl) opl = std::make_unique<ymfm::ym3812>(*this);
    else if ((next == HardwareFmVoice::Kind::Opl3 || next == HardwareFmVoice::Kind::Opl3FourOp) && !opl3)
      opl3 = std::make_unique<ymfm::ymf262>(*this);
    else if (next == HardwareFmVoice::Kind::Opm && !opm) opm = std::make_unique<ymfm::ym2151>(*this);
  }

  void reset_chips() {
    if (opn) opn->reset();
    if (opn1) opn1->reset();
    if (opna) opna->reset();
    if (opl) opl->reset();
    if (opl3) opl3->reset();
    if (opm) opm->reset();
    accumulator = 0;
    last = 0;
    last_pitch_a = last_pitch_b = -1;
    last_frequency = -1;
    programmed = keyed = false;
  }

  void flush(HardwareFmVoice::Kind k) {
    for (int i = 0; i < 32; ++i) {
      if (k == HardwareFmVoice::Kind::Ym2612) { ymfm::ym2612::output_data out{}; opn->generate(&out); }
      else if (k == HardwareFmVoice::Kind::Opn) { ymfm::ym2203::output_data out{}; opn1->generate(&out); }
      else if (k == HardwareFmVoice::Kind::Opna) { ymfm::ym2608::output_data out{}; opna->generate(&out); }
      else if (k == HardwareFmVoice::Kind::Opl2) { ymfm::ym3812::output_data out{}; opl->generate(&out); }
      else if (k == HardwareFmVoice::Kind::Opl3 || k == HardwareFmVoice::Kind::Opl3FourOp) {
        ymfm::ymf262::output_data out{}; opl3->generate(&out);
      } else { ymfm::ym2151::output_data out{}; opm->generate(&out); }
    }
  }
};

HardwareFmVoice::HardwareFmVoice() : impl_(std::make_unique<Impl>()) {}
HardwareFmVoice::~HardwareFmVoice() = default;
HardwareFmVoice::HardwareFmVoice(HardwareFmVoice&&) noexcept = default;
HardwareFmVoice& HardwareFmVoice::operator=(HardwareFmVoice&&) noexcept = default;
void HardwareFmVoice::prepare() {
  for (const auto kind : {Kind::Ym2612, Kind::Opn, Kind::Opna, Kind::Opl2, Kind::Opl3, Kind::Opm})
    impl_->ensure(kind);
}
void HardwareFmVoice::reset() { impl_->reset_chips(); }

void HardwareFmVoice::key_on(Kind kind, double frequency, const FmControls& c) {
  impl_->ensure(kind);
  reset();
  impl_->ensure(kind);
  impl_->kind = kind;
  impl_->programmed = impl_->keyed = true;
  impl_->flush(kind);
  const int alg = std::clamp(c.algorithm, 0, 7), fb = std::clamp(c.feedback, 0, 7);
  const int carrier_level = carrier_attenuation(c.brightness);
  if (kind == Kind::Opl2 || kind == Kind::Opl3 || kind == Kind::Opl3FourOp) {
    const bool four = kind == Kind::Opl3FourOp;
    auto setup = [&](auto& chip) {
      Impl::write(chip, 0x01, 0x20); // Enable the OPL2 waveform-select registers.
      Impl::write(chip, 0xbd, static_cast<uint8_t>((c.am_depth >= 64 ? 0x80 : 0) | (c.pm_depth >= 64 ? 0x40 : 0)));
      Impl::write_operators(chip, four ? opl_slots_four : opl_slots_two, four ? 4 : 2, c, alg,
                          carrier_level, four);
      if (four) {
        Impl::write(chip, 0x105, 1);
        Impl::write(chip, 0x104, 1);
        Impl::write(chip, 0xc0, static_cast<uint8_t>(0x30 | (fb << 1) | ((alg >> 1) & 1)));
        Impl::write(chip, 0xc3, static_cast<uint8_t>(0x30 | (alg & 1)));
      } else {
        if (kind == Kind::Opl3) Impl::write(chip, 0x105, 1);
        Impl::write(chip, 0xc0, static_cast<uint8_t>(0x30 | (fb << 1) | (alg & 1)));
      }
      int block = 0, fn = 0;
      Impl::fnum(frequency, block, fn);
      const uint8_t hi = static_cast<uint8_t>(0x20 | (block << 2) | (fn >> 8));
      Impl::write(chip, 0xa0, static_cast<uint8_t>(fn));
      Impl::write(chip, 0xb0, hi);
      if (four) {
        Impl::write(chip, 0xa3, static_cast<uint8_t>(fn));
        Impl::write(chip, 0xb3, hi);
      }
    };
    if (kind == Kind::Opl2) setup(*impl_->opl);
    else setup(*impl_->opl3);
    return;
  }
  if (kind == Kind::Opm) {
    constexpr uint8_t slots[] = {0x00, 0x08, 0x10, 0x18};
    for (int op = 0; op < 4; ++op) {
      const uint8_t s = slots[op];
      Impl::write(*impl_->opm, static_cast<uint16_t>(0x40 + s), static_cast<uint8_t>((std::clamp(c.detune,0,7) << 4) | (1 + op)));
      Impl::write(*impl_->opm, static_cast<uint16_t>(0x60 + s), operator_level(alg, op, carrier_level));
      Impl::write(*impl_->opm, static_cast<uint16_t>(0x80 + s), static_cast<uint8_t>((std::clamp(c.key_scale,0,3) << 6) | std::clamp(c.attack,0,31)));
      Impl::write(*impl_->opm, static_cast<uint16_t>(0xa0 + s), static_cast<uint8_t>((c.am_depth ? 0x80 : 0) | std::clamp(c.decay,0,31)));
      Impl::write(*impl_->opm, static_cast<uint16_t>(0xc0 + s), static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));
      Impl::write(*impl_->opm, static_cast<uint16_t>(0xe0 + s), static_cast<uint8_t>((std::clamp(c.sustain_level,0,15) << 4) | std::clamp(c.release,0,15)));
    }
    Impl::write(*impl_->opm, 0x18, static_cast<uint8_t>(std::clamp(c.lfo_rate,0,7) * 32));
    Impl::write(*impl_->opm, 0x19, static_cast<uint8_t>(std::clamp(c.am_depth,0,127)));
    Impl::write(*impl_->opm, 0x19, static_cast<uint8_t>(0x80 | std::clamp(c.pm_depth,0,127)));
    Impl::write(*impl_->opm, 0x20, static_cast<uint8_t>(0xc0 | (fb << 3) | alg));
    // OPM key-code zero denotes C-sharp; MIDI pitch classes start at C.
    const int midi = std::clamp(static_cast<int>(std::round(68.0 + 12.0 * std::log2(frequency / 440.0))), 0, 127);
    constexpr uint8_t notes[] = {0,1,2,4,5,6,8,9,10,12,13,14};
    Impl::write(*impl_->opm, 0x28, static_cast<uint8_t>((std::clamp(midi / 12 - 1, 0, 7) << 4) | notes[midi % 12]));
    Impl::write(*impl_->opm, 0x30, 0); Impl::write(*impl_->opm, 0x38, static_cast<uint8_t>((std::clamp(c.pm_depth / 16, 0, 7) << 4) | std::clamp(c.am_depth / 32, 0, 3))); Impl::write(*impl_->opm, 0x08, 0x78);
    return;
  }
  constexpr uint8_t slots[] = {0x00, 0x04, 0x08, 0x0c};
  auto opn_write = [&](uint16_t reg, uint8_t data) {
    if (kind == Kind::Opn) Impl::write(*impl_->opn1, reg, data);
    else if (kind == Kind::Opna) Impl::write(*impl_->opna, reg, data);
    else Impl::write(*impl_->opn, reg, data);
  };
  for (int op = 0; op < 4; ++op) {
    const uint8_t s = slots[op];
    opn_write(static_cast<uint16_t>(0x30 + s), static_cast<uint8_t>((std::clamp(c.detune,0,7) << 4) | (1 + op)));
    opn_write(static_cast<uint16_t>(0x40 + s), operator_level(alg, op, carrier_level));
    opn_write(static_cast<uint16_t>(0x50 + s), static_cast<uint8_t>((std::clamp(c.key_scale,0,3) << 6) | std::clamp(c.attack,0,31)));
    opn_write(static_cast<uint16_t>(0x60 + s), static_cast<uint8_t>((c.am_depth ? 0x80 : 0) | std::clamp(c.decay,0,31)));
    opn_write(static_cast<uint16_t>(0x70 + s), static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));
    opn_write(static_cast<uint16_t>(0x80 + s), static_cast<uint8_t>((std::clamp(c.sustain_level,0,15) << 4) | std::clamp(c.release,0,15)));
  }
  if (kind != Kind::Opn) opn_write(0x22, static_cast<uint8_t>((c.am_depth || c.pm_depth ? 8 : 0) | std::clamp(c.lfo_rate,0,7)));
  opn_write(0xb0, static_cast<uint8_t>((fb << 3) | alg));
  if (kind != Kind::Opn) opn_write(0xb4, static_cast<uint8_t>(0xc0 | (std::min(3,c.am_depth/32) << 4) | std::min(7,c.pm_depth/16)));
  // OPN uses a 21-bit phase denominator; YM2203 clocks FM every 72 clocks,
  // while OPNA/YM2612 use 144. Mixing those factors transposed the latter down an octave.
  int block = 0;
  double fnum = 0;
  for (; block <= 7; ++block) {
    const double active_clock = kind == Kind::Opn ? Impl::opn_clock : (kind == Kind::Opna ? Impl::opna_clock : Impl::opn2_clock);
    fnum = frequency * (kind == Kind::Opn ? 72.0 : 144.0) * 2097152.0 / (active_clock * std::pow(2.0, block));
    if (fnum < 2048.0 || block == 7) break;
  }
  const int fn = std::clamp(static_cast<int>(std::round(fnum)), 0, 2047);
  opn_write(0xa4, static_cast<uint8_t>((block << 3) | (fn >> 8)));
  opn_write(0xa0, static_cast<uint8_t>(fn));
  opn_write(0x28, 0xf0);
}
void HardwareFmVoice::key_off() {
  if (!impl_->programmed || !impl_->keyed) return;
  impl_->keyed = false;
  impl_->last_frequency = -1;
  const auto opl_pitch = static_cast<uint8_t>(std::max(0, impl_->last_pitch_b) & ~0x20);
  if (impl_->kind == Kind::Ym2612 && impl_->opn) Impl::write(*impl_->opn, 0x28, 0);
  else if (impl_->kind == Kind::Opn && impl_->opn1) Impl::write(*impl_->opn1, 0x28, 0);
  else if (impl_->kind == Kind::Opna && impl_->opna) Impl::write(*impl_->opna, 0x28, 0);
  else if (impl_->kind == Kind::Opl2 && impl_->opl) Impl::write(*impl_->opl, 0xb0, opl_pitch);
  else if ((impl_->kind == Kind::Opl3 || impl_->kind == Kind::Opl3FourOp) && impl_->opl3) {
    Impl::write(*impl_->opl3, 0xb0, opl_pitch);
    if (impl_->kind == Kind::Opl3FourOp) Impl::write(*impl_->opl3, 0xb3, opl_pitch);
  } else if (impl_->opm) Impl::write(*impl_->opm, 0x08, 0);
}
void HardwareFmVoice::update_controls(const FmControls& c) {
  if (!impl_->programmed) return;
  const int alg=std::clamp(c.algorithm,0,7),fb=std::clamp(c.feedback,0,7);
  const int carrier=carrier_attenuation(c.brightness);
  if (impl_->kind==Kind::Opl2||impl_->kind==Kind::Opl3||impl_->kind==Kind::Opl3FourOp) {
    const bool four = impl_->kind == Kind::Opl3FourOp;
    auto apply=[&](auto& chip){
      Impl::write(chip, 0x01, 0x20); // Enable the OPL2 waveform-select registers.
      Impl::write(chip, 0xbd, static_cast<uint8_t>((c.am_depth >= 64 ? 0x80 : 0) | (c.pm_depth >= 64 ? 0x40 : 0)));
      Impl::write_operators(chip, four ? opl_slots_four : opl_slots_two, four ? 4 : 2, c, alg, carrier, four);
      if (four) {
        Impl::write(chip,0xc0,static_cast<uint8_t>(0x30|(fb<<1)|((alg>>1)&1)));
        Impl::write(chip,0xc3,static_cast<uint8_t>(0x30|(alg&1)));
      } else Impl::write(chip,0xc0,static_cast<uint8_t>(0x30|(fb<<1)|(alg&1)));
    };
    if(impl_->kind==Kind::Opl2)apply(*impl_->opl);else apply(*impl_->opl3);return;
  }
  if (impl_->kind==Kind::Opm) {constexpr uint8_t slots[]={0,8,16,24};for(int op=0;op<4;++op){const uint8_t s=slots[op];
    Impl::write(*impl_->opm,static_cast<uint16_t>(0x40+s),static_cast<uint8_t>((std::clamp(c.detune,0,7)<<4)|(op+1)));
    Impl::write(*impl_->opm,static_cast<uint16_t>(0x60+s),operator_level(alg,op,carrier));
    Impl::write(*impl_->opm,static_cast<uint16_t>(0x80+s),static_cast<uint8_t>((std::clamp(c.key_scale,0,3)<<6)|std::clamp(c.attack,0,31)));
    Impl::write(*impl_->opm,static_cast<uint16_t>(0xa0+s),static_cast<uint8_t>((c.am_depth?0x80:0)|std::clamp(c.decay,0,31)));
    Impl::write(*impl_->opm,static_cast<uint16_t>(0xc0+s),static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));
    Impl::write(*impl_->opm,static_cast<uint16_t>(0xe0+s),static_cast<uint8_t>((std::clamp(c.sustain_level,0,15)<<4)|std::clamp(c.release,0,15)));}
    Impl::write(*impl_->opm,0x38,static_cast<uint8_t>((std::clamp(c.pm_depth / 16, 0, 7) << 4) | std::clamp(c.am_depth / 32, 0, 3)));
    Impl::write(*impl_->opm,0x18,static_cast<uint8_t>(std::clamp(c.lfo_rate,0,7)*32));Impl::write(*impl_->opm,0x19,static_cast<uint8_t>(std::clamp(c.am_depth,0,127)));Impl::write(*impl_->opm,0x19,static_cast<uint8_t>(0x80|std::clamp(c.pm_depth,0,127)));Impl::write(*impl_->opm,0x20,static_cast<uint8_t>(0xc0|(fb<<3)|alg));return;
  }
  auto write=[&](uint16_t r,uint8_t d){if(impl_->kind==Kind::Opn)Impl::write(*impl_->opn1,r,d);else if(impl_->kind==Kind::Opna)Impl::write(*impl_->opna,r,d);else Impl::write(*impl_->opn,r,d);};
  constexpr uint8_t slots[]={0,4,8,12};for(int op=0;op<4;++op){const uint8_t s=slots[op];write(static_cast<uint16_t>(0x30+s),static_cast<uint8_t>((std::clamp(c.detune,0,7)<<4)|(op+1)));write(static_cast<uint16_t>(0x40+s),operator_level(alg,op,carrier));write(static_cast<uint16_t>(0x50+s),static_cast<uint8_t>((std::clamp(c.key_scale,0,3)<<6)|std::clamp(c.attack,0,31)));write(static_cast<uint16_t>(0x60+s),static_cast<uint8_t>((c.am_depth?0x80:0)|std::clamp(c.decay,0,31)));write(static_cast<uint16_t>(0x70+s),static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));write(static_cast<uint16_t>(0x80+s),static_cast<uint8_t>((std::clamp(c.sustain_level,0,15)<<4)|std::clamp(c.release,0,15)));}
  write(0xb0,static_cast<uint8_t>((fb<<3)|alg));if(impl_->kind!=Kind::Opn){write(0x22,static_cast<uint8_t>((c.am_depth||c.pm_depth?8:0)|std::clamp(c.lfo_rate,0,7)));write(0xb4,static_cast<uint8_t>(0xc0|(std::min(3,c.am_depth/32)<<4)|std::min(7,c.pm_depth/16)));}
}
float HardwareFmVoice::render(double host_rate, double frequency) {
  if (!impl_->programmed) return 0.0f;
  // Steady notes need no new pitch registers or repeated logarithms/divider searches.
  if (frequency != impl_->last_frequency) {
  if (impl_->kind == Kind::Opm) {
    const double midi_f = std::clamp(68.0 + 12.0 * std::log2(std::max(1.0, frequency) / 440.0), 0.0, 127.999);
    const int midi = static_cast<int>(std::floor(midi_f));
    constexpr uint8_t notes[] = {0,1,2,4,5,6,8,9,10,12,13,14};
    const int kc = (std::clamp(midi / 12 - 1, 0, 7) << 4) | notes[midi % 12];
    const int kf = std::clamp(static_cast<int>((midi_f - midi) * 64.0), 0, 63) << 2;
    if (kc != impl_->last_pitch_a) { Impl::write(*impl_->opm, 0x28, static_cast<uint8_t>(kc)); impl_->last_pitch_a = kc; }
    if (kf != impl_->last_pitch_b) { Impl::write(*impl_->opm, 0x30, static_cast<uint8_t>(kf)); impl_->last_pitch_b = kf; }
  } else if (impl_->kind == Kind::Opl2 || impl_->kind == Kind::Opl3 || impl_->kind == Kind::Opl3FourOp) {
    int block = 0, fn = 0;
    Impl::fnum(frequency, block, fn);
    const int hi = (impl_->keyed ? 0x20 : 0) | (block << 2) | (fn >> 8);
    if (fn != impl_->last_pitch_a || hi != impl_->last_pitch_b) {
      auto write_pitch = [&](auto& chip) {
        Impl::write(chip, 0xa0, static_cast<uint8_t>(fn));
        Impl::write(chip, 0xb0, static_cast<uint8_t>(hi));
        if (impl_->kind == Kind::Opl3FourOp) {
          Impl::write(chip, 0xa3, static_cast<uint8_t>(fn));
          Impl::write(chip, 0xb3, static_cast<uint8_t>(hi));
        }
      };
      if (impl_->kind == Kind::Opl2) write_pitch(*impl_->opl);
      else write_pitch(*impl_->opl3);
      impl_->last_pitch_a = fn; impl_->last_pitch_b = hi;
    }
  } else {
    const double clock = impl_->kind == Kind::Opn ? Impl::opn_clock : (impl_->kind == Kind::Opna ? Impl::opna_clock : Impl::opn2_clock);
    int block = 0; double fnum = 0;
    for (; block <= 7; ++block) { fnum = frequency * (impl_->kind == Kind::Opn ? 72.0 : 144.0) * 2097152.0 / (clock * std::pow(2.0, block)); if (fnum < 2048.0 || block == 7) break; }
    const int fn = std::clamp(static_cast<int>(std::round(fnum)), 0, 2047), hi = (block << 3) | (fn >> 8);
    if (fn != impl_->last_pitch_a || hi != impl_->last_pitch_b) {
      if (impl_->kind == Kind::Opn) { Impl::write(*impl_->opn1, 0xa4, static_cast<uint8_t>(hi)); Impl::write(*impl_->opn1, 0xa0, static_cast<uint8_t>(fn)); }
      else if (impl_->kind == Kind::Opna) { Impl::write(*impl_->opna, 0xa4, static_cast<uint8_t>(hi)); Impl::write(*impl_->opna, 0xa0, static_cast<uint8_t>(fn)); }
      else { Impl::write(*impl_->opn, 0xa4, static_cast<uint8_t>(hi)); Impl::write(*impl_->opn, 0xa0, static_cast<uint8_t>(fn)); }
      impl_->last_pitch_a = fn; impl_->last_pitch_b = hi;
    }
  }
    impl_->last_frequency = frequency;
  }
  double native_rate = 0;
  if (impl_->kind == Kind::Ym2612) native_rate = impl_->opn->sample_rate(Impl::opn2_clock);
  else if (impl_->kind == Kind::Opn) native_rate = impl_->opn1->sample_rate(Impl::opn_clock);
  else if (impl_->kind == Kind::Opna) native_rate = impl_->opna->sample_rate(Impl::opna_clock);
  else if (impl_->kind == Kind::Opl2) native_rate = impl_->opl->sample_rate(Impl::opl_clock);
  else if (impl_->kind == Kind::Opl3 || impl_->kind == Kind::Opl3FourOp)
    native_rate = impl_->opl3->sample_rate(Impl::opl3_clock);
  else native_rate = impl_->opm->sample_rate(Impl::opm_clock);
  const double full_scale = kChipFullScale[static_cast<int>(impl_->kind)];
  impl_->accumulator += native_rate / host_rate;
  while (impl_->accumulator >= 1.0) {
    if (impl_->kind == Kind::Ym2612) { ymfm::ym2612::output_data out{}; impl_->opn->generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / (2.0 * full_scale)); }
    else if (impl_->kind == Kind::Opn) { ymfm::ym2203::output_data out{}; impl_->opn1->generate(&out); impl_->last = static_cast<float>(out.data[0] / full_scale); }
    else if (impl_->kind == Kind::Opna) { ymfm::ym2608::output_data out{}; impl_->opna->generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / (2.0 * full_scale)); }
    else if (impl_->kind == Kind::Opl2) { ymfm::ym3812::output_data out{}; impl_->opl->generate(&out); impl_->last = static_cast<float>(out.data[0] / full_scale); }
    else if (impl_->kind == Kind::Opl3 || impl_->kind == Kind::Opl3FourOp) {
      ymfm::ymf262::output_data out{}; impl_->opl3->generate(&out);
      impl_->last = static_cast<float>((out.data[0] + out.data[1]) / (2.0 * full_scale));
    } else { ymfm::ym2151::output_data out{}; impl_->opm->generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / (2.0 * full_scale)); }
    impl_->accumulator -= 1.0;
  }
  return impl_->last;
}
}
