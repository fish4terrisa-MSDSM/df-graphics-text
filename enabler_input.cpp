#include <map>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <stdlib.h>
#include <math.h>

#include <SDL2/SDL.h>

#include "interface.h"
#include "enabler_input.h"
#include "init.h"
extern initst init;
#include "platform.h"
#include "files.h"
#include "find_files.h"
#include "svector.h"
#ifdef CURSES
#include "curses.h"
#endif
#include "ViewBase.h"
using namespace std;

// The timeline events we actually pass back from get_input. Well, no,
// that's just k, but..
struct Event {
  Repeat r;
  InterfaceKey k;
  int repeats;  // Starts at 0, increments once per repeat
  int serial;
  int time;
  int tick;  // The sim-tick at which we last returned this event
  bool macro;  // Created as part of macro playback.

  bool operator== (const Event &other) const {
    if (r != other.r) return false;
    if (k != other.k) return false;
    if (repeats != other.repeats) return false;
    if (serial != other.serial) return false;
    if (time != other.time) return false;
    if (macro != other.macro) return false;
    return true;
  }

  // We sort by time first, and then serial number.
  // The order of the other bits is unimportant.
  bool operator< (const Event &o) const {
    if (time != o.time) return time < o.time;
    if (serial != o.serial) return serial < o.serial;
    if (r != o.r) return r < o.r;
    if (k != o.k) return k < o.k;
    if (repeats != o.repeats) return repeats < o.repeats;
    if (macro != o.macro) return macro < o.macro;
    return false;
  }
};

// Used to decide which key-binding to display. As a heuristic, we
// prefer whichever display string is shortest.
struct less_sz {
  bool operator() (const string &a, const string &b) const {
    if (a.size() < b.size()) return true;
    if (a.size() > b.size()) return false;
    return a < b;
  }
};

// These change dynamically in the normal process of DF
static int last_serial = 0; // Input serial number, to differentiate distinct physical presses
static set<Event> timeline; // A timeline of pending key events (for next get_input)
static set<EventMatch> pressed_keys; // Keys we consider "pressed"
static int modState; // Modifier state
  
// These do not change as part of the normal dynamics of DF, only at startup/when editing.
static multimap<EventMatch,InterfaceKey> keymap;
static map<InterfaceKey,Repeat> repeatmap;
static map<InterfaceKey,set<string,less_sz> > keydisplay; // Used only for display, not for meaning

// Macro recording
static bool macro_recording = false;
static macro active_macro; // Active macro
static map<string,macro> macros;
static Time macro_end = 0; // Time at which the currently playing macro will end

// Prefix command state
static bool in_prefix_command = false;
static string prefix_command;

// Keybinding editing
static bool key_registering = false;
static list<EventMatch> stored_keys;

// Returns an unused serial number
static Time next_serial() {
  return ++last_serial;
}

static void update_keydisplay(InterfaceKey binding, string display) {
  // Need to filter out space/tab, for obvious reasons.
  if (display == " ") display = "Space";
  if (display == "\t") display = "Tab";
  map<InterfaceKey,set<string,less_sz> >::iterator it = keydisplay.find(binding);
  if (it == keydisplay.end()) {
    set<string,less_sz> s; s.insert(display);
    keydisplay[binding] = s;
  } else {
    keydisplay[binding].insert(display);
  }
}

static void assertgood(ifstream &s) {
  if (s.eof())
    MessageBox(NULL, L"EOF while parsing keyboard bindings", 0, 0);
  else if (!s.good())
    MessageBox(NULL, L"I/O error while parsing keyboard bindings", 0, 0);
  else
    return;
  abort();
}

// Decodes an UTF-8 encoded string into a /single/ UTF-8 character,
// discarding any overflow. Returns 0 on parse error.
int decode_utf8(const string &s) {
  int unicode = 0, length, i;
  if (s.length() == 0) return 0;
  length = decode_utf8_predict_length(s[0]);
  switch (length) {
  case 1: unicode = s[0]; break;
  case 2: unicode = s[0] & 0x1f; break;
  case 3: unicode = s[0] & 0x0f; break;
  case 4: unicode = s[0] & 0x07; break;
  default: return 0;
  }

  // Concatenate the follow-up bytes
  if (s.length() < length) return 0;
  for (i = 1; i < length; i++) {
    if ((s[i] & 0xc0) != 0x80) return 0;
    unicode = (unicode << 6) | (s[i] & 0x3f);
  }
  return unicode;
}

// Returns the length of an utf-8 sequence, based on its first byte
int decode_utf8_predict_length(char byte) {
  if ((byte & 0x80) == 0) return 1;
  if ((byte & 0xe0) == 0xc0) return 2;
  if ((byte & 0xf0) == 0xe0) return 3;
  if ((byte & 0xf8) == 0xf0) return 4;
  return 0; // Invalid start byte
}

// Encode an arbitrary unicode value as a string. Returns an empty
// string if the value is out of range.
string encode_utf8(int unicode) {
  string s;
  int i;
  if (unicode < 0 || unicode > 0x10ffff) return ""; // Out of range for utf-8
  else if (unicode <= 0x007f) { // 1-byte utf-8
    s.resize(1, 0);
  }
  else if (unicode <= 0x07ff) { // 2-byte utf-8
    s.resize(2, 0);
    s[0] = (unsigned char)0xc0;
  }
  else if (unicode <= 0xffff) { // 3-byte utf-8
    s.resize(3, 0);
    s[0] = (unsigned char)0xe0;
  }
  else { // 4-byte utf-8
    s.resize(4, 0);
    s[0] = (unsigned char)0xf0;
  }

  // Build up the string, right to left
  for (i = (int)s.length()-1; i > 0; i--) {
    s[i] = (unsigned char)(0x80 | (unicode & 0x3f));
    unicode >>= 6;
  }
  // Finally, what's left goes in the low bits of s[0]
  s[0] |= unicode;
  return s;
}

string translate_mod(Uint8 mod) {
  string ret;
  if (mod & 1) ret += "Shift+";
  if (mod & 2) ret += "Ctrl+";
  if (mod & 4) ret += "Alt+";
  return ret;
}


