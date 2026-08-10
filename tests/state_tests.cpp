// CLAP state coverage: the current v13 round trip including DPCM bank payloads, the
// legacy v8..v12 migration paths, and rejection of malformed blobs.
//
// The legacy layouts are reconstructed here from the reader's own struct definitions.
// That pins the on-disk contract so a future parameter insertion cannot silently shift
// an old project's values: adding a parameter anywhere other than the end of the enum
// breaks these tests.
#include "clap_harness.hpp"

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

void test_v13_round_trip(const Library& library) {
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
void test_v13_bank_payload_round_trip(const Library& library) {
  const clap_plugin_t* plugin = library.create();

  // Install banks through a v13 blob, then save and compare the produced bytes.
  const uint32_t count = params_of(plugin)->count(plugin);
  Blob blob(13, count, 16);
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

// Legacy layouts, as declared by the reader. Each entry is {version, value_count,
// size_count}; versions 8 and 9 stored a single DPCM size rather than sixteen.
struct LegacyLayout { uint32_t version; size_t values; size_t sizes; };
constexpr LegacyLayout kLegacy[] = {
    {12, 77, 16}, {11, 75, 16}, {10, 73, 16}, {9, 72, 1}, {8, 62, 1},
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
  // v13 header: magic, version, kParamCount doubles, then sixteen sizes.
  const size_t sizes_at = 8 + count * sizeof(double);
  uint32_t sizes[16]{};
  std::memcpy(sizes, resaved.bytes.data() + sizes_at, sizeof(sizes));
  assert(sizes[0] == payload.size());
  for (size_t slot = 1; slot < 16; ++slot) assert(sizes[slot] == 0);
  const size_t payload_at = sizes_at + sizeof(sizes);
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
  for (const uint32_t version : {0U, 1U, 7U, 14U, 99U, 0xffffffffU}) {
    StateMemory wrong = saved;
    std::memcpy(wrong.bytes.data() + 4, &version, sizeof(version));
    assert(!load_state(plugin, wrong));
  }

  // A bank size beyond the 1 MiB per-slot limit must be refused rather than allocated.
  Blob oversized(13, count, 16);
  for (clap_id i = 0; i < count; ++i) oversized.value(i, param_info(plugin, i).default_value);
  oversized.size(3, 1024U * 1024U + 1U);
  assert(!load_state(plugin, oversized.memory()));

  // A declared bank size with no payload behind it must fail rather than read past the end.
  Blob missing_payload(13, count, 16);
  for (clap_id i = 0; i < count; ++i) missing_payload.value(i, param_info(plugin, i).default_value);
  missing_payload.size(0, 512);
  assert(!load_state(plugin, missing_payload.memory()));

  // Exactly at the limit is accepted.
  Blob at_limit(13, count, 16);
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
    Blob blob(13, count, 16);
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

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const Library library(argv[1]);

  test_v13_round_trip(library);
  test_v13_bank_payload_round_trip(library);
  test_legacy_migration(library);
  test_legacy_sample_lands_in_first_slot(library);
  test_rejects_malformed_state(library);
  test_state_values_are_clamped(library);
  test_partial_stream_transfers(library);
  test_state_load_during_playback(library);
  std::printf("state_tests: all checks passed\n");
}
