#include <map>
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "curses.h"
#include "enabler.h"
#include "init.h"

using std::map;
using std::pair;
using std::make_pair;

extern std::unordered_map<std::string, std::vector<long>> global_filename_to_texpos;
extern std::unordered_map<std::string, std::string> global_token_to_filename;

static bool curses_initialized = false;

static void endwin_void() {
  if (curses_initialized) {
    endwin();
    curses_initialized = false;
  }
}

struct CursesCell {
    uint32_t ch;
    uint8_t fr, fg, fb, br, bg, bb;
};

// Ensure tparm and tigetstr are defined
extern "C" {
    static char *(*_tigetstr)(const char *);
    static char *(*_tparm)(const char *, ...);
}

class renderer_curses : public renderer {
  std::vector<CursesCell> backbuffer;
  int last_w = 0, last_h = 0;
  char *setf24 = nullptr;
  char *setb24 = nullptr;
  bool has_truecolor = false;

  static int ncurses_map_color_256(int r, int g, int b) {
    // standard 6x6x6 color cube
    int ri = (r * 5) / 255;
    int gi = (g * 5) / 255;
    int bi = (b * 5) / 255;
    return 16 + 36 * ri + 6 * gi + bi;
  }

  std::string make_color_esc(bool is_fg, int r, int g, int b) {
    if (has_truecolor) {
        if (_tparm && (is_fg ? setf24 : setb24) && (is_fg ? setf24 : setb24) != (char*)-1) {
            int p1 = (r << 16) | (g << 8) | b;
            return _tparm(is_fg ? setf24 : setb24, p1);
        } else {
            // Direct ANSI escape string
            char buf[64];
            snprintf(buf, sizeof(buf), "\033[%d;2;%d;%d;%dm", is_fg ? 38 : 48, r, g, b);
            return buf;
        }
    } else {
        // Fallback to 256 colors
        int color_idx = ncurses_map_color_256(r, g, b);
        char buf[64];
        snprintf(buf, sizeof(buf), "\033[%d;5;%dm", is_fg ? 38 : 48, color_idx);
        return buf;
    }
  }

public:
  void update_tile(int x, int y) {
    if (x < 0 || y < 0 || x >= last_w || y >= last_h) return;
    const unsigned char *s = gps.screen + (x * gps.dimy + y) * 8;
    uint32_t ch = s[0];
    // 13 and 14 are sound indicators 
    if (init.display.disable_sound_indicator && (ch == 13 || ch == 14)) {
        const unsigned char *old_s = screen_old + (x * gps.dimy + y) * 8;
        uint32_t old_ch = old_s[0];
        if (old_ch != 13 && old_ch != 14 && old_ch != 0 && old_ch != 32) {
            ch = old_ch;
            uint8_t fr = old_s[1], fg = old_s[2], fb = old_s[3];
            uint8_t br = old_s[4], bg = old_s[5], bb = old_s[6];
            backbuffer[y * last_w + x] = {ch, fr, fg, fb, br, bg, bb};

            unsigned char *write_s = gps.screen + (x * gps.dimy + y) * 8;
            write_s[0] = ch;
            write_s[1] = fr; write_s[2] = fg; write_s[3] = fb;
            write_s[4] = br; write_s[5] = bg; write_s[6] = bb;
            return;
        }
    }
    uint8_t fr = s[1], fg = s[2], fb = s[3];
    uint8_t br = s[4], bg = s[5], bb = s[6];
    backbuffer[y * last_w + x] = {ch, fr, fg, fb, br, bg, bb};
  }

  void update_anchor_tile(int x,int y) {}

  void update_top_tile(int x,int y) {
    if (x < 0 || y < 0 || x >= last_w || y >= last_h) return;
    const unsigned char *s = gps.screen_top + (x * gps.dimy + y) * 8;
    uint32_t ch = s[0];
    if (ch == 0) return; // transparent if zero
    
    // 13 and 14 are sound indicators
    if (init.display.disable_sound_indicator && (ch == 13 || ch == 14)) {
        const unsigned char *old_s = screen_top_old + (x * gps.dimy + y) * 8;
        uint32_t old_ch = old_s[0];
        if (old_ch != 13 && old_ch != 14 && old_ch != 0 && old_ch != 32) {
            ch = old_ch;
            uint8_t fr = old_s[1], fg = old_s[2], fb = old_s[3];
            uint8_t br = old_s[4], bg = old_s[5], bb = old_s[6];
            backbuffer[y * last_w + x] = {ch, fr, fg, fb, br, bg, bb};

            // Re-write to gps.screen_top so it propagates safely
            unsigned char *write_s = gps.screen_top + (x * gps.dimy + y) * 8;
            write_s[0] = ch;
            write_s[1] = fr; write_s[2] = fg; write_s[3] = fb;
            write_s[4] = br; write_s[5] = bg; write_s[6] = bb;
            return;
        } else {
            return;
        }
    }

    uint8_t fr = s[1], fg = s[2], fb = s[3];
    uint8_t br = s[4], bg = s[5], bb = s[6];
    backbuffer[y * last_w + x] = {ch, fr, fg, fb, br, bg, bb};
  }

