// CLAP state coverage: the current v16 round trip including DPCM bank payloads, the
// legacy v8..v14 migration paths, and rejection of malformed blobs.
//
// The legacy layouts are reconstructed here from the reader's own struct definitions.
// That pins the on-disk contract so a future parameter insertion cannot silently shift
// an old project's values: adding a parameter anywhere other than the end of the enum
// breaks these tests.
#include "clap_harness.hpp"

#include <array>
#include <limits>

using namespace harness;

namespace {

constexpr uint32_t kMagic = 0x53454e59U;  // "YNES"

// Byte-level builder for a state blob, so the tests do not depend on the plug-in's
// private structs.
class Blob {
 public:
  Blob(uint32_t version, size_t value_count, size_t size_count)
      : value_count_(value_count), size_count_(size_count) {
    put32(kMagic);
    put32(version);
    values_at_ = bytes_.size();
    bytes_.resize(values_at_ + value_count * sizeof(double), 0);
    sizes_at_ = bytes_.size();
    bytes_.resize(sizes_at_ + size_count * sizeof(uint32_t), 0);
    // From v15 the editor size follows the sizes; these are the editor's defaults.
    if (version >= 15) { put32(1600); put32(1050); }
    // Trailing padding to the natural alignment of the writer's struct.
    while (bytes_.size() % alignof(double) != 0) bytes_.push_back(0);
  }

  void value(size_t index, double v) {
    assert(index < value_count_);
    std::memcpy(bytes_.data() + values_at_ + index * sizeof(double), &v, sizeof(v));
  }
  void size(size_t index, uint32_t v) {
    assert(index < size_count_);
    std::memcpy(bytes_.data() + sizes_at_ + index * sizeof(uint32_t), &v, sizeof(v));
  }
  void append(const std::vector<uint8_t>& payload) {
    bytes_.insert(bytes_.end(), payload.begin(), payload.end());
  }
  StateMemory memory() const { return StateMemory{bytes_, 0}; }
  std::vector<uint8_t>& bytes() { return bytes_; }

