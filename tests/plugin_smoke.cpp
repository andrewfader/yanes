#include <clap/clap.h>
#include "clap_harness.hpp"
#include <cassert>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

namespace {
bool g_creating_plugin = false;
// The editor runs on the host's main thread, which means it can only exist where the host
// offers a timer to drive it from. This stub is the smallest host that qualifies.
clap_id g_timer_id = CLAP_INVALID_ID;
uint32_t g_timer_registered = 0, g_timer_unregistered = 0;
bool register_timer(const clap_host_t*, uint32_t period_ms, clap_id* timer_id) {
  assert(period_ms > 0 && timer_id);
  *timer_id = g_timer_id = 7;
  ++g_timer_registered;
  return true;
}
bool unregister_timer(const clap_host_t*, clap_id timer_id) {
  assert(timer_id == g_timer_id);
  ++g_timer_unregistered;
  return true;
}
const clap_host_timer_support_t g_timer_support{register_timer, unregister_timer};
const void* host_extension(const clap_host_t*, const char* id) {
  assert(!g_creating_plugin && "Host get_extension must not be called during create_plugin");
  if (id && std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &g_timer_support;
  return nullptr;
}
// A host with no timer at all, used to check that the editor declines rather than falling
// back to a thread of its own.
const void* bare_host_extension(const clap_host_t*, const char*) { return nullptr; }
void request_restart(const clap_host_t*) {
  assert(!g_creating_plugin && "Host request_restart must not be called during create_plugin");
}
void request_process(const clap_host_t*) {
  assert(!g_creating_plugin && "Host request_process must not be called during create_plugin");
}
void request_callback(const clap_host_t*) {
  assert(!g_creating_plugin && "Host request_callback must not be called during create_plugin before plugin->init()");
}
struct EventList {
  std::array<const clap_event_header_t*, 2> events{};
  uint32_t count{};
};
struct StateMemory { std::vector<uint8_t> bytes; size_t read{}; };
int64_t state_write(const clap_ostream_t* stream, const void* data, uint64_t size) {
  auto* memory=static_cast<StateMemory*>(stream->ctx);const auto* bytes=static_cast<const uint8_t*>(data);
  memory->bytes.insert(memory->bytes.end(),bytes,bytes+size);return static_cast<int64_t>(size);
}
int64_t state_read(const clap_istream_t* stream, void* data, uint64_t size) {
  auto* memory=static_cast<StateMemory*>(stream->ctx);const uint64_t available=memory->bytes.size()-memory->read;
  const uint64_t amount=std::min(size,available);std::memcpy(data,memory->bytes.data()+memory->read,amount);memory->read+=amount;return static_cast<int64_t>(amount);
}
uint32_t event_count(const clap_input_events_t* list) {
  return static_cast<const EventList*>(list->ctx)->count;
}
const clap_event_header_t* event_get(const clap_input_events_t* list, uint32_t index) {
  const auto* events = static_cast<const EventList*>(list->ctx);
  return index < events->count ? events->events[index] : nullptr;
}
}

int main(int argc, char** argv) {
  assert(argc == 2);
  harness::LibraryHandle library = harness::open_library(argv[1]);
  assert(library);
  const auto* entry = static_cast<const clap_plugin_entry_t*>(harness::find_symbol(library, "clap_entry"));
  assert(entry && clap_version_is_compatible(entry->clap_version));
  assert(entry->init(argv[1]));
  const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  assert(factory && factory->get_plugin_count(factory) == 1);
  const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
  assert(descriptor && std::strcmp(descriptor->id, "org.yanes.native") == 0);

  const clap_host_t host{CLAP_VERSION_INIT, nullptr, "YANES test host", "YANES", "", "1",
                         host_extension, request_restart, request_process, request_callback};
  g_creating_plugin = true;
  const clap_plugin_t* plugin = factory->create_plugin(factory, &host, descriptor->id);
  g_creating_plugin = false;
  assert(plugin && plugin->init(plugin));
  const auto* audio = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
  const auto* notes = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
  const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
  const auto* latency = static_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
  const auto* tail = static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
  assert(audio && audio->count(plugin, false) == 1 && audio->count(plugin, true) == 0);
  assert(notes && notes->count(plugin, true) == 1);
  assert(params && params->count(plugin) == 79);
  assert(latency && latency->get(plugin) == 0);
  assert(tail);
#ifdef __linux__
  assert(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false));
  assert(!gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, true));
  {
    // Same plug-in, a host that offers no timer: the editor has nothing to run on and has
    // to say so, leaving the host to use its own parameter panel.
    const clap_host_t bare{CLAP_VERSION_INIT, nullptr, "YANES bare host", "YANES", "", "1",
                           bare_host_extension, request_restart, request_process, request_callback};
    g_creating_plugin = true;
    const clap_plugin_t* timerless = factory->create_plugin(factory, &bare, descriptor->id);
    g_creating_plugin = false;
    assert(timerless && timerless->init(timerless));
    const auto* bare_gui = static_cast<const clap_plugin_gui_t*>(
        timerless->get_extension(timerless, CLAP_EXT_GUI));
    assert(bare_gui && !bare_gui->is_api_supported(timerless, CLAP_WINDOW_API_X11, false));
    assert(!bare_gui->create(timerless, CLAP_WINDOW_API_X11, false));
    timerless->destroy(timerless);
  }