  void update_top_anchor_tile(int x,int y) {}
  void update_viewport_tile(graphic_viewportst *vp, int32_t x, int32_t y) {}
  void update_map_port_tile(graphic_map_portst *vp, int32_t x, int32_t y) {}
  void update_full_map_port(graphic_map_portst *port) {}
  void update_full_viewport(graphic_viewportst *port) {}

  void do_blank_screen_fill() {
      for (auto &c : backbuffer) {
          c.ch = ' ';
          c.fr = c.fg = c.fb = c.br = c.bg = c.bb = 0;
      }
  }

  void clean_tile_cache() {}
  void tidy_tile_cache() {}
  void clean_cached_tile(int32_t texpos, float r, float g, float b, float br, float bg, float bb, uint32_t flag) {}

  void update_all() {
    for (int x = 0; x < init.display.grid_x; x++) {
      for (int y = 0; y < init.display.grid_y; y++) {
        update_tile(x, y);
        if (gps.top_in_use) update_top_tile(x, y);
      }
    }
  }

  void render() {
    if (last_w == 0 || last_h == 0) return;

    std::string out = "\033[?25l\033[H";
    int current_fr=-1, current_fg=-1, current_fb=-1;
    int current_br=-1, current_bg=-1, current_bb=-1;

    for (int y = 0; y < last_h; y++) {
        for (int x = 0; x < last_w; x++) {
            auto c = backbuffer[y * last_w + x];

            // Emulated Mouse Cursor Overlay (Safe ANSI styling)
            if (g_mouse_emu.enabled && x == g_mouse_emu.cur_x && y == g_mouse_emu.cur_y) {
                if (g_mouse_emu.click_state == MouseEmulationState::CLICK_DOWN) {
                    c.br = 0; c.bg = 255; c.bb = 0;     // Cyber Green left click
                    c.fr = 0; c.fg = 0; c.fb = 0;
                } else if (g_mouse_emu.right_click_state == MouseEmulationState::CLICK_DOWN) {
                    c.br = 0; c.bg = 255; c.bb = 255;   // Cyber Cyan right click
                    c.fr = 0; c.fg = 0; c.fb = 0;
                } else if (g_mouse_emu.middle_click_state == MouseEmulationState::CLICK_DOWN) {
                    c.br = 255; c.bg = 255; c.bb = 0;   // Cyber Yellow middle click
                    c.fr = 0; c.fg = 0; c.fb = 0;
                } else {
                    // Default inversion: swap fg and bg
                    uint8_t old_fr = c.fr, old_fg = c.fg, old_fb = c.fb;
                    uint8_t old_br = c.br, old_bg = c.bg, old_bb = c.bb;

                    c.fr = old_br; c.fg = old_bg; c.fb = old_bb;
                    c.br = old_fr; c.bg = old_fg; c.bb = old_fb;

                    // Check if new background is same as new foreground, or is black/white
                    bool is_bg_same = (c.br == c.fr && c.bg == c.fg && c.bb == c.fb);
                    bool is_bg_black = (c.br == 0 && c.bg == 0 && c.bb == 0);
                    bool is_bg_white = (c.br == 255 && c.bg == 255 && c.bb == 255);

                    if (is_bg_same || is_bg_black || is_bg_white) {
                        // Cyber green background, black foreground for high contrast
                        c.br = 0; c.bg = 255; c.bb = 0;
                        c.fr = 0; c.fg = 0; c.fb = 0;
                    }
                }
            }

            if (c.fr != current_fr || c.fg != current_fg || c.fb != current_fb) {
                out += make_color_esc(true, c.fr, c.fg, c.fb);
                current_fr = c.fr; current_fg = c.fg; current_fb = c.fb;
            }
            if (c.br != current_br || c.bg != current_bg || c.bb != current_bb) {
                out += make_color_esc(false, c.br, c.bg, c.bb);
                current_br = c.br; current_bg = c.bg; current_bb = c.bb;
            }

            if (c.ch >= 32 && c.ch <= 126) {
                out += (char)c.ch;
            } else if (c.ch != 0 && c.ch < 256) {
                out += encode_utf8(charmap[c.ch]);
            } else {
                out += ' ';
            }
        }
        if (y < last_h - 1) out += "\r\n";
    }

    // Always hide hardware cursor; we handle the position natively now.
    out += "\033[?25l";
    write(STDOUT_FILENO, out.data(), out.size());
  }

