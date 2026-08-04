#include "hardware_fm.hpp"
#include <ymfm_opn.h>
#include <ymfm_opl.h>
#include <ymfm_opm.h>
#include <algorithm>
#include <cmath>

namespace yanes {
struct HardwareFmVoice::Impl : ymfm::ymfm_interface {
  static constexpr uint32_t opn2_clock = 7670454, opn_clock = 4000000, opna_clock = 8000000;
  static constexpr uint32_t opl_clock = 3579545, opl3_clock = 14318180, opm_clock = 3579545;
  ymfm::ym2612 opn{*this};
  ymfm::ym2203 opn1{*this};
  ymfm::ym2608 opna{*this};
  ymfm::ym3812 opl{*this};
  ymfm::ymf262 opl3{*this};
  ymfm::ym2151 opm{*this};
  HardwareFmVoice::Kind kind{HardwareFmVoice::Kind::Ym2612};
  double accumulator{};
  float last{};
  int last_pitch_a{-1}, last_pitch_b{-1};
  bool programmed{};
  Impl() { opn1.set_fidelity(ymfm::OPN_FIDELITY_MIN); opna.set_fidelity(ymfm::OPN_FIDELITY_MIN); opn.reset(); opn1.reset(); opna.reset(); opl.reset(); opl3.reset(); opm.reset(); }
  template<class Chip> static void write(Chip& chip, uint16_t reg, uint8_t value) { chip.write((reg >> 8U) * 2U, static_cast<uint8_t>(reg)); chip.write((reg >> 8U) * 2U + 1U, value); }
};

HardwareFmVoice::HardwareFmVoice() : impl_(std::make_unique<Impl>()) {}
HardwareFmVoice::~HardwareFmVoice() = default;
HardwareFmVoice::HardwareFmVoice(HardwareFmVoice&&) noexcept = default;
HardwareFmVoice& HardwareFmVoice::operator=(HardwareFmVoice&&) noexcept = default;
void HardwareFmVoice::reset() { impl_->opn.reset(); impl_->opn1.reset(); impl_->opna.reset(); impl_->opl.reset(); impl_->opl3.reset(); impl_->opm.reset(); impl_->accumulator = 0; impl_->last = 0; impl_->last_pitch_a = impl_->last_pitch_b = -1; impl_->programmed = false; }

void HardwareFmVoice::key_on(Kind kind, double frequency, const FmControls& c) {
  reset();
  impl_->kind = kind;
  impl_->programmed = true;
  // Flush the chip's internal operator pipeline after reset before programming it.
  for (int i = 0; i < 32; ++i) {
    if (kind == Kind::Ym2612) { ymfm::ym2612::output_data out{}; impl_->opn.generate(&out); }
    else if (kind == Kind::Opn) { ymfm::ym2203::output_data out{}; impl_->opn1.generate(&out); }
    else if (kind == Kind::Opna) { ymfm::ym2608::output_data out{}; impl_->opna.generate(&out); }
    else if (kind == Kind::Opl2) { ymfm::ym3812::output_data out{}; impl_->opl.generate(&out); }
    else if (kind == Kind::Opl3) { ymfm::ymf262::output_data out{}; impl_->opl3.generate(&out); }
    else { ymfm::ym2151::output_data out{}; impl_->opm.generate(&out); }
  }
  const int alg = std::clamp(c.algorithm, 0, 7), fb = std::clamp(c.feedback, 0, 7);
  const int carrier_level = std::clamp(static_cast<int>((1.0 - c.brightness) * 32.0), 0, 48);
  if (kind == Kind::Opl2 || kind == Kind::Opl3) {
    auto setup = [&](auto& chip) {
      constexpr uint8_t slots[] = {0, 3};
      for (int op = 0; op < 2; ++op) {
        const uint8_t s = slots[op];
        Impl::write(chip, static_cast<uint16_t>(0x20 + s), static_cast<uint8_t>((c.key_scale ? 0x10 : 0) | 0x40 | (op + 1)));
        Impl::write(chip, static_cast<uint16_t>(0x40 + s), static_cast<uint8_t>(op ? carrier_level : 18));
        Impl::write(chip, static_cast<uint16_t>(0x60 + s), static_cast<uint8_t>((std::clamp(c.attack,0,15) << 4) | std::clamp(c.decay,0,15)));
        Impl::write(chip, static_cast<uint16_t>(0x80 + s), static_cast<uint8_t>((std::clamp(c.sustain_level,0,15) << 4) | std::clamp(c.release,0,15)));
        Impl::write(chip, static_cast<uint16_t>(0xe0 + s), static_cast<uint8_t>(op & 3));
      }
      Impl::write(chip, 0xc0, static_cast<uint8_t>(0x30 | (fb << 1) | (alg & 1)));
      int block = 0; double fnum = 0;
      for (; block < 7; ++block) { fnum = frequency * 72.0 * 1048576.0 / (Impl::opl_clock * std::pow(2.0, block)); if (fnum < 1024.0) break; }
      const int fn = std::clamp(static_cast<int>(std::round(fnum)), 0, 1023);
      Impl::write(chip, 0xa0, static_cast<uint8_t>(fn));
      Impl::write(chip, 0xb0, static_cast<uint8_t>(0x20 | (block << 2) | (fn >> 8)));
    };
    if (kind == Kind::Opl2) setup(impl_->opl); else { Impl::write(impl_->opl3, 0x105, 1); setup(impl_->opl3); }
    return;
  }
  if (kind == Kind::Opm) {
    constexpr uint8_t slots[] = {0x00, 0x08, 0x10, 0x18};
    for (int op = 0; op < 4; ++op) {
      const uint8_t s = slots[op];
      Impl::write(impl_->opm, static_cast<uint16_t>(0x40 + s), static_cast<uint8_t>((std::clamp(c.detune,0,7) << 4) | (1 + op)));
      Impl::write(impl_->opm, static_cast<uint16_t>(0x60 + s), static_cast<uint8_t>(op == 3 ? carrier_level : 20 + op * 7));
      Impl::write(impl_->opm, static_cast<uint16_t>(0x80 + s), static_cast<uint8_t>((std::clamp(c.key_scale,0,3) << 6) | std::clamp(c.attack,0,31)));
      Impl::write(impl_->opm, static_cast<uint16_t>(0xa0 + s), static_cast<uint8_t>((c.am_depth ? 0x80 : 0) | std::clamp(c.decay,0,31)));
      Impl::write(impl_->opm, static_cast<uint16_t>(0xc0 + s), static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));
      Impl::write(impl_->opm, static_cast<uint16_t>(0xe0 + s), static_cast<uint8_t>((std::clamp(c.sustain_level,0,15) << 4) | std::clamp(c.release,0,15)));
    }
    Impl::write(impl_->opm, 0x18, static_cast<uint8_t>(std::clamp(c.lfo_rate,0,7) * 32));
    Impl::write(impl_->opm, 0x19, static_cast<uint8_t>(std::clamp(c.am_depth,0,127)));
    Impl::write(impl_->opm, 0x19, static_cast<uint8_t>(0x80 | std::clamp(c.pm_depth,0,127)));
    Impl::write(impl_->opm, 0x20, static_cast<uint8_t>(0xc0 | (fb << 3) | alg));
    const int midi = std::clamp(static_cast<int>(std::round(69.0 + 12.0 * std::log2(frequency / 440.0))), 0, 127);
    constexpr uint8_t notes[] = {0,1,2,4,5,6,8,9,10,12,13,14};
    Impl::write(impl_->opm, 0x28, static_cast<uint8_t>((std::clamp(midi / 12 - 1, 0, 7) << 4) | notes[midi % 12]));
    Impl::write(impl_->opm, 0x30, 0); Impl::write(impl_->opm, 0x38, 0x00); Impl::write(impl_->opm, 0x08, 0x78);
    return;
  }
  constexpr uint8_t slots[] = {0x00, 0x04, 0x08, 0x0c};
  auto opn_write = [&](uint16_t reg, uint8_t data) {
    if (kind == Kind::Opn) Impl::write(impl_->opn1, reg, data);
    else if (kind == Kind::Opna) Impl::write(impl_->opna, reg, data);
    else Impl::write(impl_->opn, reg, data);
  };
  for (int op = 0; op < 4; ++op) {
    const uint8_t s = slots[op];
    opn_write(static_cast<uint16_t>(0x30 + s), static_cast<uint8_t>((std::clamp(c.detune,0,7) << 4) | (1 + op)));
    opn_write(static_cast<uint16_t>(0x40 + s), static_cast<uint8_t>(op == 3 ? carrier_level : 20 + op * 7));
    opn_write(static_cast<uint16_t>(0x50 + s), static_cast<uint8_t>((std::clamp(c.key_scale,0,3) << 6) | std::clamp(c.attack,0,31)));
    opn_write(static_cast<uint16_t>(0x60 + s), static_cast<uint8_t>((c.am_depth ? 0x80 : 0) | std::clamp(c.decay,0,31)));
    opn_write(static_cast<uint16_t>(0x70 + s), static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));
    opn_write(static_cast<uint16_t>(0x80 + s), static_cast<uint8_t>((std::clamp(c.sustain_level,0,15) << 4) | std::clamp(c.release,0,15)));
  }
  if (kind != Kind::Opn) opn_write(0x22, static_cast<uint8_t>((c.am_depth || c.pm_depth ? 8 : 0) | std::clamp(c.lfo_rate,0,7)));
  opn_write(0xb0, static_cast<uint8_t>((fb << 3) | alg));
  if (kind != Kind::Opn) opn_write(0xb4, static_cast<uint8_t>(0xc0 | (std::min(3,c.am_depth/32) << 4) | std::min(7,c.pm_depth/16)));
  int block = 0;
  double fnum = 0;
  for (; block < 7; ++block) {
    const double active_clock = kind == Kind::Opn ? Impl::opn_clock : (kind == Kind::Opna ? Impl::opna_clock : Impl::opn2_clock);
    fnum = frequency * 144.0 * 1048576.0 / (active_clock * std::pow(2.0, block));
    if (fnum < 2048.0) break;
  }
  const int fn = std::clamp(static_cast<int>(std::round(fnum)), 0, 2047);
  opn_write(0xa4, static_cast<uint8_t>((block << 3) | (fn >> 8)));
  opn_write(0xa0, static_cast<uint8_t>(fn));
  opn_write(0x28, 0xf0);
}
void HardwareFmVoice::key_off() {
  if (impl_->kind == Kind::Ym2612) Impl::write(impl_->opn, 0x28, 0);
  else if (impl_->kind == Kind::Opn) Impl::write(impl_->opn1, 0x28, 0);
  else if (impl_->kind == Kind::Opna) Impl::write(impl_->opna, 0x28, 0);
  else if (impl_->kind == Kind::Opl2) Impl::write(impl_->opl, 0xb0, 0);
  else if (impl_->kind == Kind::Opl3) Impl::write(impl_->opl3, 0xb0, 0);
  else Impl::write(impl_->opm, 0x08, 0);
}
void HardwareFmVoice::update_controls(const FmControls& c) {
  if (!impl_->programmed) return;
  const int alg=std::clamp(c.algorithm,0,7),fb=std::clamp(c.feedback,0,7);
  const int carrier=std::clamp(static_cast<int>((1.0-c.brightness)*32.0),0,48);
  if (impl_->kind==Kind::Opl2||impl_->kind==Kind::Opl3) {
    auto apply=[&](auto& chip){constexpr uint8_t slots[]={0,3};for(int op=0;op<2;++op){const uint8_t s=slots[op];
      Impl::write(chip,static_cast<uint16_t>(0x20+s),static_cast<uint8_t>((c.key_scale?0x10:0)|0x40|(op+1)));
      Impl::write(chip,static_cast<uint16_t>(0x40+s),static_cast<uint8_t>(op?carrier:18));
      Impl::write(chip,static_cast<uint16_t>(0x60+s),static_cast<uint8_t>((std::clamp(c.attack,0,15)<<4)|std::clamp(c.decay,0,15)));
      Impl::write(chip,static_cast<uint16_t>(0x80+s),static_cast<uint8_t>((std::clamp(c.sustain_level,0,15)<<4)|std::clamp(c.release,0,15)));}
      Impl::write(chip,0xc0,static_cast<uint8_t>(0x30|(fb<<1)|(alg&1)));};
    if(impl_->kind==Kind::Opl2)apply(impl_->opl);else apply(impl_->opl3);return;
  }
  if (impl_->kind==Kind::Opm) {constexpr uint8_t slots[]={0,8,16,24};for(int op=0;op<4;++op){const uint8_t s=slots[op];
    Impl::write(impl_->opm,static_cast<uint16_t>(0x40+s),static_cast<uint8_t>((std::clamp(c.detune,0,7)<<4)|(op+1)));
    Impl::write(impl_->opm,static_cast<uint16_t>(0x60+s),static_cast<uint8_t>(op==3?carrier:20+op*7));
    Impl::write(impl_->opm,static_cast<uint16_t>(0x80+s),static_cast<uint8_t>((std::clamp(c.key_scale,0,3)<<6)|std::clamp(c.attack,0,31)));
    Impl::write(impl_->opm,static_cast<uint16_t>(0xa0+s),static_cast<uint8_t>((c.am_depth?0x80:0)|std::clamp(c.decay,0,31)));
    Impl::write(impl_->opm,static_cast<uint16_t>(0xc0+s),static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));
    Impl::write(impl_->opm,static_cast<uint16_t>(0xe0+s),static_cast<uint8_t>((std::clamp(c.sustain_level,0,15)<<4)|std::clamp(c.release,0,15)));}
    Impl::write(impl_->opm,0x18,static_cast<uint8_t>(std::clamp(c.lfo_rate,0,7)*32));Impl::write(impl_->opm,0x19,static_cast<uint8_t>(std::clamp(c.am_depth,0,127)));Impl::write(impl_->opm,0x19,static_cast<uint8_t>(0x80|std::clamp(c.pm_depth,0,127)));Impl::write(impl_->opm,0x20,static_cast<uint8_t>(0xc0|(fb<<3)|alg));return;
  }
  auto write=[&](uint16_t r,uint8_t d){if(impl_->kind==Kind::Opn)Impl::write(impl_->opn1,r,d);else if(impl_->kind==Kind::Opna)Impl::write(impl_->opna,r,d);else Impl::write(impl_->opn,r,d);};
  constexpr uint8_t slots[]={0,4,8,12};for(int op=0;op<4;++op){const uint8_t s=slots[op];write(static_cast<uint16_t>(0x30+s),static_cast<uint8_t>((std::clamp(c.detune,0,7)<<4)|(op+1)));write(static_cast<uint16_t>(0x40+s),static_cast<uint8_t>(op==3?carrier:20+op*7));write(static_cast<uint16_t>(0x50+s),static_cast<uint8_t>((std::clamp(c.key_scale,0,3)<<6)|std::clamp(c.attack,0,31)));write(static_cast<uint16_t>(0x60+s),static_cast<uint8_t>((c.am_depth?0x80:0)|std::clamp(c.decay,0,31)));write(static_cast<uint16_t>(0x70+s),static_cast<uint8_t>(std::clamp(c.sustain_rate,0,31)));write(static_cast<uint16_t>(0x80+s),static_cast<uint8_t>((std::clamp(c.sustain_level,0,15)<<4)|std::clamp(c.release,0,15)));}
  write(0xb0,static_cast<uint8_t>((fb<<3)|alg));if(impl_->kind!=Kind::Opn){write(0x22,static_cast<uint8_t>((c.am_depth||c.pm_depth?8:0)|std::clamp(c.lfo_rate,0,7)));write(0xb4,static_cast<uint8_t>(0xc0|(std::min(3,c.am_depth/32)<<4)|std::min(7,c.pm_depth/16)));}
}
float HardwareFmVoice::render(double host_rate, double frequency) {
  // Tracker slides, CLAP tuning, pitch bend, vibrato, and portamento arrive continuously.
  // Only emit register writes when the chip's quantized pitch representation changes.
  if (impl_->kind == Kind::Opm) {
    const double midi_f = std::clamp(69.0 + 12.0 * std::log2(std::max(1.0, frequency) / 440.0), 0.0, 127.999);
    const int midi = static_cast<int>(std::floor(midi_f));
    constexpr uint8_t notes[] = {0,1,2,4,5,6,8,9,10,12,13,14};
    const int kc = (std::clamp(midi / 12 - 1, 0, 7) << 4) | notes[midi % 12];
    const int kf = std::clamp(static_cast<int>((midi_f - midi) * 64.0), 0, 63) << 2;
    if (kc != impl_->last_pitch_a) { Impl::write(impl_->opm, 0x28, static_cast<uint8_t>(kc)); impl_->last_pitch_a = kc; }
    if (kf != impl_->last_pitch_b) { Impl::write(impl_->opm, 0x30, static_cast<uint8_t>(kf)); impl_->last_pitch_b = kf; }
  } else if (impl_->kind == Kind::Opl2 || impl_->kind == Kind::Opl3) {
    int block = 0; double fnum = 0;
    for (; block < 7; ++block) { fnum = frequency * 72.0 * 1048576.0 / (Impl::opl_clock * std::pow(2.0, block)); if (fnum < 1024.0) break; }
    const int fn = std::clamp(static_cast<int>(std::round(fnum)), 0, 1023), hi = 0x20 | (block << 2) | (fn >> 8);
    if (fn != impl_->last_pitch_a || hi != impl_->last_pitch_b) {
      if (impl_->kind == Kind::Opl2) { Impl::write(impl_->opl, 0xa0, static_cast<uint8_t>(fn)); Impl::write(impl_->opl, 0xb0, static_cast<uint8_t>(hi)); }
      else { Impl::write(impl_->opl3, 0xa0, static_cast<uint8_t>(fn)); Impl::write(impl_->opl3, 0xb0, static_cast<uint8_t>(hi)); }
      impl_->last_pitch_a = fn; impl_->last_pitch_b = hi;
    }
  } else {
    const double clock = impl_->kind == Kind::Opn ? Impl::opn_clock : (impl_->kind == Kind::Opna ? Impl::opna_clock : Impl::opn2_clock);
    int block = 0; double fnum = 0;
    for (; block < 7; ++block) { fnum = frequency * 144.0 * 1048576.0 / (clock * std::pow(2.0, block)); if (fnum < 2048.0) break; }
    const int fn = std::clamp(static_cast<int>(std::round(fnum)), 0, 2047), hi = (block << 3) | (fn >> 8);
    if (fn != impl_->last_pitch_a || hi != impl_->last_pitch_b) {
      if (impl_->kind == Kind::Opn) { Impl::write(impl_->opn1, 0xa4, static_cast<uint8_t>(hi)); Impl::write(impl_->opn1, 0xa0, static_cast<uint8_t>(fn)); }
      else if (impl_->kind == Kind::Opna) { Impl::write(impl_->opna, 0xa4, static_cast<uint8_t>(hi)); Impl::write(impl_->opna, 0xa0, static_cast<uint8_t>(fn)); }
      else { Impl::write(impl_->opn, 0xa4, static_cast<uint8_t>(hi)); Impl::write(impl_->opn, 0xa0, static_cast<uint8_t>(fn)); }
      impl_->last_pitch_a = fn; impl_->last_pitch_b = hi;
    }
  }
  double native_rate = 0;
  if (impl_->kind == Kind::Ym2612) native_rate = impl_->opn.sample_rate(Impl::opn2_clock);
  else if (impl_->kind == Kind::Opn) native_rate = impl_->opn1.sample_rate(Impl::opn_clock);
  else if (impl_->kind == Kind::Opna) native_rate = impl_->opna.sample_rate(Impl::opna_clock);
  else if (impl_->kind == Kind::Opl2) native_rate = impl_->opl.sample_rate(Impl::opl_clock);
  else if (impl_->kind == Kind::Opl3) native_rate = impl_->opl3.sample_rate(Impl::opl3_clock);
  else native_rate = impl_->opm.sample_rate(Impl::opm_clock);
  impl_->accumulator += native_rate / host_rate;
  while (impl_->accumulator >= 1.0) {
    if (impl_->kind == Kind::Ym2612) { ymfm::ym2612::output_data out{}; impl_->opn.generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / 65536.0); }
    else if (impl_->kind == Kind::Opn) { ymfm::ym2203::output_data out{}; impl_->opn1.generate(&out); impl_->last = static_cast<float>(out.data[0] / 32768.0); }
    else if (impl_->kind == Kind::Opna) { ymfm::ym2608::output_data out{}; impl_->opna.generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / 65536.0); }
    else if (impl_->kind == Kind::Opl2) { ymfm::ym3812::output_data out{}; impl_->opl.generate(&out); impl_->last = static_cast<float>(out.data[0] / 32768.0); }
    else if (impl_->kind == Kind::Opl3) { ymfm::ymf262::output_data out{}; impl_->opl3.generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / 65536.0); }
    else { ymfm::ym2151::output_data out{}; impl_->opm.generate(&out); impl_->last = static_cast<float>((out.data[0] + out.data[1]) / 65536.0); }
    impl_->accumulator -= 1.0;
  }
  return impl_->last;
}
}