 private:
  void put32(uint32_t v) {
    const auto* b = reinterpret_cast<const uint8_t*>(&v);
    bytes_.insert(bytes_.end(), b, b + sizeof(v));
  }
  std::vector<uint8_t> bytes_;
  size_t value_count_, size_count_, values_at_{}, sizes_at_{};
};

// A value inside the parameter's declared range that is not its default, so a test can
// tell "migrated" apart from "fell back to the default".
double distinct_value(const clap_param_info_t& info) {
  const double candidate = info.default_value == info.max_value ? info.min_value : info.max_value;
  if (info.default_value == candidate) return info.min_value;
  return candidate;
}

void test_v16_round_trip(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const auto* params = params_of(plugin);
  const uint32_t count = params->count(plugin);

  // Drive every parameter away from its default, then save.
  std::vector<double> expected(count);
  Events changes;
  // Apply the preset selector first because selecting a preset intentionally changes
  // other parameters; the following events establish the exact snapshot to persist.
  const clap_id preset = find_param(plugin, "Preset");
  expected[preset] = distinct_value(param_info(plugin, preset));
  changes.push(param_event(preset, expected[preset]));
  for (clap_id i = 0; i < count; ++i) {
    if (i == preset) continue;
    const clap_param_info_t info = param_info(plugin, i);
    expected[i] = distinct_value(info);
    changes.push(param_event(i, expected[i]));
  }
  flush(plugin, changes);
  for (clap_id i = 0; i < count; ++i) assert(param(plugin, i) == expected[i]);

  StateMemory saved;
  assert(save_state(plugin, &saved));

  // Scramble, then reload.
  Events reset;
  for (clap_id i = 0; i < count; ++i) reset.push(param_event(i, param_info(plugin, i).default_value));
  flush(plugin, reset);
  assert(load_state(plugin, saved));
  for (clap_id i = 0; i < count; ++i) {
    if (param(plugin, i) != expected[i]) {
      std::fprintf(stderr, "parameter %u ('%s') round tripped to %.17g, expected %.17g\n",
                   i, param_info(plugin, i).name, param(plugin, i), expected[i]);
      assert(false);
    }
  }

  // State must load into a fresh instance as well, which is what reopening a project does.
  const clap_plugin_t* fresh = library.create();
  assert(load_state(fresh, saved));
  for (clap_id i = 0; i < count; ++i) assert(param(fresh, i) == expected[i]);
  fresh->destroy(fresh);
  plugin->destroy(plugin);
}

// Saving and reloading must preserve DPCM bank payloads byte for byte, which is what
// lets a project reopen without the original sample files.
void test_v16_bank_payload_round_trip(const Library& library) {
  const clap_plugin_t* plugin = library.create();

  // Install banks through a v16 blob, then save and compare the produced bytes.
  const uint32_t count = params_of(plugin)->count(plugin);
  Blob blob(16, count, 16);
  for (clap_id i = 0; i < count; ++i) blob.value(i, param_info(plugin, i).default_value);
  std::vector<uint8_t> payload;
  const uint32_t slot_sizes[16] = {32, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1024};
  for (size_t slot = 0; slot < 16; ++slot) {
    blob.size(slot, slot_sizes[slot]);
    for (uint32_t i = 0; i < slot_sizes[slot]; ++i)
      payload.push_back(static_cast<uint8_t>((slot * 31 + i * 7) & 0xff));
  }
  blob.append(payload);
  assert(load_state(plugin, blob.memory()));

  StateMemory resaved;
  assert(save_state(plugin, &resaved));
  assert(resaved.bytes.size() == blob.memory().bytes.size());
  assert(resaved.bytes == blob.memory().bytes);
  plugin->destroy(plugin);
}

// The editor size saved with a project comes back with it; nonsense sizes are ignored.
void test_editor_size_round_trip(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const uint32_t count = params_of(plugin)->count(plugin);
  const size_t gui_at = 8 + count * sizeof(double) + 16 * sizeof(uint32_t);
  const auto saved_size = [&](const clap_plugin_t* instance) {
    StateMemory out;
    assert(save_state(instance, &out));
    uint32_t size[2]{};
    std::memcpy(size, out.bytes.data() + gui_at, sizeof(size));
    return std::array<uint32_t, 2>{size[0], size[1]};
  };
  assert((saved_size(plugin) == std::array<uint32_t, 2>{1600, 1050}));

  Blob blob(16, count, 16);
  for (clap_id i = 0; i < count; ++i) blob.value(i, param_info(plugin, i).default_value);
  const uint32_t small[2] = {800, 525};
  std::memcpy(blob.bytes().data() + gui_at, small, sizeof(small));
  assert(load_state(plugin, blob.memory()));
  assert((saved_size(plugin) == std::array<uint32_t, 2>{800, 525}));

  const uint32_t tiny[2] = {20, 20};
  std::memcpy(blob.bytes().data() + gui_at, tiny, sizeof(tiny));
  assert(load_state(plugin, blob.memory()));
  assert((saved_size(plugin) == std::array<uint32_t, 2>{800, 525}));
  plugin->destroy(plugin);
}

// Legacy layouts, as declared by the reader. Each entry is {version, value_count,
// size_count}; versions 8 and 9 stored a single DPCM size rather than sixteen.
struct LegacyLayout { uint32_t version; size_t values; size_t sizes; };
constexpr LegacyLayout kLegacy[] = {
    {15, 103, 16}, {14, 91, 16}, {13, 79, 16}, {12, 77, 16}, {11, 75, 16}, {10, 73, 16}, {9, 72, 1}, {8, 62, 1},
};

void test_legacy_migration(const Library& library) {
  for (const LegacyLayout& layout : kLegacy) {
    const clap_plugin_t* plugin = library.create();
    const uint32_t count = params_of(plugin)->count(plugin);
    assert(layout.values <= count);

    // Every stored parameter gets a non-default value; anything the old version did
    // not store must come back as the current default.
    std::vector<double> stored(layout.values);
    Blob blob(layout.version, layout.values, layout.sizes);
    for (size_t i = 0; i < layout.values; ++i) {
      stored[i] = distinct_value(param_info(plugin, static_cast<clap_id>(i)));
      blob.value(i, stored[i]);
    }
    // Versions 8 and 9 migrate their single sample into slot one.
    std::vector<uint8_t> payload(24);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 3 + 1);
    blob.size(0, static_cast<uint32_t>(payload.size()));
    blob.append(payload);

    assert(load_state(plugin, blob.memory()));

    for (size_t i = 0; i < layout.values; ++i) {
      const double actual = param(plugin, static_cast<clap_id>(i));
      if (actual != stored[i]) {
        std::fprintf(stderr, "v%u: parameter %zu ('%s') migrated to %.17g, expected %.17g\n",
                     layout.version, i, param_info(plugin, static_cast<clap_id>(i)).name,
                     actual, stored[i]);
        assert(false);
      }
    }
    for (size_t i = layout.values; i < count; ++i) {
      const clap_param_info_t info = param_info(plugin, static_cast<clap_id>(i));
      if (param(plugin, static_cast<clap_id>(i)) != info.default_value) {
        std::fprintf(stderr, "v%u: parameter %zu ('%s') should have defaulted to %.17g, got %.17g\n",
                     layout.version, i, info.name, info.default_value,
                     param(plugin, static_cast<clap_id>(i)));
        assert(false);
      }
    }

    // A migrated project must re-save in the current format and survive a round trip.
    StateMemory resaved;
    assert(save_state(plugin, &resaved));
    const clap_plugin_t* fresh = library.create();
    assert(load_state(fresh, resaved));
    for (size_t i = 0; i < layout.values; ++i)
      assert(param(fresh, static_cast<clap_id>(i)) == stored[i]);
    fresh->destroy(fresh);
    plugin->destroy(plugin);
  }
}

