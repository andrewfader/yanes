// Native window backends for the editor. Included after ui_frontend.hpp inside the
// anonymous namespace.
#pragma once

bool gui_is_open(const Plugin* p);
void gui_paint(Plugin* p);

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

void gui_close_picker(Plugin* p) {
  if (!p->gui_picker) return;
  pclose(p->gui_picker);
  p->gui_picker = nullptr;
  p->gui_picker_slot = -1;
  p->gui_picker_output.clear();
}

// Starts the dialog and returns; the answer is collected in gui_poll_picker. Reading the
// pipe here instead would block the host's main thread for as long as the dialog stayed
// open, and blocking it is what leaves a host waiting on an editor that never answers.
bool gui_choose_sample(Plugin* p, int slot) {
  if (p->gui_picker) return false;
  FILE* picker = popen("zenity --file-selection --title='Load YANES WAV or DPCM sample' --file-filter='Audio | *.wav *.WAV *.ydmc'", "r");
  if (!picker) return false;
  const int fd = fileno(picker);
  if (fd < 0) { pclose(picker); return false; }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  p->gui_picker = picker;
  p->gui_picker_slot = slot;
  p->gui_picker_output.clear();
  return false;
}

void gui_paint(Plugin* p);

void gui_poll_picker(Plugin* p) {
  if (!p->gui_picker) return;
  char chunk[512];
  for (;;) {
    const ssize_t got = read(fileno(p->gui_picker), chunk, sizeof(chunk));
    if (got > 0) { p->gui_picker_output.append(chunk, static_cast<size_t>(got)); continue; }
    // Nothing to read yet means the user is still choosing; anything else means the
    // dialog has closed and whatever it wrote is complete.
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    break;
  }
  const int slot = p->gui_picker_slot;
  std::string path = p->gui_picker_output;
  gui_close_picker(p);
  if (const size_t end = path.find_first_of("\r\n"); end != std::string::npos) path.resize(end);
  if (path.empty() || slot < 0 || slot >= 16) return;
  auto bank = load_dpcm_file(path);
  if (!bank) return;
  install_dpcm_bank(p, static_cast<size_t>(slot), std::move(bank));
  gui_mark_state(p);
  gui_paint(p);
}

void gui_release_back_buffer(Plugin* p) {
  if (p->gui_back_xft) XftDrawDestroy(p->gui_back_xft);
  if (p->gui_back) XFreePixmap(p->display, p->gui_back);
  p->gui_back_xft = nullptr; p->gui_back = 0; p->gui_back_width = p->gui_back_height = 0;
}

void gui_release_fonts(Plugin* p) {
  for (auto& font : p->gui_fonts) { if (font) XftFontClose(p->display, font); font = nullptr; }
  p->gui_font_pixels = {};
}

void gui_paint(Plugin* p) {
  if (!p->display || !p->window) return;
  const int w = static_cast<int>(p->gui_width), h = static_cast<int>(p->gui_height);
  for (size_t i = 0; i < p->gui_fonts.size(); ++i) {
    const int wanted = yanes::ui::font_pixels(static_cast<yanes::ui::TextSize>(i), w, h);
    if (wanted == p->gui_font_pixels[i] && p->gui_fonts[i]) continue;
    char pattern[128]{};
    std::snprintf(pattern, sizeof(pattern), "DejaVu Sans:weight=%s:pixelsize=%d", i == 2 ? "bold" : "medium", wanted);
    if (auto* font = XftFontOpenName(p->display, DefaultScreen(p->display), pattern)) {
      if (p->gui_fonts[i]) XftFontClose(p->display, p->gui_fonts[i]);
      p->gui_fonts[i] = font;
      p->gui_font_pixels[i] = wanted;
    }
  }
  if (!p->gui_back || p->gui_back_width != w || p->gui_back_height != h) {
    gui_release_back_buffer(p);
    const int screen = DefaultScreen(p->display);
    p->gui_back = XCreatePixmap(p->display, p->window, static_cast<unsigned>(w), static_cast<unsigned>(h),
                                static_cast<unsigned>(DefaultDepth(p->display, screen)));
    p->gui_back_xft = XftDrawCreate(p->display, p->gui_back, DefaultVisual(p->display, screen),
                                    DefaultColormap(p->display, screen));
    p->gui_back_width = w; p->gui_back_height = h;
  }
  if (!p->gui_back_xft) return;
  yanes::ui::X11Canvas canvas(p->display, p->gui_back, p->gc, p->gui_back_xft, p->gui_fonts, w, h);
  gui_draw(p, canvas);
  XCopyArea(p->display, p->gui_back, p->window, p->gc, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h), 0, 0);
  XFlush(p->display);
}