  void resize(int w, int h) {
    if (enabler.overridden_grid_sizes.size() == 0)
      gps_allocate(w, h, w * 8, h * 12, 8, 12);

    last_w = init.display.grid_x;
    last_h = init.display.grid_y;
    backbuffer.resize(last_w * last_h);
    do_blank_screen_fill();

    gps.force_full_display_count = 1;
    enabler.flag |= ENABLERFLAG_RENDER;
  }

  void grid_resize(int w, int h) {
      gps_allocate(w, h, w * 8, h * 12, 8, 12);
      last_w = init.display.grid_x;
      last_h = init.display.grid_y;
      backbuffer.resize(last_w * last_h);
  }

  renderer_curses() {
    init_curses();

    char *term = getenv("COLORTERM");
    if (term && strcasecmp(term, "truecolor") == 0) {
        has_truecolor = true;
    }

    if (_tigetstr) {
        setf24 = _tigetstr("setf24");
        setb24 = _tigetstr("setb24");
    }

    if (has_truecolor) {
        if (!setf24 || setf24 == (char*)-1) {
            setf24 = (char*)"\033[%?%p1%{8}%<%t3%p1%d%e38;2;%p1%{65536}%/%d;%p1%{256}%/%{255}%&%d;%p1%{255}%&%d%;m";
            setb24 = (char*)"\033[%?%p1%{8}%<%t4%p1%d%e48;2;%p1%{65536}%/%d;%p1%{256}%/%{255}%&%d;%p1%{255}%&%d%;m";
        }
    } else if (setf24 && setf24 != (char*)-1) {
        has_truecolor = true;
    }
  }

  bool get_mouse_coords(int &x, int &y) { return false; }
  bool get_precise_mouse_coords(int &px, int &py, int &x, int &y) {
      if (g_mouse_emu.enabled) {
          x = g_mouse_emu.cur_x;
          y = g_mouse_emu.cur_y;
          px = x * 8;
          py = y * 12;
          return true;
      }
      return false;
  }
  SDL_Renderer *get_renderer(){ return NULL; }
  SDL_Window *get_window(){ return NULL; }
  void get_current_interface_tile_dims(int32_t &x,int32_t &y) { x = 8; y = 12; }
  void set_viewport_zoom_factor(int32_t n){}
};

static int getch_utf8() {
  int byte = wgetch(*stdscr_p);
  if (byte == ERR) return 0;
  if (byte > 0xff) return byte;
  int len = decode_utf8_predict_length(byte);
  if (!len) return 0;
  string input(len,0); input[0] = byte;
  for (int i = 1; i < len; i++) input[i] = wgetch(*stdscr_p);
  return -decode_utf8(input);
}