// The migrated sample must land in slot one and keep its bytes.
void test_legacy_sample_lands_in_first_slot(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const uint32_t count = params_of(plugin)->count(plugin);

  std::vector<uint8_t> payload(40);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(0xa0 + i);
  Blob blob(9, 72, 1);
  for (size_t i = 0; i < 72; ++i) blob.value(i, param_info(plugin, static_cast<clap_id>(i)).default_value);
  blob.size(0, static_cast<uint32_t>(payload.size()));
  blob.append(payload);
  assert(load_state(plugin, blob.memory()));

  StateMemory resaved;
  assert(save_state(plugin, &resaved));
  // v16 header: magic, version, kParamCount doubles, sixteen sizes, then the editor size.
  const size_t sizes_at = 8 + count * sizeof(double);
  uint32_t sizes[16]{};
  std::memcpy(sizes, resaved.bytes.data() + sizes_at, sizeof(sizes));
  assert(sizes[0] == payload.size());
  for (size_t slot = 1; slot < 16; ++slot) assert(sizes[slot] == 0);
  const size_t payload_at = sizes_at + sizeof(sizes) + 2 * sizeof(uint32_t);
  assert(resaved.bytes.size() == payload_at + payload.size());
  assert(std::equal(payload.begin(), payload.end(), resaved.bytes.begin() + static_cast<long>(payload_at)));
  plugin->destroy(plugin);
}

void test_rejects_malformed_state(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const uint32_t count = params_of(plugin)->count(plugin);

  StateMemory saved;
  assert(save_state(plugin, &saved));

  // Empty stream.
  assert(!load_state(plugin, StateMemory{}));

  // Header only.
  assert(!load_state(plugin, slice(saved, 8)));

  // Truncated body, at several lengths.
  for (const size_t length : {size_t{1}, size_t{7}, size_t{12}, size_t{100}, saved.bytes.size() - 1}) {
    assert(!load_state(plugin, slice(saved, length)));
  }

  // Wrong magic.
  StateMemory bad_magic = saved;
  bad_magic.bytes[0] ^= 0xff;
  assert(!load_state(plugin, bad_magic));

  // Unknown versions, both older and newer than the supported range.
  for (const uint32_t version : {0U, 1U, 7U, 17U, 99U, 0xffffffffU}) {
    StateMemory wrong = saved;
    std::memcpy(wrong.bytes.data() + 4, &version, sizeof(version));
    assert(!load_state(plugin, wrong));
  }

  // A bank size beyond the 1 MiB per-slot limit must be refused rather than allocated.
  Blob oversized(16, count, 16);
  for (clap_id i = 0; i < count; ++i) oversized.value(i, param_info(plugin, i).default_value);
  oversized.size(3, 1024U * 1024U + 1U);
  assert(!load_state(plugin, oversized.memory()));

  // A declared bank size with no payload behind it must fail rather than read past the end.
  Blob missing_payload(16, count, 16);
  for (clap_id i = 0; i < count; ++i) missing_payload.value(i, param_info(plugin, i).default_value);
  missing_payload.size(0, 512);
  assert(!load_state(plugin, missing_payload.memory()));

  // Exactly at the limit is accepted.
  Blob at_limit(16, count, 16);
  for (clap_id i = 0; i < count; ++i) at_limit.value(i, param_info(plugin, i).default_value);
  at_limit.size(0, 1024U * 1024U);
  at_limit.append(std::vector<uint8_t>(1024U * 1024U, 0x5a));
  assert(load_state(plugin, at_limit.memory()));

  plugin->destroy(plugin);
}

