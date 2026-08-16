#pragma once
#include <memory>

namespace yanes {
struct FmControls {
  int algorithm{}, feedback{3}, attack{31}, decay{10}, sustain_rate{5}, sustain_level{2}, release{6};
  int detune{}, key_scale{1}, lfo_rate{3}, am_depth{}, pm_depth{};
  double brightness{0.65};
};
class HardwareFmVoice {
public:
  enum class Kind { Ym2612, Opn, Opna, Opl2, Opl3, Opm, Opl3FourOp };
  HardwareFmVoice();
  ~HardwareFmVoice();
  HardwareFmVoice(HardwareFmVoice&&) noexcept;
  HardwareFmVoice& operator=(HardwareFmVoice&&) noexcept;
  void reset();
  void key_on(Kind kind, double frequency, const FmControls& controls);
  void key_off();
  void update_controls(const FmControls& controls);
  float render(double host_rate, double frequency);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