static string display(const EventMatch &match) {
    static const std::map<SDL_Keycode,string> capitals={ {SDLK_a, "A"},
        {SDLK_b, "B"},{SDLK_c, "C"},{SDLK_d, "D"},{SDLK_e, "E"},{SDLK_f, "F"},
        {SDLK_g, "G"},{SDLK_h, "H"},{SDLK_i, "I"},{SDLK_j, "J"},{SDLK_k, "K"},
        {SDLK_l, "L"},{SDLK_m, "M"},{SDLK_n, "N"},{SDLK_o, "O"},{SDLK_p, "P"},
        {SDLK_q, "Q"},{SDLK_r, "R"},{SDLK_s, "S"},{SDLK_t, "T"},{SDLK_u, "U"},
        {SDLK_v, "V"},{SDLK_w, "W"},{SDLK_x, "X"},{SDLK_y, "Y"},{SDLK_z, "Z"},
        };
  ostringstream ret;
  bool is_letter=false;
  if (match.type==type_key&&(match.mod&1)&&capitals.contains(match.key))
      {
      is_letter=true;
      if (match.mod==1)
          {
          return capitals.find(match.key)->second;
          }
      }
  ret << translate_mod(is_letter?(match.mod&~1):match.mod);
  switch (match.type) {
  case type_key: {
    map<SDL_Keycode,string>::iterator it = sdlNames.left.find(match.key);
    if (it != sdlNames.left.end())
      ret << it->second;
    else
      ret << "SDL+" << (int)match.key;
    break;
  }
  case type_button:
    ret << "Button " << (int)match.button;
    break;
  case type_mwheel:
      ret << "Mousewheel " << (match.y > 0 ? "up" : "down");
      break;
  }
  return ret.str();
}

static string translate_repeat(Repeat r) {
  switch (r) {
  case REPEAT_NOT: return "REPEAT_NOT";
  case REPEAT_SLOW: return "REPEAT_SLOW";
  case REPEAT_FAST: return "REPEAT_FAST";
  default: return "REPEAT_BROKEN";
  }
}

// Update the modstate, since SDL_getModState doesn't /work/ for alt
static void update_modstate(const SDL_Event &e) {
  if (e.type == SDL_KEYUP) {
    switch (e.key.keysym.sym) {
    case SDLK_RSHIFT:
    case SDLK_LSHIFT:
      modState &= ~1;
      break;
    case SDLK_RCTRL:
    case SDLK_LCTRL:
      modState &= ~2;
      break;
    case SDLK_RALT:
    case SDLK_LALT:
      modState &= ~4;
      break;
    }
  } else if (e.type == SDL_KEYDOWN) {
    switch (e.key.keysym.sym) {
    case SDLK_RSHIFT:
    case SDLK_LSHIFT:
      modState |= 1;
      break;
    case SDLK_RCTRL:
    case SDLK_LCTRL:
      modState |= 2;
      break;
    case SDLK_RALT:
    case SDLK_LALT:
      modState |= 4;
      break;
    }
  }
}

// Converts SDL mod states to ours, collapsing left/right shift/alt/ctrl
Uint8 getModState() {
  return modState;
}

// Not sure what to call this, but it ain't using regexes.
static bool parse_line(const string &line, const string &regex, vector<string> &parts) {
  parts.clear();
  parts.push_back(line);
  for (int l = 0, r = 0; r < regex.length();) {
    switch (regex[r]) {
    case '*': // Read until ], : or the end of the line, but at least one character.
      {
        const int start = l;
        for (; l < line.length() && (l == start || (line[l] != ']' && line[l] != ':')); l++)
          ;
        parts.push_back(line.substr(start, l - start));
        r++;
      }
      break;
    default:
      if (line[l] != regex[r]) return false;
      r++; l++;
      break;
    }
  }
  // We've made it this far, clearly the string parsed
  return true;
}

void enabler_inputst::clear_keybindings()
{
    keymap.clear();
}

bool enabler_inputst::load_keybindings(const filest &file) {
  cout << "Loading bindings from " << file.path.string() << endl;
  ifstream s=file.to_ifstream();
  if (!s.good()) return false;

  list<string> lines;
  while (s.good()) {
    string line;
    getline(s, line);
    lines.push_back(line);
  }
  if(lines.size()==0)return false;
  static const string control_version("[VERSION:*]");
  static const string bind("[BIND:*:*]");
  static const string sym("[SYM:*:*]");
  static const string key("[KEY:*]");
  static const string button("[BUTTON:*:*]");
  static const string wheel("[MOUSEWHEEL:*:*]");

  list<string>::iterator line = lines.begin();
  vector<string> match;

  int version=0;

  while (line != lines.end()) {
    if (parse_line(*line,control_version,match))
        {
        version=std::stoi(match[1]);
        }
    if (parse_line(*line, bind, match)) {
      map<string,InterfaceKey>::iterator it = bindingNames.right.find(match[1]);
      if (it != bindingNames.right.end()) {

        InterfaceKey binding = it->second;

        //bail if these key is already accounted for - prefs is loaded first, default second
        bool already_have_key=false;
        for(auto it=keymap.begin();it!=keymap.end();++it)
            {
            if(it->second==binding)
                {
                already_have_key=true;
                break;
                }
            }
        if(already_have_key)
            {
            ++line;
            continue;
            }

        // Parse repeat data
        if (match[2] == "REPEAT_FAST")
          repeatmap[(InterfaceKey)binding] = REPEAT_FAST;
        else if (match[2] == "REPEAT_SLOW")
          repeatmap[(InterfaceKey)binding] = REPEAT_SLOW;
        else if (match[2] == "REPEAT_NOT")
          repeatmap[(InterfaceKey)binding] = REPEAT_NOT;
        else {
          repeatmap[(InterfaceKey)binding] = REPEAT_NOT;
          cout << "Broken repeat request: " << match[2] << endl;
        }
        ++line;
        // Add symbols/keys/buttons
        while (line != lines.end()) {
          EventMatch matcher;
          // SDL Keys
          if (parse_line(*line, sym, match)) {
            map<string,SDL_Keycode>::iterator it = sdlNames.right.find(match[2]);
            if (it != sdlNames.right.end()) {
              matcher.mod  = atoi(string(match[1]).c_str());
              matcher.type = type_key;
              matcher.key  = it->second;
              keymap.insert(pair<EventMatch,InterfaceKey>(matcher, (InterfaceKey)binding));
              update_keydisplay(binding, display(matcher));
            } else {
              cout << "Unknown SDLKey: " << match[2] << endl;
            }
            ++line;           
          } // Legacy unicode
          else if (parse_line(*line, key, match)) {
              auto unicode = decode_utf8(match[1]);
              bool capital = unicodeCapitals.count(unicode) > 0;
              if (capital) {
                  matcher.mod = 1;
                  unicode += 32;
              }
              else {
                  matcher.mod = 0;
              }
              map<int, SDL_Keycode>::iterator it = sdlUnicode.right.find(unicode);
              if (it != sdlUnicode.right.end()) {
                  matcher.type = type_key;
                  matcher.key = it->second;
                  keymap.insert(make_pair(matcher, (InterfaceKey)binding));
                  update_keydisplay(binding, display(matcher));
              } else {
                  cout << "Broken unicode: " << *line << endl;
              }
              ++line;
          } // Mouse buttons
          else if (parse_line(*line, button, match)) {
            matcher.type = type_button;
            string str = match[2];
            matcher.button = atoi(str.c_str());
            if (matcher.button) {
                if (version<1 && (matcher.button == 4 || matcher.button == 5)) {
                    matcher.type = type_mwheel;
                    bool up = matcher.button == 4;
                    matcher.y = up ? 1 : -1;
                }
                matcher.mod = atoi(string(match[1]).c_str());
                keymap.insert(pair<EventMatch, InterfaceKey>(matcher, (InterfaceKey)binding));
                update_keydisplay(binding, display(matcher));
            }
            else {
                cout << "Broken button (should be [BUTTON:#:#]): " << *line << endl;
            }
            ++line;
          }
          // Mousewheel
          else if (parse_line(*line, wheel, match)) {
              matcher.type = type_mwheel;
              string str = match[2];
              if (str == "UP" || str == "DOWN") {
                  matcher.y = str == "UP" ? 1 : -1;
                  matcher.mod = atoi(string(match[1]).c_str());
                  keymap.insert(pair<EventMatch, InterfaceKey>(matcher, (InterfaceKey)binding));
                  update_keydisplay(binding, display(matcher));
              }
              else {
                  cout << "Broken mousewheel (should be [MOUSEWHEEL:#:UP|DOWN]: " << *line << endl;
              }
              ++line;
          } else {
            break;
          }
        }
      } else {
        cout << "Unknown binding: " << match[1] << endl;
		++line;
      }
    } else {
      // Retry with next line
      ++line;
    }
  }
  
  return true;
}