// Out-of-range values in a hand-edited or corrupted blob must be clamped into the
// declared parameter range, never installed verbatim.
void test_state_values_are_clamped(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const uint32_t count = params_of(plugin)->count(plugin);

  for (const double poison : {-1.0e30, 1.0e30}) {
    Blob blob(16, count, 16);
    for (clap_id i = 0; i < count; ++i) blob.value(i, poison);
    assert(load_state(plugin, blob.memory()));
    for (clap_id i = 0; i < count; ++i) {
      const clap_param_info_t info = param_info(plugin, i);
      const double value = param(plugin, i);
      assert(value >= info.min_value && value <= info.max_value);
    }
  }
  plugin->destroy(plugin);
}

// A failed load must leave the entire current patch (including every bank) intact.
void test_failed_load_preserves_patch(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const uint32_t count = params_of(plugin)->count(plugin);
  Blob original(16, count, 16);
  for (clap_id i = 0; i < count; ++i) original.value(i, param_info(plugin, i).default_value);
  original.value(find_param(plugin, "Master"), -24.0);
  original.size(0, 8);
  original.size(15, 8);
  original.append(std::vector<uint8_t>(16, 0x5a));
  assert(load_state(plugin, original.memory()));
  StateMemory before;
  assert(save_state(plugin, &before));

  Blob replacement(16, count, 16);
  for (clap_id i = 0; i < count; ++i) replacement.value(i, param_info(plugin, i).default_value);
  replacement.value(find_param(plugin, "Waveform"), 17);
  replacement.size(0, 8);
  replacement.size(15, 8);
  replacement.append(std::vector<uint8_t>(16, 0xa5));
  const StateMemory full = replacement.memory();
  for (const size_t missing : {size_t{16}, size_t{8}, size_t{1}}) {
    assert(!load_state(plugin, slice(full, full.bytes.size() - missing)));
    StateMemory after;
    assert(save_state(plugin, &after));
    assert(after.bytes == before.bytes);
  }
  plugin->destroy(plugin);
}

// Corrupt floating-point values must be rejected before any state is installed,
// including when they arrive in one of the supported legacy formats.
void test_nonfinite_state_is_rejected(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const uint32_t count = params_of(plugin)->count(plugin);
  StateMemory before;
  assert(save_state(plugin, &before));
  std::vector<LegacyLayout> layouts(std::begin(kLegacy), std::end(kLegacy));
  layouts.push_back({16, count, 16});
  for (const auto& layout : layouts) {
    for (const double poison : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
      Blob blob(layout.version, layout.values, layout.sizes);
      for (size_t i = 0; i < layout.values; ++i)
        blob.value(i, param_info(plugin, static_cast<clap_id>(i)).default_value);
      blob.value(find_param(plugin, "Master"), poison);
      assert(!load_state(plugin, blob.memory()));
      StateMemory after;
      assert(save_state(plugin, &after));
      assert(after.bytes == before.bytes);
    }
  }
  plugin->destroy(plugin);
}

// A stream that only returns a few bytes per call must still be read completely.
int64_t dribble_read(const clap_istream_t* stream, void* data, uint64_t size) {
  auto* memory = static_cast<StateMemory*>(stream->ctx);
  const uint64_t available = memory->bytes.size() - memory->read;
  const uint64_t amount = std::min<uint64_t>(std::min<uint64_t>(size, available), 3);
  std::memcpy(data, memory->bytes.data() + memory->read, amount);
  memory->read += amount;
  return static_cast<int64_t>(amount);
}
int64_t dribble_write(const clap_ostream_t* stream, const void* data, uint64_t size) {
  auto* memory = static_cast<StateMemory*>(stream->ctx);
  const uint64_t amount = std::min<uint64_t>(size, 5);
  const auto* bytes = static_cast<const uint8_t*>(data);
  memory->bytes.insert(memory->bytes.end(), bytes, bytes + amount);
  return static_cast<int64_t>(amount);
}

