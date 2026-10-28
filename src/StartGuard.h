// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The start watchdog (00.70). The owner: "some games don't open the first time: the screen goes blue, black or white
// and I have to start the game again". A game's start is watched from the core's set-up to a while after its first
// picture, so that a start that stops repairs itself and the log keeps where it stopped:
//  - data_dir/start.txt is written as a start begins (the game, its attempt, when) and removed once its first picture
//    is shown or the start ends. One left there at the next app start means the app was closed or stopped during that
//    start: the log says so, and for which game (a second attempt's: the menu says so too).
//  - The start's steps are logged with their times ("start: ..."), and until the first picture (and while a game ends)
//    the watchdog's thread writes the log through to the disk every 2 s, so a hang, a forced close or a power-off
//    keeps them.
//  - No picture in the console's time while the game runs (the core comes back from each frame, but no picture comes):
//    the game is stopped as Quit Game stops it and started once more (Emulator::Run returns Retry); the second time it
//    ends with a message in the menu instead.
//  - The emulation thread stopped inside one call into the core longer than the console's time, or a crash, while the
//    game starts: the watchdog's thread restarts EmuSuNi (sceSystemServiceLoadExec of /app0/eboot.bin) with
//    data_dir/start-again.txt, and the new process starts the same game at once, with a toast (a second attempt that
//    stops the same way opens the menu with a message instead). A game's end that stops the same way restarts EmuSuNi
//    into the menu.
// Off the game's timing: a pass of the main loop costs an atomic store (Beat), a picture an atomic add (Picture); the
// watchdog's thread sleeps 250 ms at a time and does nothing while no game starts or ends.

#pragma once

#include <cstdint>
#include <string>

namespace StartGuard
{
// The times of a console (seconds): of the game running without a picture, of one call into the core (a stop longer
// than that is the emulation thread stuck), and of the game's load (the core's load_game: big images from USB).
struct Limits
{
  float no_picture = 20.0f;
  float stuck = 45.0f;
  float loading = 90.0f;
};

// At the app's start, once the log is open: what the process before left (start.txt, start-again.txt) is read, logged
// and removed, and the watchdog's thread starts. *game_path: a game to start at once (the watchdog restarted EmuSuNi
// for it; "": none); *message: a failure for the menu to show ("": none).
void AppStarted(const std::string& data_dir, std::string* game_path, std::string* message);
// That game isn't started after all (not found, L1 held): its next start is a first attempt.
void Forget();

// A game's start (Emulator::Run, first thing): start.txt written, the watch on. The second attempt of the same game
// (after Retry, or the restart for it) is told by its path.
void Begin(const std::string& path, const std::string& title, const std::string& console, const Limits& limits);
// The attempt under way: 1, or 2 (a start again).
int Attempt();
// The toast for a start again ("": the first attempt).
std::string RetryToast();
// Whether Continue's state is left out this time: the attempt before stopped while that state loaded (its step).
bool SkipContinue();
inline constexpr const char* CONTINUE_STEP = "Continue: the saved state loads";
// A step of the start (a text that lives as long as the program), logged with its time since the start; a heartbeat.
// loading: the call that follows may take the console's loading time.
void Step(const char* what, bool loading = false);
// The main loop's heartbeat, at the start of each pass.
void Beat();
// A picture from the core (VideoRefresh with a frame).
void Picture();
// At the end of each pass of the main loop: the frames the core has run since the game started, and whether the
// in-game menu is open (its time isn't the game's; the time is the clock's, not the loop's clamped one). Retry: no
// picture in time, the first attempt; Fail: the second (FailMessage says it for the menu).
enum class Verdict
{
  Go,
  Retry,
  Fail
};
Verdict Pass(int64_t frames_run, bool menu_open);
std::string FailMessage();
// The game's end path begins (its stop is watched too) / the game has ended (start.txt removed, the watch off).
void Ending();
void End();

// The game's start and end, as a scope (Emulator::Run's returns).
class Scope
{
public:
  Scope(const std::string& path, const std::string& title, const std::string& console, const Limits& limits)
  {
    Begin(path, title, console, limits);
  }
  ~Scope() { End(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};
}  // namespace StartGuard

// port/ps4_crashlog.cpp's crash handler, after crash.log: a crash while a game starts or ends. The watchdog's thread
// restarts EmuSuNi meanwhile; this waits for it (6 s at most) and comes back (the app then ends as before).
extern "C" void emusuni_start_crashed(int sig);