void enabler_inputst::save_keybindings(const string &file) {
  cout << "Saving bindings to " << file << endl;
  std::filesystem::path temporary = std::filesystem::path(file + ".partial");
  ofstream s=filest(temporary).to_ofstream(); 
  multimap<InterfaceKey,EventMatch> map;
  InterfaceKey last_key = INTERFACEKEY_NONE;

  if (!s.good()) {
    wstring t = L"Failed to open " + temporary.wstring() + L" for writing";
    if (!init.media.flag.has_flag(INIT_MEDIA_FLAG_PORTABLE_MODE))
        {
        t+=L"; switching to portable mode to compensate";
        init.media.flag.add_flag(INIT_MEDIA_FLAG_PORTABLE_MODE);
        }
    MessageBox(NULL, t.c_str(), 0, 0);
    s.close();
    return;
  }
  // Invert keyboard map
  for (multimap<EventMatch,InterfaceKey>::iterator it = keymap.begin(); it != keymap.end(); ++it)
    map.insert(pair<InterfaceKey,EventMatch>(it->second,it->first));
  // Insert an empty line for the benefit of note/wordpad
  s << endl;
    
  // INPUT KEYBINDINGS VERSION--UPDATE THIS IF ANYTHING NEEDS MIGRATED
  s<<"[VERSION:1]"<<endl;
  // And write.
  for (multimap<InterfaceKey,EventMatch>::iterator it = map.begin(); it != map.end(); ++it) {
    if (!s.good()) {
      MessageBox(NULL, L"I/O error while writing keyboard mapping", 0, 0);
      s.close();
      return;
    }
    if (it->first != last_key) {
      last_key = it->first;
      s << "[BIND:" << bindingNames.left[it->first] << ":"
        << translate_repeat(repeatmap[it->first]) << "]" << endl;
    }
    switch (it->second.type) {
    case type_key:
      s << "[SYM:" << (int)it->second.mod << ":" << sdlNames.left[it->second.key] << "]" << endl;
      break;
    case type_button:
      s << "[BUTTON:" << (int)it->second.mod << ":" << (int)it->second.button << "]" << endl;
      break;
    case type_mwheel:
        s << "[MOUSEWHEEL:" << (int)it->second.mod << (it->second.y > 0 ? ":UP]" : ":DOWN]") << endl;
      break;
    }
      
  }
  s.close();
  replace_file(filest(temporary), filest(file));
}

void enabler_inputst::save_keybindings() {
  save_keybindings("prefs/interface.txt");
}

void enabler_inputst::add_input(SDL_Event &e, Uint32 now) {
  if (handle_mouse_emu_input_sdl(e, now)) return;
  // Before we can use this input, there are some issues to deal with:
  // - SDL provides unicode translations only for key-press events, not
  //   releases. We need to keep track of pressed keys, and generate
  //   unicode release events whenever any modifiers are hit, or if
  //   that raw keycode is released.
  // - Generally speaking, when modifiers are hit/released, we discard those
  //   events and generate press/release events for all pressed non-modifiers.
  // - It's possible for multiple events to be generated on the same tick.
  //   These are of course separate keypresses, and must be kept separate.
  //   That's what the serial is for.

  set<EventMatch>::iterator pkit;
  list<pair<KeyEvent, int> > synthetics;
  update_modstate(e);
  
  // Convert modifier state changes
  if ((e.type == SDL_KEYUP || e.type == SDL_KEYDOWN) &&
      (e.key.keysym.sym == SDLK_RSHIFT ||
       e.key.keysym.sym == SDLK_LSHIFT ||
       e.key.keysym.sym == SDLK_RCTRL  ||
       e.key.keysym.sym == SDLK_LCTRL  ||
       e.key.keysym.sym == SDLK_RALT   ||
       e.key.keysym.sym == SDLK_LALT   )) {
    for (pkit = pressed_keys.begin(); pkit != pressed_keys.end(); ++pkit) {
      // Release currently pressed keys
      KeyEvent synth;
      synth.release = true;
      synth.match = *pkit;
      synthetics.push_back(make_pair(synth, next_serial()));
      // Re-press them, with new modifiers, if they aren't unicode. We can't re-translate unicode.
    synth.release = false;
    synth.match.mod = getModState();
    if (!key_registering) // We don't want extras when registering keys
        synthetics.push_back(make_pair(synth, next_serial()));
    }
  } else {
    
    // Since it's not a modifier, we also pass on symbolic/button
    // (always) and unicode (if defined) events
    //
    // However, since SDL ignores(?) ctrl and alt when translating to
    // unicode, we want to ignore unicode events if those are set.
    const int serial = next_serial();

    KeyEvent real;
    real.release = (e.type == SDL_KEYUP || e.type == SDL_MOUSEBUTTONUP) ? true : false;
    real.match.mod = getModState();
    switch (e.type) {
        case SDL_MOUSEWHEEL:
            real.match.type = type_mwheel;
            real.match.scancode = 0;
            real.match.y = e.wheel.y * ((e.wheel.direction == SDL_MOUSEWHEEL_NORMAL) ? 1 : -1);
            synthetics.push_back(make_pair(real, serial));
            break;
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEBUTTONDOWN:
            real.match.type = type_button;
            real.match.scancode = 0;
            real.match.button = e.button.button;
            synthetics.push_back(make_pair(real, serial));
            break;
        case SDL_KEYUP:
        case SDL_KEYDOWN:
            real.match.type = type_key;
            real.match.scancode = e.key.keysym.scancode;
            real.match.key = e.key.keysym.sym;
            synthetics.push_back(make_pair(real, serial));
            break;
        case SDL_QUIT:
            // This one, we insert directly into the timeline.
            Event e = { REPEAT_NOT, (InterfaceKey)INTERFACEKEY_OPTIONS, 0, (int)next_serial(), (int)now, 0 };
            timeline.insert(e);
    }
  }

  list<pair<KeyEvent, int> >::iterator lit;
  for (lit = synthetics.begin(); lit != synthetics.end(); ++lit) {
    // Add or remove the key from pressed_keys, keeping that up to date
    if (lit->first.release) pressed_keys.erase(lit->first.match);
    else pressed_keys.insert(lit->first.match);
    // And pass the event on deeper.
    add_input_refined(lit->first, now, lit->second);
  }
}

