#include "clap_harness.hpp"
#include <limits>

using namespace harness;

namespace {
clap_event_param_mod_t modulation(clap_id id, double amount, uint32_t time = 0) {
  clap_event_param_mod_t event{};
  event.header = {sizeof(event), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0};
  event.param_id = id;
  event.note_id = event.port_index = event.channel = event.key = -1;
  event.amount = amount;
  return event;
}

struct Case { int source; const char* name; double amount; };

// An offset must sound exactly like the equivalent base-value automation, at
// the same sample, including when base automation arrives while it is active.
std::vector<float> render(const Library& library, Case test, bool use_mod, uint32_t frames) {
  const auto* plugin = library.create();
  std::vector<float> result;
  {
    Runner runner(plugin, 48000, frames);
    runner.set(find_param(plugin, "Waveform"), test.source);
    const auto id = find_param(plugin, test.name);
    const auto info = param_info(plugin, id);
    const double base = param(plugin, id);
    const double next_base = std::clamp(base + test.amount * 0.25, info.min_value, info.max_value);
    const double target = std::clamp(base + test.amount, info.min_value, info.max_value);
    const double next_target = std::clamp(next_base + test.amount, info.min_value, info.max_value);
    // Runner::set renders one block; reset so both block sizes begin at the
    // same chip-clock phase, rather than after different amounts of silence.
    plugin->reset(plugin);
    for (uint32_t at = 0; at < 8192; at += frames) {
      Events events;
      if (at == 0) events.push(note_event(CLAP_EVENT_NOTE_ON, 0, 69, 1, 1));
      for (const auto time : {4224U, 5184U, 6208U}) {
        if (time < at || time >= at + frames) continue;
        if (use_mod && time != 5184) {
          events.push(modulation(id, time == 4224 ? test.amount : 0.0, time - at));
        } else {
          auto event = param_event(id, time == 4224 ? target :
              time == 5184 ? (use_mod ? next_base : next_target) : next_base);
          event.header.time = time - at;
          events.push(event);
        }
      }
      assert(runner.run(&events).finite);
      result.insert(result.end(), runner.left().begin(), runner.left().end());
    }
    assert(param(plugin, id) == next_base);
  }
  plugin->destroy(plugin);
  return result;
}

void test_sample_timing(const Library& library) {
  for (const auto test : {Case{0, "Master", -12}, Case{0, "Fine tune", 75},
       Case{18, "Fine tune", 75}, Case{17, "FM brightness", -0.65},
       Case{27, "FM feedback", 4}, Case{30, "FM brightness", -0.65},
       Case{46, "Table position", 0.7}, Case{0, "Echo mix", 0.7},
       Case{0, "Master", -1000}}) {
    const auto expected = render(library, test, false, 512);
    assert(render(library, test, true, 512) == expected && "modulation must equal base plus offset");
    const auto split = render(library, test, true, 128);
    if (split != expected) {
      const auto at = static_cast<size_t>(std::mismatch(split.begin(), split.end(), expected.begin()).first - split.begin());
      std::fprintf(stderr, "modulation source=%d parameter=%s sample=%zu split=%g whole=%g\n",
          test.source, test.name, at, split[at], expected[at]);
      assert(false && "modulation must be sample accurate");
    }
  }
}

void test_state_reset_and_validation(const Library& library) {
  const auto* plugin = library.create();
  {
    Runner runner(plugin, 48000, 512);
    const auto master = find_param(plugin, "Master");
    const double base = param(plugin, master);
    for (clap_id id = 0; id < params_of(plugin)->count(plugin); ++id) {
      const auto info = param_info(plugin, id);
      assert(bool(info.flags & CLAP_PARAM_IS_MODULATABLE) == !(info.flags & CLAP_PARAM_IS_STEPPED));
      assert(!(info.flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID));
    }
    StateMemory before, after;
    assert(save_state(plugin, &before));
    Events change; change.push(modulation(master, -12));
    flush(plugin, change);
    assert(param(plugin, master) == base);
    assert(save_state(plugin, &after));
    assert(before.bytes == after.bytes && "transient modulation must not leak into project state");
    const auto* fresh = library.create();
    assert(load_state(fresh, after));
    assert(param(fresh, master) == base);
    fresh->destroy(fresh);

    Events on; on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 69, 1, 1));
    runner.run(&on); runner.settle(8);
    const auto quiet = runner.run();
    Events invalid;
    auto targeted = modulation(master, 6); targeted.note_id = 1;
    invalid.push(targeted);
    targeted = modulation(master, 6); targeted.channel = 0; invalid.push(targeted);
    invalid.push(modulation(master, std::numeric_limits<double>::quiet_NaN()));
    invalid.push(modulation(master, std::numeric_limits<double>::infinity()));
    invalid.push(modulation(CLAP_INVALID_ID, 1));
    invalid.push(modulation(find_param(plugin, "Waveform"), 17));
    clap_event_header_t short_event{sizeof(clap_event_header_t), 0,
        CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0};
    invalid.push(short_event);
    const auto ignored = runner.run(&invalid);
    assert(ignored.finite && ignored.rms < quiet.rms * 1.2);
    assert(param(plugin, find_param(plugin, "Waveform")) == 0);

    plugin->reset(plugin);
    runner.run(&on); runner.settle(8);
    const auto reset = runner.run();
    assert(reset.rms > quiet.rms * 3.9 && reset.rms < quiet.rms * 4.1);
    assert(param(plugin, master) == base);
  }
  plugin->destroy(plugin);
}
} // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const Library library(argv[1]);
  test_sample_timing(library);
  test_state_reset_and_validation(library);
  std::puts("modulation_tests: timing, base automation, FM updates, state, validation, and reset passed");
}
