// Native Win32 window with an OpenGL-capable pixel format and an input event
// queue using Android's AInputEvent numbering (so the NDK emulation layer can
// forward events to the game unchanged).
#pragma once

namespace zb::win {

enum EventType { EV_KEY = 1, EV_MOTION = 2 };      // AINPUT_EVENT_TYPE_*
enum MotionAction { ACT_DOWN = 0, ACT_UP = 1, ACT_MOVE = 2, ACT_CANCEL = 3 };

struct Event {
    int type = 0;
    int action = 0;     // key: 0 down / 1 up; motion: MotionAction
    int keycode = 0;    // Android AKEYCODE_*
    int pointer = 0;
    float x = 0, y = 0;
};

// Creates the window on a dedicated UI thread and blocks until it is ready. The game renders at
// client_w x client_h for the whole session; resizing or going full screen scales that image.
bool start(int client_w, int client_h, const char* title, bool fullscreen = false);
void stop();

void* hwnd();
void* hdc();
int width();   // game render size (fixed)
int height();
int client_width();   // current client area of the window
int client_height();

// Where the game image goes in the client area (top-left origin), aspect ratio kept.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};
Rect game_rect();

// The process handles display scaling itself (called by start(); before any window exists).
void enable_dpi_awareness();
void primary_monitor_size(int* w, int* h);  // physical pixels

// F11 / Alt+Enter do the same from the keyboard.
void toggle_fullscreen();
void set_client_size(int w, int h);  // leaves full screen
void simulate_focus(bool gained);     // tests: as if the window gained / lost the keyboard focus

bool pop_event(Event* out);
bool has_events();
bool close_requested();
void request_close();
bool focused();

// Inject input through the window's own message queue (same path as real keyboard/mouse input).
void post_key(unsigned vk, bool down);
void post_mouse(int client_x, int client_y, bool down);

}  // namespace zb::win