// Input encoding:
// 1 and up are ncurses symbols, as returned by getch.
// -1 and down are unicode values.
// esc is true if this key was part of an escape sequence.
#ifdef CURSES
void enabler_inputst::add_input_ncurses(int key, Time now, bool esc) {
  EventMatch sdl;
  const int serial = next_serial();
  sdl.type = type_key;
  sdl.scancode = 0;
  sdl.mod = 0;
  sdl.key = SDLK_UNKNOWN;

  if (esc) {
    sdl.mod = DFMOD_ALT;
  }

  if (key == -10) { // Return
    sdl.key = SDLK_RETURN;
  } else if (key == -9) { // Tab
    sdl.key = SDLK_TAB;
  } else if (key == -27) {
    sdl.key = SDLK_ESCAPE;
  } else if (key == -127) { // Backspace/del
    sdl.key = SDLK_BACKSPACE;
  } else if (key < 0 && key >= -26) { // Control-a through z (but not ctrl-j, or ctrl-i)
    sdl.mod |= DFMOD_CTRL;
    sdl.key = (SDL_Keycode)(SDLK_a + (-key) - 1);
  } else if (key <= -32 && key >= -126) { // ASCII character set
    sdl.key = (SDL_Keycode)-key;
    if (sdl.key > 64 && sdl.key < 91) { // Uppercase
      sdl.key = (SDL_Keycode)(sdl.key + 32);
      sdl.mod |= DFMOD_SHIFT;
    }
  } else if (key > 0) { // Symbols such as arrow-keys, etc.
    switch (key) {
    case KEY_DOWN: sdl.key = SDLK_DOWN; break;
    case KEY_UP: sdl.key = SDLK_UP; break;
    case KEY_LEFT: sdl.key = SDLK_LEFT; break;
    case KEY_RIGHT: sdl.key = SDLK_RIGHT; break;
    case KEY_BACKSPACE: sdl.key = SDLK_BACKSPACE; break;
    case KEY_F(1): sdl.key = SDLK_F1; break;
    case KEY_F(2): sdl.key = SDLK_F2; break;
    case KEY_F(3): sdl.key = SDLK_F3; break;
    case KEY_F(4): sdl.key = SDLK_F4; break;
    case KEY_F(5): sdl.key = SDLK_F5; break;
    case KEY_F(6): sdl.key = SDLK_F6; break;
    case KEY_F(7): sdl.key = SDLK_F7; break;
    case KEY_F(8): sdl.key = SDLK_F8; break;
    case KEY_F(9): sdl.key = SDLK_F9; break;
    case KEY_F(10): sdl.key = SDLK_F10; break;
    case KEY_F(11): sdl.key = SDLK_F11; break;
    case KEY_F(12): sdl.key = SDLK_F12; break;
    case KEY_F(13): sdl.key = SDLK_F13; break;
    case KEY_F(14): sdl.key = SDLK_F14; break;
    case KEY_F(15): sdl.key = SDLK_F15; break;
    case KEY_DC: sdl.key = SDLK_DELETE; break;
    case KEY_NPAGE: sdl.key = SDLK_PAGEDOWN; break;
    case KEY_PPAGE: sdl.key = SDLK_PAGEUP; break;
    case KEY_ENTER: sdl.key = SDLK_RETURN; break;
    }
  }

  if (key_registering) {
    if (sdl.key) {
      stored_keys.push_back(sdl);
    }
    Event e;
    e.r = REPEAT_NOT;
    e.repeats = 0;
    e.time = now;
    e.serial = serial;
    e.k = INTERFACEKEY_KEYBINDING_COMPLETE;
    e.tick = enabler.simticks;
    e.macro = false;
    timeline.insert(e);
    key_registering = false;
    return;
  }

  // Inject text input event if applicable (for search filters, naming screens, etc)
  if (key < 0 && key >= -1114111) {
      int unicode = -key;
      if (unicode >= 32 && unicode != 127) {
          string utf8 = encode_utf8(unicode);
          SDL_Event ev;
          memset(&ev, 0, sizeof(ev));
          ev.type = SDL_TEXTINPUT;
          strncpy(ev.text.text, utf8.c_str(), 31);
          ev.text.text[31] = '\0';
          enabler.set_text_input(ev);
      }
  }

  if (sdl.key) {
    set<InterfaceKey> events = key_translation(sdl);
    for (set<InterfaceKey>::iterator k = events.begin(); k != events.end(); ++k) {
      Event e;
      e.r = REPEAT_NOT;
      e.repeats = 0;
      e.time = now;
      e.serial = serial;
      e.k = *k;
      e.tick = enabler.simticks;
      e.macro = false;
      timeline.insert(e);
    }
  }
}
#endif

void enabler_inputst::add_input_refined(KeyEvent &e, Uint32 now, int serial) {
  // We may be registering a new mapping, in which case we skip the
  // rest of this function.
  if (key_registering && !e.release) {
    stored_keys.push_back(e.match);
    Event e; e.r = REPEAT_NOT; e.repeats = 0; e.time = now; e.serial = serial; e.k = INTERFACEKEY_KEYBINDING_COMPLETE; e.tick = enabler.simticks;
    timeline.insert(e);
    return;
  }

  // If this is a key-press event, we add it to the timeline. If it's
  // a release, we remove any pending repeats, but not those that
  // haven't repeated yet (which are on their first cycle); those we
  // just set to non-repeating.
  set<InterfaceKey> keys = key_translation(e.match);
  if (e.release) {
    set<Event>::iterator it = timeline.begin();
    while (it != timeline.end()) {
      set<Event>::iterator el = it++;
      if (keys.count(el->k)) {
        if (el->repeats) {
          timeline.erase(el);
        } else {
          Event new_el = *el;
          new_el.r = REPEAT_NOT;
          timeline.erase(el);
          timeline.insert(new_el);
        }
      }
    }
  } else {
    set<InterfaceKey>::iterator key;
    // As policy, when the user hits a non-repeating key we'd want to
    // also cancel any keys that are currently repeating. This allows
    // for easy recovery from stuck keys.
    //
    // Unfortunately, each key may be bound to multiple
    // commands. So, lacking information on which commands are
    // accepted at the moment, there is no way we can know if it's
    // okay to cancel repeats unless /all/ the bindings are
    // non-repeating.
    for (set<InterfaceKey>::iterator k = keys.begin(); k != keys.end(); ++k) {
      bool is_mousewheel = e.match.type == type_mwheel;
      Event ev = {is_mousewheel ? REPEAT_MWHEEL : key_repeat(*k), *k, is_mousewheel ? abs(e.match.y) : 0, serial, (int)now, enabler.simticks};
      timeline.insert(ev);
    }
    // if (cancel_ok) {
    //   // Set everything on the timeline to non-repeating
    //   multimap<Time,Event>::iterator it;
    //   for (it = timeline.begin(); it != timeline.end(); ++it) {
    //     it->second.r = REPEAT_NOT;
    //   }
  }
}