void enablerst::eventLoop_ncurses() {
  int x, y, oldx = 0, oldy = 0;
  renderer_curses *renderer = static_cast<renderer_curses*>(this->renderer);
  
  while (loopvar) {
    getmaxyx(*stdscr_p, y, x);
    if (y != oldy || x != oldx) {
      pause_async_loop();
      renderer->resize(x, y);
      unpause_async_loop();
      oldx = x; oldy = y;
    }
    
    Uint32 now = SDL_GetTicks();
    int key;
    bool paused_loop = false;
    while ((key = getch_utf8())) {
      if (!paused_loop) {
        pause_async_loop();
        paused_loop = true;
      }
      
      // Let the mouse emulation layer parse the key first
      if (handle_mouse_emu_input_ncurses(key, false)) continue;
      
      if (hooks_ncurses_key(key)) continue;
      bool esc = false;
#ifndef BUTTON1_PRESSED
#define BUTTON1_PRESSED 000000000002L
#endif
#ifndef BUTTON1_CLICKED
#define BUTTON1_CLICKED 000000000004L
#endif
#ifndef BUTTON2_PRESSED
#define BUTTON2_PRESSED 000000000100L
#endif
#ifndef BUTTON2_CLICKED
#define BUTTON2_CLICKED 000000000200L
#endif
#ifndef BUTTON3_PRESSED
#define BUTTON3_PRESSED 000000004000L
#endif
#ifndef BUTTON3_CLICKED
#define BUTTON3_CLICKED 000000010000L
#endif
#ifndef BUTTON4_PRESSED
#define BUTTON4_PRESSED 000020000000L
#endif
#ifndef BUTTON5_PRESSED
#define BUTTON5_PRESSED 001000000000L
#endif
      if (key == KEY_MOUSE) {
        MEVENT ev;
        if (getmouse(&ev) == OK) {
          // Check for scroll wheel up
            if (ev.bstate & BUTTON4_PRESSED) {
                SDL_Event sdl_ev;
                memset(&sdl_ev, 0, sizeof(sdl_ev));
                sdl_ev.type = SDL_MOUSEWHEEL;
                sdl_ev.wheel.y = 1;
                sdl_ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                enabler.add_input(sdl_ev, now);
            }
            // Check for scroll wheel down
            else if (ev.bstate & BUTTON5_PRESSED) {
                SDL_Event sdl_ev;
                memset(&sdl_ev, 0, sizeof(sdl_ev));
                sdl_ev.type = SDL_MOUSEWHEEL;
                sdl_ev.wheel.y = -1;
                sdl_ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                enabler.add_input(sdl_ev, now);
            }
            // Native Terminal Mouse Click Support
            else if (ev.bstate & BUTTON1_CLICKED || ev.bstate & BUTTON1_PRESSED) {
                g_mouse_emu.cur_x = ev.x;
                g_mouse_emu.cur_y = ev.y;
                g_mouse_emu.click_state = MouseEmulationState::CLICK_DOWN;
                g_mouse_emu.enabled = true;
            }
            // Native Terminal Mouse Middle Click Support
            else if (ev.bstate & BUTTON2_CLICKED || ev.bstate & BUTTON2_PRESSED) {
                g_mouse_emu.cur_x = ev.x;
                g_mouse_emu.cur_y = ev.y;
                g_mouse_emu.middle_click_state = MouseEmulationState::CLICK_DOWN;
                g_mouse_emu.enabled = true;
            }
            // Native Terminal Mouse Right Click Support
            else if (ev.bstate & BUTTON3_CLICKED || ev.bstate & BUTTON3_PRESSED) {
                g_mouse_emu.cur_x = ev.x;
                g_mouse_emu.cur_y = ev.y;
                g_mouse_emu.right_click_state = MouseEmulationState::CLICK_DOWN;
                g_mouse_emu.enabled = true;
            }
        }
      } else if (key == -27) { // esc
        int second = getch_utf8();
        if (second) {
          esc = true;
          key = second;
        }
      }
      add_input_ncurses(key, now, esc);
    }

    if (paused_loop)
      unpause_async_loop();

    do_frame();
  }
}

//// libncursesw stub ////

