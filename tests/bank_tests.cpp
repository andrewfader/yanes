// DPCM sample bank coverage: the WAV parser and one-bit encoder reached through
// YANES_DPCM_BANK, rejection of unsupported files, persistence of loaded banks in CLAP
// state, playback through the DPCM voice, and agreement between the plug-in's encoder
// and the yanes-dpcm command line tool.
#include "clap_harness.hpp"
#include "platform_test.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <array>

using namespace harness;

namespace {

namespace fs = std::filesystem;

fs::path g_dir;

void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
void put_tag(std::vector<uint8_t>& out, const char* tag) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
}

struct WavSpec {
  uint16_t format{1};
  uint16_t channels{1};
  uint32_t rate{8000};
  uint16_t bits{16};
  size_t frames{1000};
  bool extra_chunk{false};   // insert a LIST chunk before "data"
  bool odd_chunk{false};     // odd-sized chunk, exercising the pad byte
  bool truncate_data{false}; // declare more data than the file contains
};

std::vector<uint8_t> make_wav(const WavSpec& spec) {
  const uint16_t frame_bytes = static_cast<uint16_t>(spec.channels * spec.bits / 8);
  std::vector<uint8_t> samples;
  for (size_t f = 0; f < spec.frames; ++f) {
    // A slow ramp, so the encoder produces a mix of up and down deltas.
    const int value = static_cast<int>(20000.0 * std::sin(6.28318530718 * static_cast<double>(f) / 220.0));
    for (uint16_t c = 0; c < spec.channels; ++c) {
      if (spec.bits == 16) put16(samples, static_cast<uint16_t>(static_cast<int16_t>(value)));
      else if (spec.bits == 8) samples.push_back(static_cast<uint8_t>(128 + value / 512));
      else for (int i = 0; i < spec.bits / 8; ++i) samples.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
  }

  std::vector<uint8_t> body;
  put_tag(body, "WAVE");
  put_tag(body, "fmt ");
  put32(body, 16);
  put16(body, spec.format);
  put16(body, spec.channels);
  put32(body, spec.rate);
  put32(body, spec.rate * frame_bytes);
  put16(body, frame_bytes);
  put16(body, spec.bits);
  if (spec.extra_chunk) {
    put_tag(body, "LIST");
    const uint32_t n = spec.odd_chunk ? 5 : 6;
    put32(body, n);
    for (uint32_t i = 0; i < n; ++i) body.push_back(static_cast<uint8_t>('a' + i));
    if (n & 1U) body.push_back(0);  // RIFF chunks are word aligned
  }
  put_tag(body, "data");
  put32(body, static_cast<uint32_t>(samples.size()));
  if (spec.truncate_data) samples.resize(samples.size() / 2);
  body.insert(body.end(), samples.begin(), samples.end());

  std::vector<uint8_t> wav;
  put_tag(wav, "RIFF");
  put32(wav, static_cast<uint32_t>(body.size()));
  wav.insert(wav.end(), body.begin(), body.end());
  return wav;
}

std::string write_file(const std::string& name, const std::vector<uint8_t>& bytes) {
  const fs::path path = g_dir / name;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  assert(out);
  return path.string();
}

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

// The encoder emits floor(frames * 16744 / rate) bits, packed eight to a byte.
uint32_t expected_encoded_bytes(size_t frames, uint32_t rate) {
  const size_t bits = static_cast<size_t>(static_cast<double>(frames) * 16744.0 / rate);
  return static_cast<uint32_t>((bits + 7U) / 8U);
}

// Reads back the per-slot sizes the plug-in reports in saved state.
std::array<uint32_t, 16> bank_sizes(const clap_plugin_t* plugin) {
  StateMemory saved;
  assert(save_state(plugin, &saved));
  const size_t sizes_at = 8 + params_of(plugin)->count(plugin) * sizeof(double);
  std::array<uint32_t, 16> sizes{};
  assert(saved.bytes.size() >= sizes_at + sizeof(sizes));
  std::memcpy(sizes.data(), saved.bytes.data() + sizes_at, sizeof(sizes));
  return sizes;
}

// Creates an instance with YANES_DPCM_BANK set to the platform path-list syntax.
const clap_plugin_t* create_with_bank(const Library& library, const std::string& list) {
  assert(test_platform::set_environment("YANES_DPCM_BANK", list));
  const clap_plugin_t* plugin = library.create();
  test_platform::unset_environment("YANES_DPCM_BANK");
  return plugin;
}

// --- loading -----------------------------------------------------------------------

void test_wav_and_ydmc_loading(const Library& library) {
  const std::string mono = write_file("mono.wav", make_wav({}));
  const std::string stereo = write_file("stereo.wav", make_wav({1, 2, 44100, 16, 4410, false, false, false}));
  const std::string high_rate = write_file("high.wav", make_wav({1, 1, 96000, 16, 9600, false, false, false}));
  // A raw .ydmc payload is stored verbatim.
  std::vector<uint8_t> raw(777);
  std::iota(raw.begin(), raw.end(), static_cast<uint8_t>(0));
  const std::string ydmc = write_file("raw.ydmc", raw);

  const clap_plugin_t* plugin =
      create_with_bank(library, mono + test_platform::path_list_separator() + stereo +
                                    test_platform::path_list_separator() + ydmc +
                                    test_platform::path_list_separator() + high_rate);
  const auto sizes = bank_sizes(plugin);
  assert(sizes[0] == expected_encoded_bytes(1000, 8000));
  assert(sizes[1] == expected_encoded_bytes(4410, 44100));
  assert(sizes[2] == raw.size());
  assert(sizes[3] == expected_encoded_bytes(9600, 96000));
  for (size_t slot = 4; slot < 16; ++slot) assert(sizes[slot] == 0);
  plugin->destroy(plugin);
}

// Every slot in the sixteen-slot bank must be reachable from the environment variable.
void test_all_sixteen_slots(const Library& library) {
  std::string list;
  for (int slot = 0; slot < 16; ++slot) {
    std::vector<uint8_t> raw(static_cast<size_t>(16 + slot));
    std::iota(raw.begin(), raw.end(), static_cast<uint8_t>(slot));
    if (slot) list += test_platform::path_list_separator();
    list += write_file("slot" + std::to_string(slot) + ".ydmc", raw);
  }
  const clap_plugin_t* plugin = create_with_bank(library, list);
  const auto sizes = bank_sizes(plugin);
  for (int slot = 0; slot < 16; ++slot) assert(sizes[static_cast<size_t>(slot)] == 16U + static_cast<uint32_t>(slot));
  plugin->destroy(plugin);
}

// Empty entries skip a slot; a seventeenth entry has nowhere to go and is dropped.
void test_empty_entries_and_overflow(const Library& library) {
  std::vector<uint8_t> raw(64, 0x11);
  const std::string one = write_file("one.ydmc", raw);
  const std::string separators(2, test_platform::path_list_separator());
  const clap_plugin_t* plugin = create_with_bank(
      library, std::string(1, test_platform::path_list_separator()) + one + separators + one);
  const auto sizes = bank_sizes(plugin);
  assert(sizes[0] == 0);
  assert(sizes[1] == raw.size());
  assert(sizes[2] == 0);
  assert(sizes[3] == raw.size());
  plugin->destroy(plugin);

  std::string overflow;
  for (int i = 0; i < 20; ++i) {
    if (i) overflow += test_platform::path_list_separator();
    overflow += one;
  }
  const clap_plugin_t* wide = create_with_bank(library, overflow);
  const auto wide_sizes = bank_sizes(wide);
  for (size_t slot = 0; slot < 16; ++slot) assert(wide_sizes[slot] == raw.size());
  wide->destroy(wide);
}

// Unsupported or malformed inputs must be refused, leaving the slot empty rather than
// installing garbage or reading out of bounds.
void test_rejected_inputs(const Library& library) {
  struct Case { const char* name; std::vector<uint8_t> bytes; };
  std::vector<Case> cases;
  cases.push_back({"eight_bit.wav", make_wav({1, 1, 8000, 8, 1000, false, false, false})});
  cases.push_back({"twenty_four.wav", make_wav({1, 1, 8000, 24, 1000, false, false, false})});
  cases.push_back({"float.wav", make_wav({3, 1, 8000, 16, 1000, false, false, false})});
  cases.push_back({"five_channel.wav", make_wav({1, 5, 8000, 16, 1000, false, false, false})});
  cases.push_back({"zero_rate.wav", make_wav({1, 1, 0, 16, 1000, false, false, false})});
  cases.push_back({"one_frame.wav", make_wav({1, 1, 8000, 16, 0, false, false, false})});
  cases.push_back({"truncated.wav", make_wav({1, 1, 8000, 16, 1000, false, false, true})});
  // RIFF header with nothing behind it.
  cases.push_back({"stub.wav", [] {
    std::vector<uint8_t> b; put_tag(b, "RIFF"); put32(b, 4); put_tag(b, "WAVE"); return b;
  }()});
  // Over the 1 MiB limit for a raw payload.
  cases.push_back({"huge.ydmc", std::vector<uint8_t>(1024U * 1024U + 1U, 0x7f)});
  // Empty file.
  cases.push_back({"empty.ydmc", {}});

  for (const Case& test : cases) {
    const std::string path = write_file(test.name, test.bytes);
    const clap_plugin_t* plugin = create_with_bank(library, path);
    const auto sizes = bank_sizes(plugin);
    if (sizes[0] != 0) {
      std::fprintf(stderr, "%s should have been rejected, loaded %u bytes\n", test.name, sizes[0]);
      assert(false);
    }
    plugin->destroy(plugin);
  }

  // A path that does not exist must not stop the remaining slots from loading.
  std::vector<uint8_t> raw(48, 0x22);
  const std::string good = write_file("good.ydmc", raw);
  const clap_plugin_t* plugin =
      create_with_bank(library, (g_dir / "missing.ydmc").string() +
                                    test_platform::path_list_separator() + good);
  const auto sizes = bank_sizes(plugin);
  assert(sizes[0] == 0 && sizes[1] == raw.size());
  plugin->destroy(plugin);
}

// A raw payload of exactly 1 MiB is the documented maximum and must be accepted.
void test_size_limits(const Library& library) {
  const std::string at_limit = write_file("limit.ydmc", std::vector<uint8_t>(1024U * 1024U, 0x33));
  const clap_plugin_t* plugin = create_with_bank(library, at_limit);
  assert(bank_sizes(plugin)[0] == 1024U * 1024U);
  plugin->destroy(plugin);
}

// Extra RIFF chunks, including odd-sized ones needing the pad byte, must be skipped.
void test_chunk_walking(const Library& library) {
  const std::string even = write_file("chunky.wav", make_wav({1, 1, 8000, 16, 1000, true, false, false}));
  const std::string odd = write_file("chunky_odd.wav", make_wav({1, 1, 8000, 16, 1000, true, true, false}));
  const clap_plugin_t* plugin = create_with_bank(
      library, even + test_platform::path_list_separator() + odd);
  const auto sizes = bank_sizes(plugin);
  const uint32_t expected = expected_encoded_bytes(1000, 8000);
  assert(sizes[0] == expected);
  assert(sizes[1] == expected);
  plugin->destroy(plugin);
}

// --- agreement with the command line tool -------------------------------------------

void test_matches_command_line_tool(const Library& library) {
  const char* tool = std::getenv("YANES_DPCM_TOOL");
  if (!tool) {
    std::fprintf(stderr, "YANES_DPCM_TOOL not set, skipping the encoder cross-check\n");
    return;
  }
  const WavSpec specs[] = {
      {1, 1, 8000, 16, 1000, false, false, false},
      {1, 2, 44100, 16, 4410, false, false, false},
      {1, 1, 22050, 16, 3333, false, false, false},
  };
  int index = 0;
  for (const WavSpec& spec : specs) {
    const std::string name = "tool" + std::to_string(index++);
    const std::string wav = write_file(name + ".wav", make_wav(spec));
    const std::string out = (g_dir / (name + ".ydmc")).string();
    const std::string command = "\"" + std::string(tool) + "\" \"" + wav + "\" \"" + out + "\" > " + test_platform::null_device();
    assert(std::system(command.c_str()) == 0);
    const std::vector<uint8_t> from_tool = read_file(out);

    // The plug-in encodes the same WAV; the resulting bank must be identical, and the
    // pre-encoded file must load back unchanged.
    const clap_plugin_t* plugin = create_with_bank(
        library, wav + test_platform::path_list_separator() + out);
    const auto sizes = bank_sizes(plugin);
    assert(sizes[0] == from_tool.size());
    assert(sizes[1] == from_tool.size());

    StateMemory saved;
    assert(save_state(plugin, &saved));
    const size_t payload_at = 8 + params_of(plugin)->count(plugin) * sizeof(double) + 16 * sizeof(uint32_t);
    // Slot 0 (encoded by the plug-in) must byte-match slot 1 (encoded by the tool).
    assert(std::equal(from_tool.begin(), from_tool.end(), saved.bytes.begin() + static_cast<long>(payload_at)));
    assert(std::equal(from_tool.begin(), from_tool.end(),
                      saved.bytes.begin() + static_cast<long>(payload_at + from_tool.size())));
    plugin->destroy(plugin);
  }

  // The tool reports failure rather than writing nonsense.
  const std::string bad = write_file("bad_for_tool.wav", std::vector<uint8_t>(64, 0x00));
  const std::string out = (g_dir / "bad.ydmc").string();
  assert(std::system(("\"" + std::string(tool) + "\" \"" + bad + "\" \"" + out + "\" 2> " + test_platform::null_device()).c_str()) != 0);
  assert(std::system(("\"" + std::string(tool) + "\" 2> " + test_platform::null_device()).c_str()) != 0);
}

// --- playback -----------------------------------------------------------------------

// Loaded slots must actually be reachable from the keyboard, at the documented base key
// mapping, and every combination of loop mask and trim must stay finite.
void test_bank_playback(const Library& library) {
  std::string list;
  for (int slot = 0; slot < 16; ++slot) {
    std::vector<uint8_t> raw(256);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint8_t>((i * (slot + 3)) & 0xff);
    if (slot) list += test_platform::path_list_separator();
    list += write_file("play" + std::to_string(slot) + ".ydmc", raw);
  }
  const clap_plugin_t* plugin = create_with_bank(library, list);
  {
    Runner runner(plugin, 48000.0, 512);
    const clap_id waveform = find_param(plugin, "Waveform");
    const clap_id base_key = find_param(plugin, "DPCM base key");
    const clap_id loop_mask = find_param(plugin, "DPCM loop mask");
    const clap_id initial = find_param(plugin, "DPCM initial level");
    const clap_id trim_start = find_param(plugin, "DPCM trim start");
    const clap_id trim_end = find_param(plugin, "DPCM trim end");

    runner.set(waveform, 9);  // NES DPCM drums
    runner.set(base_key, 36);
    runner.set(loop_mask, 65535);

    for (int slot = 0; slot < 16; ++slot) {
      Events on;
      on.push(note_event(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(36 + slot), slot, 1.0));
      Block block = runner.run(&on);
      assert(block.finite);
      for (int i = 0; i < 8; ++i) { block = runner.run(); assert(block.finite); }
      Events off;
      off.push(note_event(CLAP_EVENT_NOTE_OFF, -1, -1, -1, 0.0));
      runner.run(&off);
      runner.settle(40);
      assert(runner.run().peak == 0.0f);
    }

    // Keys outside the bank range clamp to the first and last slot rather than
    // indexing out of bounds.
    for (const int16_t key : {int16_t{0}, int16_t{35}, int16_t{52}, int16_t{127}}) {
      Events on;
      on.push(note_event(CLAP_EVENT_NOTE_ON, 0, key, 1, 1.0));
      assert(runner.run(&on).finite);
      assert(runner.settle(4).finite);
      plugin->reset(plugin);
    }

    // Trim and DAC controls, including the inverted range where start is past end.
    for (const double start : {0.0, 0.5, 0.95}) {
      for (const double end : {0.05, 0.5, 1.0}) {
        for (const double mask : {0.0, 65535.0}) {
          runner.set(trim_start, start);
          runner.set(trim_end, end);
          runner.set(loop_mask, mask);
          runner.set(initial, start > 0.4 ? 0 : 127);
          Events on;
          on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 40, 1, 1.0));
          assert(runner.run(&on).finite);
          assert(runner.settle(6).finite);
          plugin->reset(plugin);
        }
      }
    }
  }
  plugin->destroy(plugin);
}