void enabler_inputst::clear_input() {
  timeline.clear();
  pressed_keys.clear();
  modState = 0;
  last_serial = 0;
}

set<InterfaceKey> enabler_inputst::get_input(Time now) {
  // We walk the timeline, returning all events corresponding to a
  // single physical keypress, and inserting repeats relative to the
  // current time, not when the events we're now returning were
  // *supposed* to happen.

  set<InterfaceKey> input;
  set<Event>::iterator ev = timeline.begin();
  if (ev == timeline.end() || ev->time > now) {
    return input; // No input (yet).
  }

  const Time first_time = ev->time;
  const int first_serial = ev->serial;
  int simtick = enabler.simticks;
  bool event_from_macro = false;
  while (ev != timeline.end() && ev->time == first_time && ev->serial == first_serial) {
    // Avoid recording macro-sources events as macro events.
    if (ev->macro) event_from_macro = true;
    // To make sure the user had a chance to cancel (by lifting the key), we require there
    // to be at least three simulation ticks before the first repeat.
    if (ev->repeats == 1 && ev->tick > simtick - 3) {
    } else {
      input.insert(ev->k);
    }
    // Schedule a repeat
    Event next = *ev;
    switch (next.r) {
    case REPEAT_NOT:
      next.repeats++;
      break;
    case REPEAT_SLOW:
      next.repeats++;
      if (ev->repeats == 0) {
        next.time = now + init.input.hold_time;
        timeline.insert(next);
        break;
      }
    case REPEAT_FAST:
    {
        next.repeats++;
        double accel = 1;
        if (ev->repeats >= init.input.repeat_accel_start) {
            // Compute acceleration
            accel = MIN(init.input.repeat_accel_limit,
                sqrt(double(next.repeats - init.input.repeat_accel_start) + 16) - 3);
        }
        next.time = now + double(init.input.repeat_time) / accel;
        timeline.insert(next);
        break;
    }
    case REPEAT_MWHEEL:
        --next.repeats;
        if (ev->repeats > 0) {
            next.time = now + 1;
            timeline.insert(next);
            }
        else // gotta remove mouse wheel events now
            {
            std::erase_if(pressed_keys,[](const auto &k) -> bool
                {
                return k.type==type_mwheel;
                });
            }
        break;
    }
    // Delete the event from the timeline and iterate
    timeline.erase(ev++);
  }
#ifdef DEBUG
  if (input.size() && !init.display.flag.has_flag(INIT_DISPLAY_FLAG_TEXT)) {
    cout << "Returning input:\n";
    set<InterfaceKey>::iterator it;
    for (it = input.begin(); it != input.end(); ++it)
        cout << "    " << GetKeyDisplay(*it) << ": " << GetBindingDisplay(*it) << endl;
  }
#endif
  // It could be argued that the "record event" step of recording
  // belongs in add_input, not here.  I don't hold with this
  // argument. The whole point is to record events as the user seems
  // them happen.
  if (macro_recording && !event_from_macro) {
    set<InterfaceKey> macro_input = input;
    macro_input.erase(INTERFACEKEY_RECORD_MACRO);
    macro_input.erase(INTERFACEKEY_PLAY_MACRO);
    macro_input.erase(INTERFACEKEY_SAVE_MACRO);
    macro_input.erase(INTERFACEKEY_LOAD_MACRO);
    if (macro_input.size())
      active_macro.push_back(macro_input);
  }
  return input;
}

set<InterfaceKey> enabler_inputst::key_translation(EventMatch &match) {
  set<InterfaceKey> bindings;
  pair<multimap<EventMatch,InterfaceKey>::iterator,multimap<EventMatch,InterfaceKey>::iterator> its;
  if (match.type == type_mwheel) {
      int orig_y = match.y;
      match.y=0;
      if (orig_y > 0) {
          match.y = 1;
      } else if (orig_y < 0) {
          match.y = -1;
      }
      for (its = keymap.equal_range(match); its.first != its.second; ++its.first)
          bindings.insert((its.first)->second);
      match.y = orig_y;
  }
  else {
      for (its = keymap.equal_range(match); its.first != its.second; ++its.first)
          bindings.insert((its.first)->second);
  }

  return bindings;
}

string enabler_inputst::GetKeyDisplay(int binding) {
  map<InterfaceKey,set<string,less_sz> >::iterator it = keydisplay.find(binding);
  if (it != keydisplay.end() && it->second.size())
    return *it->second.begin();
  else {
    cout << "Missing binding displayed: " + bindingNames.left[binding] << endl;
    return "?";
  }
}

string enabler_inputst::GetBindingDisplay(int binding) {
  map<InterfaceKey,string>::iterator it = bindingNames.left.find(binding);
  if (it != bindingNames.left.end())
    return it->second;
  else
    return "NO BINDING";
}

string enabler_inputst::GetBindingTextDisplay(int binding) {
  map<InterfaceKey,string>::iterator it = displayNames.left.find(binding);
  if (it !=displayNames.left.end())
    return it->second;
  else
    return "NO BINDING";
}

Repeat enabler_inputst::key_repeat(InterfaceKey binding) {
  map<InterfaceKey,Repeat>::iterator it = repeatmap.find(binding);
  if (it != repeatmap.end())
    return it->second;
  else
    return REPEAT_NOT;
}

void enabler_inputst::key_repeat(InterfaceKey binding, Repeat repeat) {
  repeatmap[binding] = repeat;
}

void enabler_inputst::record_input() {
  active_macro.clear();
  macro_recording = true;
} 

void enabler_inputst::record_stop() {
  macro_recording = false;
}

bool enabler_inputst::is_recording() {
  return macro_recording;
}

void enabler_inputst::play_macro() {
  Time now = SDL_GetTicks();
  for_each(timeline.begin(), timeline.end(), [&](Event e){
      now = MAX(now, e.time);
    });
  for (macro::iterator sim = active_macro.begin(); sim != active_macro.end(); ++sim) {
    Event e; e.r = REPEAT_NOT; e.repeats = 0; e.serial = next_serial(); e.time = now;
    e.macro = true;  // Avoid exponential macro blowup.
    for (set<InterfaceKey>::iterator k = sim->begin(); k != sim->end(); ++k) {
      e.k = *k;
      timeline.insert(e);
      now += init.input.macro_time;
    }
  }
  macro_end = MAX(macro_end, now);
}

bool enabler_inputst::is_macro_playing() {
  return SDL_GetTicks() <= macro_end;
}