#elif defined(_WIN32)
  assert(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, false));
  assert(!gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, true));
#elif defined(__APPLE__)
  assert(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false));
  assert(!gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, true));
#endif
  uint32_t gui_width=0,gui_height=0;assert(gui->get_size(plugin,&gui_width,&gui_height));
  assert(gui_width==1600&&gui_height==1050);assert(gui->can_resize(plugin));
  clap_gui_resize_hints_t hints{};assert(gui->get_resize_hints(plugin,&hints));assert(hints.can_resize_horizontally&&hints.can_resize_vertically&&!hints.preserve_aspect_ratio);
  uint32_t adjusted_width=400,adjusted_height=300;assert(gui->adjust_size(plugin,&adjusted_width,&adjusted_height));assert(adjusted_width==960&&adjusted_height==630);
  assert(gui->set_size(plugin,1920,900));assert(gui->get_size(plugin,&gui_width,&gui_height));assert(gui_width==1920&&gui_height==900);
  assert(!gui->set_size(plugin,800,600));assert(gui->set_size(plugin,1600,1050));
  for(uint32_t i=0;i<params->count(plugin);++i){clap_param_info_t info{};assert(params->get_info(plugin,i,&info));assert(info.id==i);assert(info.name[0]&&info.module[0]);char text[128]{};assert(params->value_to_text(plugin,i,info.default_value,text,sizeof(text)));assert(text[0]);}
  const auto* voices=static_cast<const clap_plugin_voice_info_t*>(plugin->get_extension(plugin,CLAP_EXT_VOICE_INFO));
  clap_voice_info_t voice_info{};assert(voices&&voices->get(plugin,&voice_info)&&voice_info.voice_count==16&&voice_info.voice_capacity==16);
#ifdef __linux__
  if(std::getenv("YANES_TEST_GUI")){
    const auto* timers=static_cast<const clap_plugin_timer_support_t*>(plugin->get_extension(plugin,CLAP_EXT_TIMER_SUPPORT));
    assert(timers&&timers->on_timer);
    const uint32_t registered_before=g_timer_registered;
    assert(gui->create(plugin,CLAP_WINDOW_API_X11,false));
    // Opening the editor has to take a timer from the host; that timer is the only thing
    // driving it now, so a missing registration means a window that never redraws.
    assert(g_timer_registered==registered_before+1);
    gui->suggest_title(plugin,"YANES automated GUI test");
    assert(gui->show(plugin));
    // Stand in for the host's main loop: every tick pumps X and repaints as needed.
    const auto pump=[&](int ticks){for(int i=0;i<ticks;++i){timers->on_timer(plugin,g_timer_id);std::this_thread::sleep_for(std::chrono::milliseconds(16));}};
    pump(4);
    assert(gui->set_size(plugin,1200,700));pump(4);
    assert(gui->set_size(plugin,1900,1000));pump(4);
    if(const char*hold=std::getenv("YANES_TEST_GUI_HOLD_MS"))pump(std::max(0,std::atoi(hold))/16);
    assert(gui->hide(plugin));
    const uint32_t unregistered_before=g_timer_unregistered;
    gui->destroy(plugin);
    assert(g_timer_unregistered==unregistered_before+1);
  }