unsigned gui_x11_modifiers(unsigned state) {
  return ((state & ShiftMask) ? yanes::ui::kShift : 0U) | ((state & ControlMask) ? yanes::ui::kControl : 0U) |
         ((state & Mod1Mask) ? yanes::ui::kAlt : 0U);
}

// Drains whatever X has queued, then paints at most once. Called from the host's main thread,
// either because the connection became readable or because the editor's timer fired. Painting
// per event is what made fast pointer movement lag and flicker.
void gui_pump(Plugin* p) {
  bool dirty = false;
  while (p->display && XPending(p->display)) {
    XEvent e{};
    XNextEvent(p->display, &e);
    const auto logical = [&](int px, int py) {
      return std::pair<int,int>{yanes::ui::unscale_x(px, static_cast<int>(p->gui_width)),
                                yanes::ui::unscale_y(py, static_cast<int>(p->gui_height))};
    };
    switch (e.type) {
      case Expose: if (e.xexpose.count == 0) dirty = true; break;
      case ConfigureNotify:
        if (static_cast<uint32_t>(e.xconfigure.width) != p->gui_width || static_cast<uint32_t>(e.xconfigure.height) != p->gui_height) {
          p->gui_width = static_cast<uint32_t>(std::max(1, e.xconfigure.width));
          p->gui_height = static_cast<uint32_t>(std::max(1, e.xconfigure.height));
          dirty = true;
        }
        break;
      case LeaveNotify: gui_input(p, GuiPointer::Leave, 0, 0, 0); dirty = true; break;
      case ButtonPress: case ButtonRelease: {
        const auto [x, y] = logical(e.xbutton.x, e.xbutton.y);
        gui_input(p, e.type == ButtonPress ? GuiPointer::Down : GuiPointer::Up, static_cast<int>(e.xbutton.button), x, y,
                  gui_x11_modifiers(e.xbutton.state));
        dirty = true;
        break;
      }
      case MotionNotify: {
        // Only the newest queued position matters.
        while (XCheckTypedWindowEvent(p->display, p->window, MotionNotify, &e)) {}
        const auto [x, y] = logical(e.xmotion.x, e.xmotion.y);
        gui_input(p, GuiPointer::Move, 0, x, y, gui_x11_modifiers(e.xmotion.state));
        dirty = true;
        break;
      }
      default: break;
    }
  }
  if (dirty) gui_paint(p);
}

void gui_on_timer(const clap_plugin_t* plugin, clap_id timer_id) {
  auto* p = self(plugin);
  if (timer_id != p->gui_timer) return;
  gui_pump(p);
  gui_poll_picker(p);
  if (!p->display) return;
  bool dirty = gui_tick(p);
  const uint64_t revision = p->ui_revision.load(std::memory_order_acquire);
  if (revision != p->gui_seen_revision) { p->gui_seen_revision = revision; dirty = true; }
  // The scope and meters redraw at a third of the tick rate.
  if (++p->gui_scope_ticks >= 3) {
    p->gui_scope_ticks = 0;
    const uint64_t scope = p->scope_revision.load(std::memory_order_acquire);
    if (scope != p->gui_seen_scope_revision) { p->gui_seen_scope_revision = scope; dirty = true; }
  }
  if (dirty) gui_paint(p);
}