// Replaces any illegal letters.
static string filter_filename(string name, char replacement) {
  for (int i = 0; i < name.length(); i++) {
    switch (name[i]) {
    case '<': name[i] = replacement; break;
    case '>': name[i] = replacement; break;
    case ':': name[i] = replacement; break;
    case '"': name[i] = replacement; break;
    case '/': name[i] = replacement; break;
    case '\\': name[i] = replacement; break;
    case '|': name[i] = replacement; break;
    case '?': name[i] = replacement; break;
    case '*': name[i] = replacement; break;
    }
    if (name[i] <= 31) name[i] = replacement;
  }
  return name;
}

void enabler_inputst::load_macro_from_file(const string &file) {
  ifstream s(file.c_str());
  char buf[100];
  s.getline(buf, 100);
  string name(buf);
  if (macros.find(name) != macros.end()) return; // Already got it.

  macro macro;
  set<InterfaceKey> group;
  for(;;) {
    s.getline(buf, 100);
    if (!s.good()) {
      MessageBox(NULL, L"I/O error while loading macro", 0, 0);
      s.close();
      return;
    }
    string line(buf);
    if (line == "End of macro") {
      if (group.size()) macro.push_back(group);
      break;
    } else if (line == "\tEnd of group") {
      if (group.size()) macro.push_back(group);
      group.clear();
    } else if (line.substr(0,2) != "\t\t" ) {
      if( line.substr(1).find("\t") != string::npos) {
        // expecting /t##/tCMD for a repeated command
        istringstream ss(line.substr(1));
        int count;
        string remainingline;

        if(ss >> count) {
          ss >> remainingline;
          if(remainingline.size()) {
            for(int i=0; i < count; i++) {
              map<string,InterfaceKey>::iterator it = bindingNames.right.find(remainingline);
              if (it == bindingNames.right.end()) {
                cout << "Binding name unknown while loading macro: " << line.substr(1) << endl;
              } else {
                group.insert(it->second);
                if (group.size()) macro.push_back(group);
                group.clear();
              }
            }
          }
          else {
            cout << "Binding missing while loading macro: " << line.substr(1) << endl;
          }
        } else {
          cout << "Quantity not numeric or Unexpected tab(s) while loading macro: " << line.substr(1) << endl;
        }
      }
      else
      {
        // expecting /tCMD for a non-grouped command
        map<string,InterfaceKey>::iterator it = bindingNames.right.find(line.substr(1));
        if (it == bindingNames.right.end()) {
          cout << "Binding name unknown while loading macro: " << line.substr(1) << endl;
        } else {
          group.insert(it->second);
          if (group.size()) macro.push_back(group);
          group.clear();
        }
      }
    } else {
      map<string,InterfaceKey>::iterator it = bindingNames.right.find(line.substr(2));
      if (it == bindingNames.right.end())
        cout << "Binding name unknown while loading macro: " << line.substr(2) << endl;
      else
        group.insert(it->second);
    }
  }
  if (s.good())
    macros[name] = macro;
  else
    MessageBox(NULL, L"I/O error while loading macro", 0, 0);
  s.close();
}

void enabler_inputst::save_macro_to_file(const string &file, const string &name, const macro &macro) {
  ofstream s(file.c_str());
  s << name << endl;
  for (macro::const_iterator group = macro.begin(); group != macro.end(); ++group) {
    for (set<InterfaceKey>::const_iterator key = group->begin(); key != group->end(); ++key)
      s << "\t\t" << bindingNames.left[*key] << endl;
    s << "\tEnd of group" << endl;
  }
  s << "End of macro" << endl;
  s.close();
}

list<string> enabler_inputst::list_macros() {
  // First, check for unloaded macros
  for (auto &dir : filest("prefs/macros").both_locations())
      {
      std::error_code ec;
      for (auto &dir_entry : std::filesystem::recursive_directory_iterator(dir,ec))
          {
          if (dir_entry.path().extension()==".mak")
              {
              load_macro_from_file(dir_entry.path().string());
              }
          }
      }
  // Then return all in-memory macros
  list<string> ret;
  for (map<string,macro>::iterator it = macros.begin(); it != macros.end(); ++it)
    ret.push_back(it->first);
  return ret;
}

void enabler_inputst::load_macro(string name) {
  if (macros.find(name) != macros.end())
    active_macro = macros[name];
  else
    macros.clear();
}

void enabler_inputst::save_macro(string name) {
  macros[name] = active_macro;
  auto macros_dir=filest("prefs/macros").canon_location();
  std::filesystem::create_directories(macros_dir);
  save_macro_to_file((macros_dir/(filter_filename(name,'_')+".mak")).string(),name,active_macro);
}

void enabler_inputst::delete_macro(string name) {
  map<string,macro>::iterator it = macros.find(name);
  if (it != macros.end()) macros.erase(it);
  // TODO: Store the filename it was loaded from instead
  // How long has that TODO been there?
  remove_file(std::filesystem::path("prefs/macros")/(filter_filename(name,'_')+".mak"));
}


// Sets the next key-press to be stored instead of executed.
void enabler_inputst::register_key() {
  key_registering = true;
  stored_keys.clear();
}

void enabler_inputst::stop_registering_key() {
  key_registering = false;
}

// Returns a description of stored keys. Max one of each type.
std::list<RegisteredKey> enabler_inputst::getRegisteredKey() {
  key_registering = false;
  std::list<RegisteredKey> ret;
  for (list<EventMatch>::iterator it = stored_keys.begin(); it != stored_keys.end(); ++it) {
    struct RegisteredKey r = {it->type, display(*it)};
    ret.push_back(r);
  }
  return ret;
}

// Binds one of the stored keys to key
void enabler_inputst::bindRegisteredKey(MatchType type, InterfaceKey key) {

    //2022-08-29: this loop fails if there's no type match, so we add something as needed
    bool added=false;
  for (list<EventMatch>::iterator it = stored_keys.begin(); it != stored_keys.end(); ++it) {
    if (it->type == type) {
      keymap.insert(pair<EventMatch,InterfaceKey>(*it, key));
      update_keydisplay(key, display(*it));
      added=true;
    }
  }
  if(!added&&stored_keys.size()>0)
    {
      keymap.insert(pair<EventMatch,InterfaceKey>(*stored_keys.begin(), key));
      update_keydisplay(key, display(*stored_keys.begin()));
    }
}

bool enabler_inputst::is_registering() {
  return key_registering;
}


list<EventMatch> enabler_inputst::list_keys(InterfaceKey key) {
  list<EventMatch> ret;
  // Oh, now this is inefficient.
  for (multimap<EventMatch,InterfaceKey>::iterator it = keymap.begin(); it != keymap.end(); ++it)
    if (it->second == key) ret.push_back(it->first);
  return ret;
}

void enabler_inputst::remove_key(InterfaceKey key, EventMatch ev) {
  for (multimap<EventMatch,InterfaceKey>::iterator it = keymap.find(ev);
       it != keymap.end() && it->first == ev;
       ++it) {
    if (it->second == key) keymap.erase(it++);
  }
  // Also remove the key from key displaying, assuming we can find it
  map<InterfaceKey,set<string,less_sz> >::iterator it = keydisplay.find(key);
  if (it != keydisplay.end())
    it->second.erase(display(ev));
}

