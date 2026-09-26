// Headless relevance/dialog checks; opt-in --display-review DIR exercises native X11.
#include "../src/yanes.cpp"
#include <cassert>
#include <chrono>
#include <filesystem>

namespace {
int timers = 0;
bool register_timer(const clap_host_t*, uint32_t, clap_id* id) { ++timers; *id = 1; return true; }
bool unregister_timer(const clap_host_t*, clap_id id) { assert(id == 1); --timers; return true; }
const clap_host_timer_support_t timer_host{register_timer, unregister_timer};

void snapshot(Plugin& p, const std::string& path) {
  XSync(p.display, False);
  XImage* image = XGetImage(p.display, p.gui_back, 0, 0, p.gui_width, p.gui_height, AllPlanes, ZPixmap);
  assert(image);
  FILE* out = std::fopen(path.c_str(), "wb"); assert(out);
  std::fprintf(out, "P6\n%u %u\n255\n", p.gui_width, p.gui_height);
  for (uint32_t y = 0; y < p.gui_height; ++y)
    for (uint32_t x = 0; x < p.gui_width; ++x) {
      const unsigned long pixel = XGetPixel(image, static_cast<int>(x), static_cast<int>(y));
      const unsigned char rgb[]{static_cast<unsigned char>(pixel >> 16), static_cast<unsigned char>(pixel >> 8), static_cast<unsigned char>(pixel)};
      assert(std::fwrite(rgb, 1, 3, out) == 3);
    }
  assert(std::fclose(out) == 0);
  XDestroyImage(image);
}

// Explicit opt-in because normal CTest runs must work without a display. Exercise the
// production window, fonts, backing pixmap, event pump, artwork, and editor ownership.
void display_review(Plugin& p, const std::string& directory) {
  using namespace yanes::ui;
  std::filesystem::create_directories(directory);
  p.api.plugin_data = &p;
  p.host_timers = &timer_host;
  double now = now_ms();
  for (int cycle = 0; cycle < 3; ++cycle) {
    std::fprintf(stderr, "opening cycle %d\n", cycle);
    assert(gui_create(&p.api, CLAP_WINDOW_API_X11, false));
    XSynchronize(p.display, True);
    assert(timers == 1);
    assert(gui_show(&p.api));
    auto& editor = gui_editor(&p);
    for (int percent : {50, 75, 100, 125}) {
      assert(gui_set_size(&p.api, width * percent / 100, height * percent / 100));
      XSync(p.display, False); gui_pump(&p);
      for (int page = 0; page < editor.page_count(); ++page) {
        editor.set_page(page);
        for (int tick = 0; tick < 40; ++tick) editor.tick(now += 16);
        gui_paint(&p);
        if (cycle == 0) snapshot(p, directory + "/page" + std::to_string(page + 1) + "-" + std::to_string(percent) + ".ppm");
      }
      const auto event = [&](int type, int button, int x, int y) {
        XEvent e{};
        e.xbutton.type = type; e.xbutton.display = p.display; e.xbutton.window = p.window;
        e.xbutton.button = static_cast<unsigned>(button);
        e.xbutton.x = scale_x(x, static_cast<int>(p.gui_width));
        e.xbutton.y = scale_y(y, static_cast<int>(p.gui_height));
        XSendEvent(p.display, p.window, False, type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &e);
        XSync(p.display, False); gui_pump(&p);
      };
      // A real X11 click at every scale must hit the intended sample column.
      bool sample_clicked = false;
      for (const auto& control : editor.layout().controls) if (editor.page_spec(6).sections[static_cast<size_t>(control.section)].controls[static_cast<size_t>(control.control)].param == kWaveSample1) {
        sample_clicked = true;
        p.params[kWaveSample1].store(0);
        const auto g = lane_geometry(control.rect);
        const int x = g.bars.x + g.step_width(32) / 2;
        event(ButtonPress, 1, x, g.bars.y + 1);
        event(ButtonRelease, 1, x, g.bars.y + 1);
        assert(p.params[kWaveSample1].load() == 15);
      }
      assert(sample_clicked);
      event(ButtonPress, 1, preset_rect.x + 110, preset_rect.y + 20);
      event(ButtonRelease, 1, preset_rect.x + 110, preset_rect.y + 20);
      for (int tick = 0; tick < 40; ++tick) editor.tick(now += 16);
      gui_paint(&p);
      if (cycle == 0) snapshot(p, directory + "/presets-" + std::to_string(percent) + ".ppm");
      event(ButtonPress, 1, 5, 5); event(ButtonRelease, 1, 5, 5);
      p.gui_out_r.store(p.gui_out_w.load());
    }
    std::fprintf(stderr, "closing cycle %d\n", cycle);
    assert(gui_hide(&p.api));
    gui_destroy(&p.api);
    assert(timers == 0 && !p.display && !p.window && !p.gc && !p.gui_back && !p.gui_back_xft);
    for (auto* font : p.gui_fonts) assert(!font);
    gui_destroy(&p.api);
  }
}
} // namespace

int main(int argc, char** argv) {
  Plugin p;
  for (clap_id i = 0; i < kParamCount; ++i) p.params[i].store(kSpecs[i].def);
  const auto source = [&](int wave) { p.params[kWaveform].store(wave); };
  source(11); assert(gui_param_relevant(&p, kExpansionShape));
  source(52); assert(gui_param_relevant(&p, kExpansionShape));
  source(7); assert(!gui_param_relevant(&p, kFmBrightness));
  source(49); assert(!gui_param_relevant(&p, kFmRatio)); assert(!gui_param_relevant(&p, kGenesisFeedback));
  source(27); assert(!gui_param_relevant(&p, kFmDetune)); assert(!gui_param_relevant(&p, kFmLfoRate));
  source(31); assert(!gui_param_relevant(&p, kFmPmDepth));
  source(30); assert(gui_param_relevant(&p, kFmPmDepth));
  source(58); assert(!gui_param_relevant(&p, kExpansionShape)); assert(gui_param_relevant(&p, kWaveSample1));

  int pipefd[2]; assert(pipe(pipefd) == 0);
  const pid_t pid = fork(); assert(pid >= 0);
  if (pid == 0) { close(pipefd[0]); for (;;) pause(); }
  close(pipefd[1]);
  p.gui_picker = pipefd[0]; p.gui_picker_pid = pid;
  const auto started = std::chrono::steady_clock::now();
  gui_close_picker(&p);
  assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
  assert(p.gui_picker == -1 && p.gui_picker_pid == -1);
  assert(waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD);
  gui_close_picker(&p); // Repeated teardown is harmless.
  if (argc == 3 && std::strcmp(argv[1], "--display-review") == 0) {
    display_review(p, argv[2]);
    // This standalone process owns fontconfig initialization. The plugin must never
    // finalize these process-global caches, which a DAW or another plugin may use.
    FcFini();
  }
}