void gui_on_fd(const clap_plugin_t* plugin, int fd, clap_posix_fd_flags_t) {
  auto* p = self(plugin);
  if (fd == p->gui_fd) gui_pump(p);
}

const clap_plugin_timer_support_t kTimerSupport{gui_on_timer};
const clap_plugin_posix_fd_support_t kPosixFdSupport{gui_on_fd};

bool gui_is_open(const Plugin* p) { return p->display != nullptr; }
bool gui_supported(const clap_plugin_t* plugin, const char* api, bool floating) {
  // Without a host timer there is no main thread to run the editor on, and running it on
  // one of our own is what this backend no longer does. Declining leaves the host on its
  // own parameter panel, which stays fully usable.
  return plugin && self(plugin)->host_timers && self(plugin)->host_timers->register_timer &&
         api && !std::strcmp(api, CLAP_WINDOW_API_X11) && !floating;
}
bool gui_preferred(const clap_plugin_t*, const char** api, bool* floating) {
  *api = CLAP_WINDOW_API_X11; *floating = false; return true;
}
bool gui_create(const clap_plugin_t* plugin, const char* api, bool floating) {
  auto* p = self(plugin);
  if (!gui_supported(plugin, api, floating) || p->display) return false;
  p->display = XOpenDisplay(nullptr);
  if (!p->display) return false;
  p->window = XCreateSimpleWindow(p->display, DefaultRootWindow(p->display), 0, 0, p->gui_width, p->gui_height, 0, 0, 0x0b1119);
  // No server-side background: the back buffer covers every pixel, and letting X clear the
  // window first on every expose or resize is a visible flash.
  XSetWindowBackgroundPixmap(p->display, p->window, None);
  p->gc = XCreateGC(p->display, p->window, 0, nullptr);
  XSetGraphicsExposures(p->display, p->gc, False);
  p->gui_seen_revision = p->ui_revision.load(std::memory_order_acquire);
  p->gui_seen_scope_revision = p->scope_revision.load(std::memory_order_acquire);
  p->gui_scope_ticks = 0;
  XSelectInput(p->display, p->window, ExposureMask | ButtonPressMask | ButtonReleaseMask |
               Button1MotionMask | PointerMotionMask | LeaveWindowMask | StructureNotifyMask);
  // The timer is what the editor cannot run without; the descriptor is an optimisation that
  // delivers X events as they arrive rather than at the next tick.
  p->gui_timer = CLAP_INVALID_ID;
  if (!p->host_timers->register_timer(p->host, 16, &p->gui_timer)) {
    gui_destroy(plugin);
    return false;
  }
  if (p->host_fds && p->host_fds->register_fd &&
      p->host_fds->register_fd(p->host, ConnectionNumber(p->display), CLAP_POSIX_FD_READ))
    p->gui_fd = ConnectionNumber(p->display);
  return true;
}
void gui_destroy(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (p->gui_timer != CLAP_INVALID_ID && p->host_timers && p->host_timers->unregister_timer)
    p->host_timers->unregister_timer(p->host, p->gui_timer);
  p->gui_timer = CLAP_INVALID_ID;
  if (p->gui_fd >= 0 && p->host_fds && p->host_fds->unregister_fd)
    p->host_fds->unregister_fd(p->host, p->gui_fd);
  p->gui_fd = -1;
  gui_close_picker(p);
  if (p->display) {
    gui_release_back_buffer(p);
    gui_release_fonts(p);
    if (p->gc) XFreeGC(p->display, p->gc);
    if (p->window) XDestroyWindow(p->display, p->window);
    XCloseDisplay(p->display);
  }
  p->display = nullptr; p->window = 0; p->gc = nullptr;
}
bool gui_scale(const clap_plugin_t*, double) { return false; }
bool gui_get_size(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
  *w = self(plugin)->gui_width; *h = self(plugin)->gui_height; return true;
}
bool gui_can_resize(const clap_plugin_t*) { return true; }
bool gui_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
  if (!hints) return false;
  *hints = {};
  hints->can_resize_horizontally = true;
  hints->can_resize_vertically = true;
  return true;
}
bool gui_adjust(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
  if (!w || !h) return false;
  *w = std::max(*w, static_cast<uint32_t>(yanes::ui::minimum_width));
  *h = std::max(*h, static_cast<uint32_t>(yanes::ui::minimum_height));
  return true;
}
bool gui_set_size(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
  auto* p = self(plugin);
  if (w < static_cast<uint32_t>(yanes::ui::minimum_width) || h < static_cast<uint32_t>(yanes::ui::minimum_height)) return false;
  p->gui_width = w; p->gui_height = h;
  if (p->display) { XResizeWindow(p->display, p->window, w, h); gui_paint(p); }
  return true;
}
bool gui_parent(const clap_plugin_t* plugin, const clap_window_t* parent) {
  auto* p = self(plugin);
  if (!p->display || !parent || std::strcmp(parent->api, CLAP_WINDOW_API_X11)) return false;
  XReparentWindow(p->display, p->window, parent->x11, 0, 0);
  XFlush(p->display);
  return true;
}
bool gui_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
void gui_title(const clap_plugin_t* plugin, const char* title) {
  auto* p = self(plugin);
  if (p->display && p->window && title) { XStoreName(p->display, p->window, title); XFlush(p->display); }
}
bool gui_show(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (!p->display) return false;
  XMapWindow(p->display, p->window);
  gui_paint(p);
  return true;
}
bool gui_hide(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (!p->display) return false;
  XUnmapWindow(p->display, p->window);
  return true;
}