bool enabler_inputst::prefix_building() {
  return in_prefix_command;
}

void enabler_inputst::prefix_toggle() {
  in_prefix_command = !in_prefix_command;
  prefix_command.clear();
}

void enabler_inputst::prefix_add_digit(char digit) {
  prefix_command.push_back(digit);
#ifdef DEBUG
  cout << "Built prefix to " << prefix_command << endl;
#endif
  if (atoi(prefix_command.c_str()) > 99)
    prefix_command = "99";  // Let's not go overboard here.
}

int enabler_inputst::prefix_end() {
  if (prefix_command.size()) {
    int repeats = atoi(prefix_command.c_str());
    prefix_toggle();
    return repeats;
  } else {
    return 1;
  }
}

string enabler_inputst::prefix() {
  return prefix_command;
}

MouseEmulationState g_mouse_emu;
std::map<EmuKeyBind, EmuAction> emu_bindings;

void trigger_approve() {
    g_mouse_emu.click_state = MouseEmulationState::CLICK_DOWN;
}

void move_mouse_emu(int dx, int dy) {
    g_mouse_emu.cur_x = CLAMP(g_mouse_emu.cur_x + dx, 0, init.display.grid_x - 1);
    g_mouse_emu.cur_y = CLAMP(g_mouse_emu.cur_y + dy, 0, init.display.grid_y - 1);
}

void update_mouse_emulation() {
    if (!g_mouse_emu.enabled) return;

    enabler.tracking_on = 1;
    gps.mouse_x = g_mouse_emu.cur_x;
    gps.mouse_y = g_mouse_emu.cur_y;

    if (g_mouse_emu.click_state == MouseEmulationState::CLICK_DOWN) {
        enabler.mouse_lbut = 1;
        enabler.mouse_lbut_down = 1;
        enabler.mouse_lbut_lift = 0;
        g_mouse_emu.click_state = MouseEmulationState::CLICK_UP;
    } else if (g_mouse_emu.click_state == MouseEmulationState::CLICK_UP) {
        enabler.mouse_lbut = 0;
        enabler.mouse_lbut_down = 0;
        enabler.mouse_lbut_lift = 1;
        g_mouse_emu.click_state = MouseEmulationState::CLICK_NONE;
    } else {
        if (enabler.mouse_lbut_lift) {
            enabler.mouse_lbut_lift = 0;
        }
    }

    if (g_mouse_emu.right_click_state == MouseEmulationState::CLICK_DOWN) {
        enabler.mouse_rbut = 1;
        enabler.mouse_rbut_down = 1;
        enabler.mouse_rbut_lift = 0;
        g_mouse_emu.right_click_state = MouseEmulationState::CLICK_UP;
    } else if (g_mouse_emu.right_click_state == MouseEmulationState::CLICK_UP) {
        enabler.mouse_rbut = 0;
        enabler.mouse_rbut_down = 0;
        enabler.mouse_rbut_lift = 1;
        g_mouse_emu.right_click_state = MouseEmulationState::CLICK_NONE;
    } else {
        if (enabler.mouse_rbut_lift) {
            enabler.mouse_rbut_lift = 0;
        }
    }

    if (g_mouse_emu.middle_click_state == MouseEmulationState::CLICK_DOWN) {
        enabler.mouse_mbut = 1;
        enabler.mouse_mbut_down = 1;
        enabler.mouse_mbut_lift = 0;
        g_mouse_emu.middle_click_state = MouseEmulationState::CLICK_UP;
    } else if (g_mouse_emu.middle_click_state == MouseEmulationState::CLICK_UP) {
        enabler.mouse_mbut = 0;
        enabler.mouse_mbut_down = 0;
        enabler.mouse_mbut_lift = 1;
        g_mouse_emu.middle_click_state = MouseEmulationState::CLICK_NONE;
    } else {
        if (enabler.mouse_mbut_lift) {
            enabler.mouse_mbut_lift = 0;
        }
    }
}

EmuKeyBind parse_emu_key_string(std::string str) {
    EmuKeyBind bind;
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("ctrl+") != std::string::npos) {
        bind.ctrl = true;
    }
    if (lower.find("alt+") != std::string::npos) {
        bind.alt = true;
    }
    if (lower.find("shift+") != std::string::npos) {
        bind.shift = true;
    }

    size_t last_plus = str.find_last_of('+');
    std::string key_name = (last_plus == std::string::npos) ? str : str.substr(last_plus + 1);

    auto it = sdlNames.right.find(key_name);
    if (it != sdlNames.right.end()) {
        bind.key = it->second;
    } else {
        if (key_name.length() == 1) {
            char c = key_name[0];
            if (c >= 'A' && c <= 'Z') {
                c = c - 'A' + 'a';
                bind.shift = true;
            }
            bind.key = (SDL_Keycode)c;
        }
    }
    return bind;
}

void write_default_mouse_emu_config(const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) return;
    out << "# Dwarf Fortress Mouse Emulation Config\n";
    out << "# Format: [BIND_ACTION:ACTION_NAME:KeyCombo]\n\n";
    out << "[BIND_ACTION:TOGGLE_MOUSE_MODE:Ctrl+m]\n";
    out << "[BIND_ACTION:TOGGLE_MOUSE_MODE:F10]\n";
    out << "[BIND_ACTION:APPROVE:Enter]\n";
    out << "[BIND_ACTION:APPROVE:Ctrl+Space]\n";
    out << "[BIND_ACTION:RIGHT_CLICK:Ctrl+r]\n";
    out << "[BIND_ACTION:MIDDLE_CLICK:Ctrl+g]\n";
    out << "[BIND_ACTION:MOVE_N:Up]\n";
    out << "[BIND_ACTION:MOVE_N:8]\n";
    out << "[BIND_ACTION:MOVE_S:Down]\n";
    out << "[BIND_ACTION:MOVE_S:2]\n";
    out << "[BIND_ACTION:MOVE_W:Left]\n";
    out << "[BIND_ACTION:MOVE_W:4]\n";
    out << "[BIND_ACTION:MOVE_E:Right]\n";
    out << "[BIND_ACTION:MOVE_E:6]\n";
    out << "[BIND_ACTION:MOVE_NW:7]\n";
    out << "[BIND_ACTION:MOVE_NE:9]\n";
    out << "[BIND_ACTION:MOVE_SW:1]\n";
    out << "[BIND_ACTION:MOVE_SE:3]\n";
}