// With no bank loaded the generated kick/snare fallback must still sound.
void test_generated_fallback(const Library& library) {
  test_platform::unset_environment("YANES_DPCM_BANK");
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, 48000.0, 512);
    runner.set(find_param(plugin, "Waveform"), 9);
    assert(bank_sizes(plugin)[0] == 0);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 36, 1, 1.0));
    const Block block = runner.run(&on);
    assert(block.finite && block.peak > 1.0e-3f);
  }
  plugin->destroy(plugin);
}

// Replacing a bank while a voice is sounding must not disturb it: the plug-in documents
// immutable snapshots for exactly this case.
void test_bank_replacement_during_playback(const Library& library) {
  std::vector<uint8_t> first(512, 0xaa);
  const std::string path = write_file("swap.ydmc", first);
  const clap_plugin_t* plugin = create_with_bank(library, path);
  {
    Runner runner(plugin, 48000.0, 512);
    runner.set(find_param(plugin, "Waveform"), 9);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 36, 1, 1.0));
    assert(runner.run(&on).finite);

    // Loading state swaps every slot underneath the sounding voice.
    const uint32_t count = params_of(plugin)->count(plugin);
    StateMemory replacement;
    assert(save_state(plugin, &replacement));
    assert(load_state(plugin, replacement));
    for (int i = 0; i < 8; ++i) assert(runner.run().finite);

    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, -1, -1, -1, 0.0));
    runner.run(&off);
    runner.settle(40);
    assert(runner.run().peak == 0.0f);
    assert(count > 0);
  }
  plugin->destroy(plugin);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  g_dir = fs::temp_directory_path() / ("yanes-bank-tests-" + std::to_string(test_platform::process_id()));
  fs::create_directories(g_dir);

  {
    const Library library(argv[1]);
    test_wav_and_ydmc_loading(library);
    test_all_sixteen_slots(library);
    test_empty_entries_and_overflow(library);
    test_rejected_inputs(library);
    test_size_limits(library);
    test_chunk_walking(library);
    test_matches_command_line_tool(library);
    test_bank_playback(library);
    test_generated_fallback(library);
    test_bank_replacement_during_playback(library);
  }

  fs::remove_all(g_dir);
  std::printf("bank_tests: all checks passed\n");
}