#endif
  const auto* state=static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin,CLAP_EXT_STATE));
  assert(state);
  assert(plugin->activate(plugin, 48000.0, 1, 512));
  assert(plugin->start_processing(plugin));

  clap_event_param_value_t release_tail{};
  release_tail.header = {sizeof(release_tail), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
  release_tail.param_id = 5; release_tail.note_id = -1; release_tail.port_index = -1;
  release_tail.channel = -1; release_tail.key = -1; release_tail.value = 100.0;
  EventList tail_events{{&release_tail.header, nullptr}, 1};
  clap_input_events_t tail_input{&tail_events, event_count, event_get};
  params->flush(plugin, &tail_input, nullptr);
  assert(tail->get(plugin) == 4800);

  std::array<float, 512> left{}, right{};
  std::array<float*, 2> channels{left.data(), right.data()};
  clap_audio_buffer_t output{channels.data(), nullptr, 2, 0, 0};
  for (int waveform = 0; waveform <= 57; ++waveform) {
    clap_event_param_value_t param{};
    param.header = {sizeof(param), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    param.param_id = 0;
    param.note_id = -1; param.port_index = -1; param.channel = -1; param.key = -1;
    param.value = waveform;
    clap_event_note_t note{};
    note.header = {sizeof(note), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
    note.note_id = waveform; note.port_index = 0; note.channel = waveform % 10;
    note.key = 60; note.velocity = 1.0;
    EventList events{{&param.header, &note.header}, 2};
    clap_input_events_t input_events{&events, event_count, event_get};
    clap_process_t process{};
    process.frames_count = 512;
    process.in_events = &input_events;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    assert(plugin->process(plugin, &process) == CLAP_PROCESS_CONTINUE);
    bool finite = true;
    float peak = 0.0f;
    for (float sample : left) { finite = finite && std::isfinite(sample); peak = std::max(peak, std::abs(sample)); }
    assert(finite);
    if (waveform == 17 || (waveform >= 27 && waveform <= 30) || waveform >= 51) {
      if (!(peak > 1.0e-4f)) std::fprintf(stderr, "inaudible synthesis waveform %d (peak %.8f)\n", waveform,peak);
      assert(peak > 1.0e-4f);
    }
    plugin->reset(plugin);
  }
  for(int key=36;key<48;++key){
    clap_event_param_value_t kit{};kit.header={sizeof(kit),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};kit.param_id=0;kit.note_id=-1;kit.port_index=-1;kit.channel=-1;kit.key=-1;kit.value=57;
    clap_event_note_t hit{};hit.header={sizeof(hit),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_ON,0};hit.note_id=1000+key;hit.port_index=0;hit.channel=9;hit.key=static_cast<int16_t>(key);hit.velocity=1.0;
    EventList events{{&kit.header,&hit.header},2};clap_input_events_t input{&events,event_count,event_get};clap_process_t process{};process.frames_count=512;process.in_events=&input;process.audio_outputs=&output;process.audio_outputs_count=1;assert(plugin->process(plugin,&process)==CLAP_PROCESS_CONTINUE);
    float peak=0.0f;for(float sample:left)peak=std::max(peak,std::abs(sample));if(!(peak>1.0e-4f))std::fprintf(stderr,"inaudible retro drum key %d (peak %.8f)\n",key,peak);assert(peak>1.0e-4f);plugin->reset(plugin);
  }
  clap_event_note_t held{};held.header={sizeof(held),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_ON,0};held.note_id=900;held.port_index=0;held.channel=0;held.key=60;held.velocity=1.0;
  clap_event_param_value_t initial{};initial.header={sizeof(initial),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};initial.param_id=0;initial.note_id=-1;initial.port_index=-1;initial.channel=-1;initial.key=-1;initial.value=0;
  EventList held_events{{&initial.header,&held.header},2};clap_input_events_t held_input{&held_events,event_count,event_get};clap_process_t held_process{};held_process.frames_count=512;held_process.in_events=&held_input;held_process.audio_outputs=&output;held_process.audio_outputs_count=1;assert(plugin->process(plugin,&held_process)==CLAP_PROCESS_CONTINUE);
  for(const int waveform:{27,28,30}){clap_event_param_value_t transition=initial;transition.value=waveform;EventList transition_events{{&transition.header,nullptr},1};clap_input_events_t transition_input{&transition_events,event_count,event_get};held_process.in_events=&transition_input;assert(plugin->process(plugin,&held_process)==CLAP_PROCESS_CONTINUE);float peak=0.0f;for(float sample:left)peak=std::max(peak,std::abs(sample));if(!(peak>1.0e-4f))std::fprintf(stderr,"inaudible held-note FM transition %d (peak %.8f)\n",waveform,peak);assert(peak>1.0e-4f);}
  plugin->reset(plugin);
  plugin->stop_processing(plugin);
  StateMemory memory;clap_ostream_t output_state{&memory,state_write};assert(state->save(plugin,&output_state));
  clap_event_param_value_t change{};change.header={sizeof(change),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};change.param_id=0;change.value=40;
  EventList state_events{{&change.header,nullptr},1};clap_input_events_t state_input{&state_events,event_count,event_get};params->flush(plugin,&state_input,nullptr);
  double changed=0;assert(params->get_value(plugin,0,&changed)&&changed==40);
  clap_istream_t input_state{&memory,state_read};assert(state->load(plugin,&input_state));double restored=0;assert(params->get_value(plugin,0,&restored)&&restored==30);
  StateMemory truncated=memory;truncated.read=0;truncated.bytes.resize(12);clap_istream_t bad_state{&truncated,state_read};assert(!state->load(plugin,&bad_state));
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  entry->deinit();
  harness::close_library(library);
}