#elif defined(_WIN32)

bool gui_choose_sample(Plugin* p, int slot) {
  char path[4096]{};
  OPENFILENAMEA ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = p->hwnd;
  ofn.lpstrFile = path;
  ofn.nMaxFile = sizeof(path);
  ofn.lpstrFilter = "Audio (*.wav, *.ydmc)\0*.wav;*.WAV;*.ydmc\0All\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameA(&ofn)) return false;
  if (auto bank = load_dpcm_file(path)) {
    install_dpcm_bank(p, static_cast<size_t>(slot), std::move(bank));
    return true;
  }
  return false;
}

void gui_paint(Plugin* p) {
  if (!p->hwnd) return;
  RECT client{};
  GetClientRect(p->hwnd, &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  p->gui_width = static_cast<uint32_t>(width);
  p->gui_height = static_cast<uint32_t>(height);
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(p->hwnd, &ps);
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
  HGDIOBJ previous = SelectObject(mem, bitmap);
  {
    yanes::ui::Win32Canvas canvas(mem, width, height);
    gui_draw(p, canvas);
  }
  BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
  SelectObject(mem, previous);
  DeleteObject(bitmap);
  DeleteDC(mem);
  EndPaint(p->hwnd, &ps);
}

LRESULT CALLBACK gui_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* p = reinterpret_cast<Plugin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (!p) return DefWindowProcW(hwnd, msg, wparam, lparam);
  const auto logical = [&](LPARAM lp) {
    return std::pair<int,int>{yanes::ui::unscale_x(GET_X_LPARAM(lp), static_cast<int>(p->gui_width)),
                              yanes::ui::unscale_y(GET_Y_LPARAM(lp), static_cast<int>(p->gui_height))};
  };
  // Mouse messages carry Shift and Ctrl in wparam; Alt has to be asked for.
  const auto modifiers = [&](WPARAM keys) {
    return ((keys & MK_SHIFT) ? yanes::ui::kShift : 0U) | ((keys & MK_CONTROL) ? yanes::ui::kControl : 0U) |
           (GetKeyState(VK_MENU) < 0 ? yanes::ui::kAlt : 0U);
  };
  switch (msg) {
    case WM_PAINT: gui_paint(p); return 0;
    // The back buffer paints every pixel; letting Windows erase first is a visible flash.
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      p->gui_width = static_cast<uint32_t>(std::max(1, static_cast<int>(LOWORD(lparam))));
      p->gui_height = static_cast<uint32_t>(std::max(1, static_cast<int>(HIWORD(lparam))));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_TIMER: {
      const bool animating = gui_tick(p);
      const uint64_t revision = p->ui_revision.load(std::memory_order_acquire);
      const uint64_t scope = p->scope_revision.load(std::memory_order_acquire);
      if (animating || revision != p->gui_seen_revision || scope != p->gui_seen_scope_revision) {
        p->gui_seen_revision = revision;
        p->gui_seen_scope_revision = scope;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      // Ask for WM_MOUSELEAVE, so hover highlights clear when the pointer leaves.
      TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      const auto [x, y] = logical(lparam);
      gui_input(p, GuiPointer::Move, 0, x, y, modifiers(wparam));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
      SetCapture(hwnd);
      const int button = msg == WM_LBUTTONDOWN ? 1 : (msg == WM_MBUTTONDOWN ? 2 : 3);
      const auto [x, y] = logical(lparam);
      gui_input(p, GuiPointer::Down, button, x, y, modifiers(wparam));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
      const int button = msg == WM_LBUTTONUP ? 1 : (msg == WM_MBUTTONUP ? 2 : 3);
      const auto [x, y] = logical(lparam);
      gui_input(p, GuiPointer::Up, button, x, y, modifiers(wparam));
      ReleaseCapture();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &pt);
      const int x = yanes::ui::unscale_x(pt.x, static_cast<int>(p->gui_width));
      const int y = yanes::ui::unscale_y(pt.y, static_cast<int>(p->gui_height));
      gui_input(p, GuiPointer::Down, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 4 : 5, x, y,
                modifiers(GET_KEYSTATE_WPARAM(wparam)));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSELEAVE:
      gui_input(p, GuiPointer::Leave, 0, 0, 0);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    default: break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool gui_is_open(const Plugin* p) { return p->hwnd != nullptr; }
bool gui_supported(const clap_plugin_t*, const char* api, bool floating) {
  return api && !std::strcmp(api, CLAP_WINDOW_API_WIN32) && !floating;
}
bool gui_preferred(const clap_plugin_t*, const char** api, bool* floating) {
  *api = CLAP_WINDOW_API_WIN32; *floating = false; return true;
}
bool gui_create(const clap_plugin_t* plugin, const char* api, bool floating) {
  auto* p = self(plugin);
  if (!gui_supported(plugin, api, floating) || p->hwnd) return false;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = gui_wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW is narrow without UNICODE
  wc.lpszClassName = L"YANES_Editor_0_2_0";
  RegisterClassExW(&wc);
  p->hwnd = CreateWindowExW(0, wc.lpszClassName, L"YANES", WS_CHILD,
                            0, 0, static_cast<int>(p->gui_width), static_cast<int>(p->gui_height),
                            GetDesktopWindow(), nullptr, wc.hInstance, p);
  if (!p->hwnd) return false;
  SetTimer(p->hwnd, 1, 16, nullptr);
  p->gui_seen_revision = p->ui_revision.load(std::memory_order_acquire);
  return true;
}
void gui_destroy(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (p->hwnd) { KillTimer(p->hwnd, 1); DestroyWindow(p->hwnd); }
  p->hwnd = nullptr;
}
bool gui_scale(const clap_plugin_t*, double) { return false; }
bool gui_get_size(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
  *w = self(plugin)->gui_width; *h = self(plugin)->gui_height; return true;
}
bool gui_can_resize(const clap_plugin_t*) { return true; }
bool gui_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
  if (!hints) return false;
  *hints = {};
  hints->can_resize_horizontally = true;
  hints->can_resize_vertically = true;
  return true;
}
bool gui_adjust(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
  if (!w || !h) return false;
  *w = std::max(*w, static_cast<uint32_t>(yanes::ui::minimum_width));
  *h = std::max(*h, static_cast<uint32_t>(yanes::ui::minimum_height));
  return true;
}
bool gui_set_size(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
  auto* p = self(plugin);
  if (w < static_cast<uint32_t>(yanes::ui::minimum_width) || h < static_cast<uint32_t>(yanes::ui::minimum_height)) return false;
  p->gui_width = w; p->gui_height = h;
  if (p->hwnd) SetWindowPos(p->hwnd, nullptr, 0, 0, static_cast<int>(w), static_cast<int>(h), SWP_NOZORDER | SWP_NOMOVE);
  return true;
}
bool gui_parent(const clap_plugin_t* plugin, const clap_window_t* parent) {
  auto* p = self(plugin);
  if (!p->hwnd || !parent || std::strcmp(parent->api, CLAP_WINDOW_API_WIN32) || !parent->win32) return false;
  SetParent(p->hwnd, static_cast<HWND>(parent->win32));
  ShowWindow(p->hwnd, SW_SHOW);
  return true;
}
bool gui_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
void gui_title(const clap_plugin_t* plugin, const char* title) {
  if (self(plugin)->hwnd && title) SetWindowTextA(self(plugin)->hwnd, title);
}
bool gui_show(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (!p->hwnd) return false;
  ShowWindow(p->hwnd, SW_SHOW);
  InvalidateRect(p->hwnd, nullptr, FALSE);
  return true;
}
bool gui_hide(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (!p->hwnd) return false;
  ShowWindow(p->hwnd, SW_HIDE);
  return true;
}

#elif defined(__APPLE__)

bool gui_choose_sample(Plugin* p, int slot) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  [panel setAllowsMultipleSelection:NO];
  [panel setCanChooseDirectories:NO];
  [panel setAllowedFileTypes:@[@"wav", @"WAV", @"ydmc"]];
  if ([panel runModal] != NSModalResponseOK) return false;
  NSURL* url = [[panel URLs] firstObject];
  if (!url) return false;
  const char* path = [[url path] UTF8String];
  if (!path) return false;
  if (auto bank = load_dpcm_file(path)) {
    install_dpcm_bank(p, static_cast<size_t>(slot), std::move(bank));
    return true;
  }
  return false;
}

void gui_paint(Plugin* p) {
  if (!p->view) return;
  const NSRect bounds = [p->view bounds];
  p->gui_width = static_cast<uint32_t>(std::max(1.0, bounds.size.width));
  p->gui_height = static_cast<uint32_t>(std::max(1.0, bounds.size.height));
  yanes::ui::CocoaCanvas canvas(static_cast<int>(p->gui_width), static_cast<int>(p->gui_height));
  gui_draw(p, canvas);
}

bool gui_is_open(const Plugin* p) { return p->view != nullptr; }
bool gui_supported(const clap_plugin_t*, const char* api, bool floating) {
  return api && !std::strcmp(api, CLAP_WINDOW_API_COCOA) && !floating;
}
bool gui_preferred(const clap_plugin_t*, const char** api, bool* floating) {
  *api = CLAP_WINDOW_API_COCOA; *floating = false; return true;
}
bool gui_create(const clap_plugin_t* plugin, const char* api, bool floating) {
  auto* p = self(plugin);
  if (!gui_supported(plugin, api, floating) || p->view) return false;
  const NSRect frame = NSMakeRect(0, 0, p->gui_width, p->gui_height);
  p->view = [[YANES_EDITOR_VIEW alloc] initWithFrame:frame];
  p->view.plugin = p;
  p->view.refresh = [NSTimer scheduledTimerWithTimeInterval:0.016 target:p->view selector:@selector(onRefresh:)
                                                   userInfo:nil repeats:YES];
  p->gui_seen_revision = p->ui_revision.load(std::memory_order_acquire);
  return p->view != nullptr;
}
void gui_destroy(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (p->view) {
    [p->view.refresh invalidate];
    p->view.refresh = nil;
    [p->view removeFromSuperview];
    [p->view release];
    p->view = nullptr;
  }
}
bool gui_scale(const clap_plugin_t*, double) { return false; }
bool gui_get_size(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
  *w = self(plugin)->gui_width; *h = self(plugin)->gui_height; return true;
}
bool gui_can_resize(const clap_plugin_t*) { return true; }
bool gui_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
  if (!hints) return false;
  *hints = {};
  hints->can_resize_horizontally = true;
  hints->can_resize_vertically = true;
  return true;
}
bool gui_adjust(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
  if (!w || !h) return false;
  *w = std::max(*w, static_cast<uint32_t>(yanes::ui::minimum_width));
  *h = std::max(*h, static_cast<uint32_t>(yanes::ui::minimum_height));
  return true;
}
bool gui_set_size(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
  auto* p = self(plugin);
  if (w < static_cast<uint32_t>(yanes::ui::minimum_width) || h < static_cast<uint32_t>(yanes::ui::minimum_height)) return false;
  p->gui_width = w; p->gui_height = h;
  if (p->view) [p->view setFrameSize:NSMakeSize(w, h)];
  return true;
}
bool gui_parent(const clap_plugin_t* plugin, const clap_window_t* parent) {
  auto* p = self(plugin);
  if (!p->view || !parent || std::strcmp(parent->api, CLAP_WINDOW_API_COCOA) || !parent->cocoa) return false;
  NSView* host = static_cast<NSView*>(parent->cocoa);
  [host addSubview:p->view];
  [p->view setFrame:[host bounds]];
  [p->view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  return true;
}
bool gui_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
void gui_title(const clap_plugin_t*, const char*) {}
bool gui_show(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (!p->view) return false;
  [p->view setHidden:NO];
  [p->view setNeedsDisplay:YES];
  return true;
}
bool gui_hide(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  if (!p->view) return false;
  [p->view setHidden:YES];
  return true;
}

void editor_cocoa_draw(void* plugin) { gui_paint(static_cast<Plugin*>(plugin)); }
void editor_cocoa_input(void* plugin, GuiPointer action, int button, int x, int y, unsigned modifiers) {
  gui_input(static_cast<Plugin*>(plugin), action, button, x, y, modifiers);
  auto* p = static_cast<Plugin*>(plugin);
  if (p->view) [p->view setNeedsDisplay:YES];
}
void editor_cocoa_refresh(void* plugin) {
  auto* p = static_cast<Plugin*>(plugin);
  const bool animating = gui_tick(p);
  const uint64_t revision = p->ui_revision.load(std::memory_order_acquire);
  const uint64_t scope = p->scope_revision.load(std::memory_order_acquire);
  if (animating || revision != p->gui_seen_revision || scope != p->gui_seen_scope_revision) {
    p->gui_seen_revision = revision;
    p->gui_seen_scope_revision = scope;
    if (p->view) [p->view setNeedsDisplay:YES];
  }
}

#else
bool gui_is_open(const Plugin*) { return false; }
void gui_destroy(const clap_plugin_t*) {}
#endif

#ifdef YANES_HAS_EDITOR
const clap_plugin_gui_t kGui{gui_supported, gui_preferred, gui_create, gui_destroy, gui_scale, gui_get_size,
  gui_can_resize, gui_resize_hints, gui_adjust, gui_set_size, gui_parent, gui_transient, gui_title, gui_show, gui_hide};
#endif