void test_partial_stream_transfers(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const auto* state = static_cast<const clap_plugin_state_t*>(
      plugin->get_extension(plugin, CLAP_EXT_STATE));

  const clap_id waveform = find_param(plugin, "Waveform");
  Events change;
  change.push(param_event(waveform, 33));
  flush(plugin, change);

  StateMemory memory;
  clap_ostream_t out{&memory, dribble_write};
  assert(state->save(plugin, &out));

  StateMemory reference;
  assert(save_state(plugin, &reference));
  assert(memory.bytes == reference.bytes);

  Events reset;
  reset.push(param_event(waveform, 0));
  flush(plugin, reset);

  memory.read = 0;
  clap_istream_t in{&memory, dribble_read};
  assert(state->load(plugin, &in));
  assert(param(plugin, waveform) == 33);
  plugin->destroy(plugin);
}

// Restoring state while notes are sounding must not strand a voice.
void test_state_load_during_playback(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, 48000.0, 512);
    StateMemory saved;
    assert(save_state(plugin, &saved));

    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    runner.run(&on);
    assert(runner.run().peak > 1.0e-3f);

    assert(load_state(plugin, saved));
    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, -1, -1, -1, 0.0));
    runner.run(&off);
    runner.settle(40);
    assert(runner.run().peak == 0.0f);
  }
  plugin->destroy(plugin);
}

// Selecting a preset must produce the same patch no matter which preset preceded it.
// Presets used to apply only their own edits, so browsing through them accumulated the
// earlier selections' console noise, arpeggios, and effects on top of the new sound.
void test_presets_do_not_accumulate(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  const clap_id preset = find_param(plugin, "Preset");
  const clap_param_info_t preset_info = param_info(plugin, preset);
  const uint32_t count = params_of(plugin)->count(plugin);

  // The reference patch for each preset is the one a freshly created instance produces.
  std::vector<std::vector<double>> reference;
  for (int choice = 0; choice <= static_cast<int>(preset_info.max_value); ++choice) {
    const clap_plugin_t* fresh = library.create();
    set_param(fresh, preset, choice);
    std::vector<double> values(count);
    for (clap_id i = 0; i < count; ++i) values[i] = param(fresh, i);
    reference.push_back(std::move(values));
    fresh->destroy(fresh);
  }

  // Walking every preset in both directions through one instance must reproduce them.
  for (int pass = 0; pass < 2; ++pass) {
    for (int step = 0; step <= static_cast<int>(preset_info.max_value); ++step) {
      const int choice = pass == 0 ? step : static_cast<int>(preset_info.max_value) - step;
      set_param(plugin, preset, choice);
      for (clap_id i = 0; i < count; ++i) {
        if (param(plugin, i) == reference[static_cast<size_t>(choice)][i]) continue;
        std::fprintf(stderr,
                     "preset %d left '%s' at %.17g, a freshly loaded instance gives %.17g\n",
                     choice, param_info(plugin, i).name, param(plugin, i),
                     reference[static_cast<size_t>(choice)][i]);
        assert(false);
      }
    }
  }

  // Master is the user's output level rather than part of a recipe, so it must survive.
  const clap_id master = find_param(plugin, "Master");
  set_param(plugin, master, -24.0);
  set_param(plugin, preset, 8);
  assert(param(plugin, master) == -24.0);
  plugin->destroy(plugin);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const Library library(argv[1]);

  test_v16_round_trip(library);
  test_v16_bank_payload_round_trip(library);
  test_editor_size_round_trip(library);
  test_legacy_migration(library);
  test_legacy_sample_lands_in_first_slot(library);
  test_rejects_malformed_state(library);
  test_state_values_are_clamped(library);
  test_failed_load_preserves_patch(library);
  test_nonfinite_state_is_rejected(library);
  test_partial_stream_transfers(library);
  test_state_load_during_playback(library);
  test_presets_do_not_accumulate(library);
  std::printf("state_tests: all checks passed\n");
}