void load_mouse_emu_config() {
    filest config_file("data/init/mouse_emu.txt");
    auto any_loc = config_file.any_location();
    if (!any_loc) {
        write_default_mouse_emu_config(config_file.canon_location().string());
        any_loc = config_file.any_location();
    }
    if (!any_loc) return;

    std::ifstream s(any_loc.value().string());
    if (!s.good()) return;

    static const string bind_action("[BIND_ACTION:*:*]");
    vector<string> match;
    string line;

    while (getline(s, line)) {
        if (parse_line(line, bind_action, match)) {
            string act_name = match[1];
            string key_combo = match[2];

            EmuAction act = EMU_ACT_COUNT;
            if (act_name == "TOGGLE_MOUSE_MODE") act = EMU_ACT_TOGGLE_MOUSE;
            else if (act_name == "APPROVE") act = EMU_ACT_APPROVE;
            else if (act_name == "RIGHT_CLICK") act = EMU_ACT_RIGHT_CLICK;
            else if (act_name == "MIDDLE_CLICK") act = EMU_ACT_MIDDLE_CLICK;
            else if (act_name == "MOVE_N") act = EMU_ACT_MOVE_N;
            else if (act_name == "MOVE_NE") act = EMU_ACT_MOVE_NE;
            else if (act_name == "MOVE_E") act = EMU_ACT_MOVE_E;
            else if (act_name == "MOVE_SE") act = EMU_ACT_MOVE_SE;
            else if (act_name == "MOVE_S") act = EMU_ACT_MOVE_S;
            else if (act_name == "MOVE_SW") act = EMU_ACT_MOVE_SW;
            else if (act_name == "MOVE_W") act = EMU_ACT_MOVE_W;
            else if (act_name == "MOVE_NW") act = EMU_ACT_MOVE_NW;

            if (act != EMU_ACT_COUNT) {
                EmuKeyBind bind = parse_emu_key_string(key_combo);
                if (bind.key != SDLK_UNKNOWN) {
                    emu_bindings[bind] = act;
                }
            }
        }
    }
    s.close();
}

void execute_emu_action(EmuAction act) {
    switch (act) {
        case EMU_ACT_TOGGLE_MOUSE:
            g_mouse_emu.enabled = !g_mouse_emu.enabled;
            if (g_mouse_emu.enabled) {
                g_mouse_emu.cur_x = init.display.grid_x / 2;
                g_mouse_emu.cur_y = init.display.grid_y / 2;
            } else {
                enabler.mouse_lbut = 0;
                enabler.mouse_lbut_down = 0;
                enabler.mouse_lbut_lift = 0;
                enabler.mouse_rbut = 0;
                enabler.mouse_rbut_down = 0;
                enabler.mouse_rbut_lift = 0;
                enabler.mouse_mbut = 0;
                enabler.mouse_mbut_down = 0;
                enabler.mouse_mbut_lift = 0;
                enabler.tracking_on = 0;
            }
            break;
        case EMU_ACT_APPROVE:
            trigger_approve();
            break;
        case EMU_ACT_RIGHT_CLICK:
            g_mouse_emu.right_click_state = MouseEmulationState::CLICK_DOWN;
            break;
        case EMU_ACT_MIDDLE_CLICK:
            g_mouse_emu.middle_click_state = MouseEmulationState::CLICK_DOWN;
            break;
        case EMU_ACT_MOVE_N: move_mouse_emu(0, -1); break;
        case EMU_ACT_MOVE_NE: move_mouse_emu(1, -1); break;
        case EMU_ACT_MOVE_E: move_mouse_emu(1, 0); break;
        case EMU_ACT_MOVE_SE: move_mouse_emu(1, 1); break;
        case EMU_ACT_MOVE_S: move_mouse_emu(0, 1); break;
        case EMU_ACT_MOVE_SW: move_mouse_emu(-1, 1); break;
        case EMU_ACT_MOVE_W: move_mouse_emu(-1, 0); break;
        case EMU_ACT_MOVE_NW: move_mouse_emu(-1, -1); break;
        default: break;
    }
}

bool handle_mouse_emu_input_sdl(const SDL_Event &e, Uint32 now) {
    if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) return false;
    bool down = (e.type == SDL_KEYDOWN);

    EmuKeyBind match;
    match.key = e.key.keysym.sym;
    Uint16 mod = e.key.keysym.mod;
    match.ctrl = (mod & KMOD_CTRL);
    match.alt = (mod & KMOD_ALT);
    match.shift = (mod & KMOD_SHIFT);

    auto it = emu_bindings.find(match);
    if (it == emu_bindings.end()) {
        if (match.shift && match.key >= 'a' && match.key <= 'z') {
            EmuKeyBind match_no_shift = match;
            match_no_shift.shift = false;
            it = emu_bindings.find(match_no_shift);
        }
    }

    if (it != emu_bindings.end()) {
        if (down) {
            execute_emu_action(it->second);
        }
        return true;
    }

    return false;
}

bool handle_mouse_emu_input_ncurses(int key, bool esc) {
    EmuKeyBind match;
    match.ctrl = false;
    match.alt = esc;
    match.shift = false;
    match.key = SDLK_UNKNOWN;

    if (key == -10) match.key = SDLK_RETURN;
    else if (key == -9) match.key = SDLK_TAB;
    else if (key == -27) match.key = SDLK_ESCAPE;
    else if (key == -127) match.key = SDLK_BACKSPACE;
    else if (key < 0 && key >= -26) {
        match.ctrl = true;
        match.key = (SDL_Keycode)(SDLK_a + (-key) - 1);
    } else if (key <= -32 && key >= -126) {
        match.key = (SDL_Keycode)-key;
        if (match.key > 64 && match.key < 91) {
            match.key = (SDL_Keycode)(match.key + 32);
            match.shift = true;
        }
    } else if (key > 0) {
        switch (key) {
            case KEY_DOWN: match.key = SDLK_DOWN; break;
            case KEY_UP: match.key = SDLK_UP; break;
            case KEY_LEFT: match.key = SDLK_LEFT; break;
            case KEY_RIGHT: match.key = SDLK_RIGHT; break;
            case KEY_BACKSPACE: match.key = SDLK_BACKSPACE; break;
            case KEY_F(1): match.key = SDLK_F1; break;
            case KEY_F(2): match.key = SDLK_F2; break;
            case KEY_F(3): match.key = SDLK_F3; break;
            case KEY_F(4): match.key = SDLK_F4; break;
            case KEY_F(5): match.key = SDLK_F5; break;
            case KEY_F(6): match.key = SDLK_F6; break;
            case KEY_F(7): match.key = SDLK_F7; break;
            case KEY_F(8): match.key = SDLK_F8; break;
            case KEY_F(9): match.key = SDLK_F9; break;
            case KEY_F(10): match.key = SDLK_F10; break;
            case KEY_F(11): match.key = SDLK_F11; break;
            case KEY_F(12): match.key = SDLK_F12; break;
            case KEY_DC: match.key = SDLK_DELETE; break;
            case KEY_NPAGE: match.key = SDLK_PAGEDOWN; break;
            case KEY_PPAGE: match.key = SDLK_PAGEUP; break;
            case KEY_ENTER: match.key = SDLK_RETURN; break;
        }
    }

    auto it = emu_bindings.find(match);
    if (it != emu_bindings.end()) {
        execute_emu_action(it->second);
        return true;
    }

    return false;
}