extern "C" {
  static void *handle;
  WINDOW **stdscr_p;

  int COLOR_PAIRS;
  static int (*_erase)(void);
  static int (*_wmove)(WINDOW *w, int y, int x);
  static int (*_waddnstr)(WINDOW *w, const char *s, int n);
  static int (*_nodelay)(WINDOW *w, bool b);
  static int (*_refresh)(void);
  static int (*_wgetch)(WINDOW *w);
  static int (*_endwin)(void);
  static WINDOW *(*_initscr)(void);
  static int (*_raw)(void);
  static int (*_keypad)(WINDOW *w, bool b);
  static int (*_noecho)(void);
  static int (*_set_escdelay)(int delay);
  static int (*_curs_set)(int s);
  static int (*_start_color)(void);
  static int (*_init_pair)(short p, short fg, short bg);
  static int (*_getmouse)(MEVENT *m);
  static int (*_waddnwstr)(WINDOW *w, const wchar_t *s, int i);
  static mmask_t (*_mousemask)(mmask_t, mmask_t *);

  static void *dlsym_orexit(const char *symbol, bool actually_exit = true) {
    void *sym = dlsym(handle, symbol);
    if (!sym) {
      printf("Symbol not found: %s\n", symbol);
      if (actually_exit)
        exit(EXIT_FAILURE);
    }
    return sym;
  }

  int erase(void) { return _erase(); }
  int wmove(WINDOW *w, int y, int x) { return _wmove(w, y, x); }
  int waddnstr(WINDOW *w, const char *s, int n) { return _waddnstr(w, s, n); }
  int nodelay(WINDOW *w, bool b) { return _nodelay(w, b); }
  int refresh(void) { return _refresh(); }
  int wgetch(WINDOW *w) { return _wgetch(w); }
  int endwin(void) { return _endwin(); }
  WINDOW *initscr(void) { return _initscr(); }
  int raw(void) { return _raw(); }
  int keypad(WINDOW *w, bool b) { return _keypad(w, b); }
  int noecho(void) { return _noecho(); }
  int set_escdelay(int delay) { return _set_escdelay ? _set_escdelay(delay) : 0; }
  int curs_set(int s) { return _curs_set(s); }
  int start_color(void) { return _start_color(); }
  int init_pair(short p, short fg, short bg) { return _init_pair(p, fg, bg); }
  int getmouse(MEVENT *m) { return _getmouse(m); }
  int waddnwstr(WINDOW *w, const wchar_t *s, int n) { return _waddnwstr(w, s, n); }
  mmask_t mousemask(mmask_t newmask, mmask_t *oldmask) {
    if (_mousemask) return _mousemask(newmask, oldmask);
    return 0;
  }

  void init_curses() {
    static bool stub_initialized = false;
    if (!stub_initialized) {
      stub_initialized = true;
      handle = dlopen("libncursesw.so.5", RTLD_LAZY);
      if (handle) goto opened;
      handle = dlopen("libncursesw.so", RTLD_LAZY);
      if (handle) goto opened;
      puts("Didn't find any flavor of libncursesw, attempting libncurses");
      sleep(5);
      handle = dlopen("libncurses.dylib", RTLD_LAZY);
      if (handle) goto opened;
      handle = dlopen("libncurses.so.5", RTLD_LAZY);
      if (handle) goto opened;
      handle = dlopen("libncurses.so", RTLD_LAZY);
      if (handle) goto opened;
      handle = dlopen("libncurses.5.4.dylib", RTLD_LAZY);
      if (handle) goto opened;
      handle = dlopen("/usr/lib/libncurses.dylib", RTLD_LAZY);
      if (handle) goto opened;
      handle = dlopen("/usr/lib/libncurses.5.4.dylib", RTLD_LAZY);
      if (handle) goto opened;

    opened:
      if (!handle) {
        puts("Unable to open any flavor of libncurses!");
        exit(EXIT_FAILURE);
      }
      int *pairs = (int*)dlsym_orexit("COLOR_PAIRS", false);
      if(pairs) COLOR_PAIRS = *pairs;
      stdscr_p = (WINDOW**)dlsym_orexit("stdscr");
      _erase = (int (*)(void))dlsym_orexit("erase");
      _wmove = (int (*)(WINDOW *w, int y, int x))dlsym_orexit("wmove");
      _waddnstr = (int (*)(WINDOW *w, const char *s, int n))dlsym_orexit("waddnstr");
      _nodelay = (int (*)(WINDOW *w, bool b))dlsym_orexit("nodelay");
      _refresh = (int (*)(void))dlsym_orexit("refresh");
      _wgetch = (int (*)(WINDOW *w))dlsym_orexit("wgetch");
      _endwin = (int (*)(void))dlsym_orexit("endwin");
      _initscr = (WINDOW *(*)(void))dlsym_orexit("initscr");
      _raw = (int (*)(void))dlsym_orexit("raw");
      _keypad = (int (*)(WINDOW *w, bool b))dlsym_orexit("keypad");
      _noecho = (int (*)(void))dlsym_orexit("noecho");
      _set_escdelay = (int (*)(int delay))dlsym_orexit("set_escdelay", false);
      _curs_set = (int (*)(int s))dlsym_orexit("curs_set");
      _start_color = (int (*)(void))dlsym_orexit("start_color");
      _init_pair = (int (*)(short p, short fg, short bg))dlsym_orexit("init_pair");
      _getmouse = (int (*)(MEVENT *m))dlsym_orexit("getmouse");
      _mousemask = (mmask_t (*)(mmask_t, mmask_t *))dlsym_orexit("mousemask", false);
      _waddnwstr = (int (*)(WINDOW *w, const wchar_t *s, int i))dlsym_orexit("waddnwstr");
      _tigetstr = (char *(*)(const char *))dlsym_orexit("tigetstr", false);
      _tparm = (char *(*)(const char *, ...))dlsym_orexit("tparm", false);
    }
    
    if (!curses_initialized) {
      curses_initialized = true;
      WINDOW *new_window = initscr();
      if (!new_window) {
        puts("unable to create ncurses window - initscr failed!");
        exit(EXIT_FAILURE);
      }
      if (!*stdscr_p) *stdscr_p = new_window;
      raw();
      noecho();
      keypad(*stdscr_p, true);
      nodelay(*stdscr_p, true);
      set_escdelay(25);
      curs_set(0);
      start_color();
      init_pair(1, COLOR_WHITE, COLOR_BLACK);
      if (_mousemask) {
        mousemask(0xffffffff, NULL); // Enable all mouse events including scroll wheel
      }
      
      atexit(endwin_void);
    }
  }
}
