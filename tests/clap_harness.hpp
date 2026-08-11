// Shared scaffolding for the CLAP-level tests. Each test binary dlopens the built
// plug-in and drives it exclusively through the public CLAP interfaces, so the tests
// exercise the same entry points a host uses.
#pragma once

#include <clap/clap.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

#ifdef _WIN32
using LibraryHandle = HMODULE;
inline LibraryHandle open_library(const char* path) { return LoadLibraryA(path); }
inline void* find_symbol(LibraryHandle handle, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(handle, name));
}
inline void close_library(LibraryHandle handle) { FreeLibrary(handle); }
#else
using LibraryHandle = void*;
inline LibraryHandle open_library(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
inline void* find_symbol(LibraryHandle handle, const char* name) { return dlsym(handle, name); }
inline void close_library(LibraryHandle handle) { dlclose(handle); }
#endif

inline const void* host_extension(const clap_host_t*, const char*) { return nullptr; }
inline void host_noop(const clap_host_t*) {}

inline const clap_host_t kHost{CLAP_VERSION_INIT, nullptr, "YANES test host", "YANES", "", "1",
                               host_extension, host_noop, host_noop, host_noop};

// Owns the dlopen handle and the CLAP entry point for one test process.
struct Library {
  LibraryHandle handle{};
  const clap_plugin_entry_t* entry{};
  const clap_plugin_factory_t* factory{};
  const clap_plugin_descriptor_t* descriptor{};

  explicit Library(const char* path) {
    handle = open_library(path);
    assert(handle && "failed to load the plug-in");
    entry = static_cast<const clap_plugin_entry_t*>(find_symbol(handle, "clap_entry"));
    assert(entry && clap_version_is_compatible(entry->clap_version));
    assert(entry->init(path));
    factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    assert(factory && factory->get_plugin_count(factory) == 1);
    descriptor = factory->get_plugin_descriptor(factory, 0);
    assert(descriptor);
  }
  ~Library() { entry->deinit(); close_library(handle); }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  const clap_plugin_t* create() const {
    const clap_plugin_t* plugin = factory->create_plugin(factory, &kHost, descriptor->id);
    assert(plugin && plugin->init(plugin));
    return plugin;
  }
};

// Growable input event queue. Events are stored by value so callers do not have to
// keep the originals alive for the duration of a process call.
class Events {
 public:
  void clear() { storage_.clear(); offsets_.clear(); }

  template <typename Event>
  void push(const Event& event) {
    static_assert(alignof(Event) <= 8, "event alignment exceeds the storage guarantee");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&event);
    offsets_.push_back(storage_.size() * sizeof(uint64_t));
    const size_t words = (sizeof(Event) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    const size_t at = storage_.size();
    storage_.resize(at + words, 0);
    std::memcpy(storage_.data() + at, bytes, sizeof(Event));
  }

  clap_input_events_t list() { return clap_input_events_t{this, size, get}; }

 private:
  static uint32_t size(const clap_input_events_t* list) {
    return static_cast<uint32_t>(static_cast<const Events*>(list->ctx)->offsets_.size());
  }
  static const clap_event_header_t* get(const clap_input_events_t* list, uint32_t index) {
    const auto* self = static_cast<const Events*>(list->ctx);
    if (index >= self->offsets_.size()) return nullptr;
    const auto* base = reinterpret_cast<const uint8_t*>(self->storage_.data());
    return reinterpret_cast<const clap_event_header_t*>(base + self->offsets_[index]);
  }
  // uint64_t storage keeps every event 8-byte aligned, which the event structs require.
  std::vector<uint64_t> storage_;
  std::vector<size_t> offsets_;
};

inline clap_event_note_t note_event(uint16_t type, int16_t channel, int16_t key,
                                    int32_t note_id, double velocity) {
  clap_event_note_t event{};
  event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
  event.note_id = note_id;
  event.port_index = 0;
  event.channel = channel;
  event.key = key;
  event.velocity = velocity;
  return event;
}

inline clap_event_param_value_t param_event(clap_id id, double value) {
  clap_event_param_value_t event{};
  event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
  event.param_id = id;
  event.cookie = nullptr;
  event.note_id = -1;
  event.port_index = -1;
  event.channel = -1;
  event.key = -1;
  event.value = value;
  return event;
}

inline clap_event_midi_t midi_event(uint8_t status, uint8_t data1, uint8_t data2) {
  clap_event_midi_t event{};
  event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
  event.port_index = 0;
  event.data[0] = status;
  event.data[1] = data1;
  event.data[2] = data2;
  return event;
}

inline clap_event_note_expression_t expression_event(clap_note_expression id, int16_t channel,
                                                     int16_t key, int32_t note_id, double value) {
  clap_event_note_expression_t event{};
  event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
  event.expression_id = id;
  event.note_id = note_id;
  event.port_index = 0;
  event.channel = channel;
  event.key = key;
  event.value = value;
  return event;
}

// Summary of one rendered block. Peak is the loudest absolute sample across both
// channels; rms lets a test distinguish "quiet" from "genuinely silent".
struct Block {
  float peak{};
  double rms{};
  bool finite{true};
};

// Drives a plug-in instance at a fixed sample rate and block size.
class Runner {
 public:
  Runner(const clap_plugin_t* plugin, double sample_rate, uint32_t frames)
      : plugin_(plugin), frames_(frames), left_(frames), right_(frames) {
    assert(plugin_->activate(plugin_, sample_rate, 1, frames));
    assert(plugin_->start_processing(plugin_));
  }
  ~Runner() { plugin_->stop_processing(plugin_); plugin_->deactivate(plugin_); }
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  Block run(Events* events = nullptr) {
    std::fill(left_.begin(), left_.end(), 0.0f);
    std::fill(right_.begin(), right_.end(), 0.0f);
    channels_[0] = left_.data();
    channels_[1] = right_.data();
    clap_audio_buffer_t output{channels_, nullptr, 2, 0, 0};
    clap_input_events_t input = events ? events->list() : empty_.list();
    clap_process_t process{};
    process.frames_count = frames_;
    process.in_events = &input;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    assert(plugin_->process(plugin_, &process) == CLAP_PROCESS_CONTINUE);

    Block block;
    double sum = 0.0;
    for (uint32_t i = 0; i < frames_; ++i) {
      for (float sample : {left_[i], right_[i]}) {
        block.finite = block.finite && std::isfinite(sample);
        block.peak = std::max(block.peak, std::abs(sample));
        sum += static_cast<double>(sample) * sample;
      }
    }
    block.rms = std::sqrt(sum / (2.0 * frames_));
    return block;
  }

  // Renders `blocks` blocks with no events and returns the loudest one.
  Block settle(int blocks) {
    Block worst;
    for (int i = 0; i < blocks; ++i) {
      const Block block = run();
      worst.finite = worst.finite && block.finite;
      worst.peak = std::max(worst.peak, block.peak);
      worst.rms = std::max(worst.rms, block.rms);
    }
    return worst;
  }

  void set(clap_id id, double value) {
    Events events;
    events.push(param_event(id, value));
    run(&events);
  }

  const std::vector<float>& left() const { return left_; }
  const std::vector<float>& right() const { return right_; }
  uint32_t frames() const { return frames_; }

 private:
  const clap_plugin_t* plugin_;
  uint32_t frames_;
  std::vector<float> left_, right_;
  float* channels_[2]{};
  Events empty_;
};

// In-memory CLAP state streams.
struct StateMemory {
  std::vector<uint8_t> bytes;
  size_t read{};
};

inline int64_t state_write(const clap_ostream_t* stream, const void* data, uint64_t size) {
  auto* memory = static_cast<StateMemory*>(stream->ctx);
  const auto* bytes = static_cast<const uint8_t*>(data);
  memory->bytes.insert(memory->bytes.end(), bytes, bytes + size);
  return static_cast<int64_t>(size);
}

inline int64_t state_read(const clap_istream_t* stream, void* data, uint64_t size) {
  auto* memory = static_cast<StateMemory*>(stream->ctx);
  const uint64_t available = memory->bytes.size() - memory->read;
  const uint64_t amount = std::min(size, available);
  std::memcpy(data, memory->bytes.data() + memory->read, amount);
  memory->read += amount;
  return static_cast<int64_t>(amount);
}

inline bool save_state(const clap_plugin_t* plugin, StateMemory* memory) {
  const auto* state = static_cast<const clap_plugin_state_t*>(
      plugin->get_extension(plugin, CLAP_EXT_STATE));
  assert(state);
  clap_ostream_t stream{memory, state_write};
  return state->save(plugin, &stream);
}

inline bool load_state(const clap_plugin_t* plugin, StateMemory memory) {
  const auto* state = static_cast<const clap_plugin_state_t*>(
      plugin->get_extension(plugin, CLAP_EXT_STATE));
  assert(state);
  memory.read = 0;
  clap_istream_t stream{&memory, state_read};
  return state->load(plugin, &stream);
}

inline const clap_plugin_params_t* params_of(const clap_plugin_t* plugin) {
  const auto* params = static_cast<const clap_plugin_params_t*>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  assert(params);
  return params;
}

// Resolves a parameter by its display name so the tests do not depend on the
// plug-in's private enum order.
inline clap_id find_param(const clap_plugin_t* plugin, const char* name) {
  const auto* params = params_of(plugin);
  for (uint32_t i = 0; i < params->count(plugin); ++i) {
    clap_param_info_t info{};
    assert(params->get_info(plugin, i, &info));
    if (std::strcmp(info.name, name) == 0) return info.id;
  }
  std::fprintf(stderr, "no parameter named '%s'\n", name);
  assert(false && "unknown parameter name");
  return 0;
}

inline clap_param_info_t param_info(const clap_plugin_t* plugin, clap_id id) {
  clap_param_info_t info{};
  assert(params_of(plugin)->get_info(plugin, id, &info));
  return info;
}

inline double param(const clap_plugin_t* plugin, clap_id id) {
  double value = 0.0;
  assert(params_of(plugin)->get_value(plugin, id, &value));
  return value;
}

// Delivers events outside the audio thread, the way a host applies automation while
// the plug-in is idle.
inline void flush(const clap_plugin_t* plugin, Events& events) {
  clap_input_events_t list = events.list();
  params_of(plugin)->flush(plugin, &list, nullptr);
}

inline void set_param(const clap_plugin_t* plugin, clap_id id, double value) {
  Events events;
  events.push(param_event(id, value));
  flush(plugin, events);
}

inline StateMemory slice(const StateMemory& memory, size_t length) {
  StateMemory result;
  result.bytes.assign(memory.bytes.begin(), memory.bytes.begin() + static_cast<long>(length));
  return result;
}

}  // namespace harness
