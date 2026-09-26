// Informational CPU measurements through the public CLAP API. No machine-dependent pass threshold.
#include "clap_harness.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>

using namespace harness;
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: yanes-benchmark PLUGIN.clap\n"; return 2; }
  const Library library(argv[1]);
  const auto* plugin = library.create();
  const auto waveform = find_param(plugin, "Waveform");
  const int last = static_cast<int>(param_info(plugin, waveform).max_value);
  std::cout << "source,name,voices,sample_rate,block_frames,mean_block_us,max_block_us,realtime_percent\n";
  for (int source = 0; source <= last; ++source) {
    for (int voices : {1, 16}) {
      Runner runner(plugin, 48000, 128);
      runner.set(waveform, source);
      Events on;
      for (int voice = 0; voice < voices; ++voice)
        on.push(note_event(CLAP_EVENT_NOTE_ON, static_cast<int16_t>(voice), static_cast<int16_t>(48 + voice), voice, 0.8));
      runner.run(&on);
      runner.settle(4); // Exclude activation, note setup, and warm-up from steady-state timing.
      double total = 0, worst = 0;
      for (int block = 0; block < 128; ++block) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = runner.run();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        if (!result.finite) { std::cerr << "nonfinite source " << source << '\n'; return 1; }
        total += us; worst = std::max(worst, us);
      }
      char name[128]{};
      params_of(plugin)->value_to_text(plugin, waveform, source, name, sizeof(name));
      std::cout << source << ",\"" << name << "\"," << voices << ",48000,128," << std::fixed
                << std::setprecision(3) << total / 128 << ',' << worst << ',' << (total / 128) / (128.0 / 48000 * 1e6) * 100 << '\n';
    }
  }
  plugin->destroy(plugin);
}
