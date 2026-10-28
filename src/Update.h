// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Updates from inside the app (00.73). The newest version is EmuSuNi's newest release on GitHub
// (github.com/arturofuentes1988/EmuSuNi: its tag vNN.NN and its .pkg), asked over HTTPS at most once a day by itself
// and whenever the player asks (Settings -> Check for Updates); with Include Test Versions, pre-releases too. A
// newer version's notice comes at every start until it is installed (the player's Later closes it until the next
// start; "[obligatoria]" in the release's notes makes it an important update). Update downloads the .pkg - to the
// USB drive's PKG folder, else /data/EmuSuNi/update - checks it (GitHub's SHA-256 of it, else a .sha256 file of
// the release, else its size) and hands it to the PS4's package installer; one on the internal storage is deleted
// once the PS4 installed it. Every step goes to the log ("update: ...").
// What is remembered between starts: /data/EmuSuNi/update.ini. A file /data/EmuSuNi/update-url.txt (its first line:
// another address of GitHub's answer, http or https) points the check somewhere else, for tests.
// Nothing here throws, and nothing blocks the caller: the network and the files are a thread's.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Update
{
// ---- The rules (no PS4 calls: tested on the PC) ----

// "v00.73", "00.73", "V0.73", "v00.73-test" -> 73 (the major number * 100 + the minor); -1 when it isn't a version.
int VersionNumber(const std::string& text);
// Whether version a is above version b ("NN.NN", "vNN.NN"); false when either isn't a version.
bool IsNewer(const std::string& a, const std::string& b);

struct Release
{
  std::string tag;          // "v00.74"
  std::string version;      // "00.74"
  std::string title;        // the release's name
  std::string notes;        // its text (Markdown)
  std::string page;         // its page on GitHub
  bool prerelease = false;  // a test version
  bool important = false;   // "[obligatoria]" in its notes
  std::string pkg_name;     // its .pkg
  std::string pkg_url;      // where the .pkg is downloaded from
  int64_t pkg_size = -1;
  std::string sha256;       // GitHub's SHA-256 of the .pkg (its "digest"), lowercase hex; "" when it gives none
  std::string sha256_url;   // a .sha256 file of the release for it, else ""
};

// GitHub's answer - one release (/releases/latest) or a list of them (/releases) - read: the newest version in it
// with a .pkg (test versions only with `tests`; drafts never). False, with *error, when there is none.
bool ParseReleases(const std::string& json, bool tests, Release* out, std::string* error);
// Whether the notes mark an important update: "[obligatoria]" (or "[mandatory]"), in any letters' case.
bool IsImportant(const std::string& notes);
// The notes' first lines as plain text (no Markdown marks; headings that only repeat the version, tables, pictures
// and the markers left out), at most `count`.
std::vector<std::string> NoteLines(const std::string& notes, size_t count);
// The SHA-256 in a .sha256 file ("<64 hex digits>  name", or the digits alone): lowercase, "" when there is none.
std::string HashFromText(const std::string& text);
// The SHA-256 of a file, lowercase hex ("" when it can't be read); `progress` (optional) gets the bytes read so far.
std::string FileSha256(const std::string& path, void (*progress)(int64_t done, void* user) = nullptr,
                       void* user = nullptr);

// What is remembered between starts (update.ini).
struct Memory
{
  int64_t checked = 0;   // when a check last got GitHub's answer (Unix time)
  std::string etag;      // that answer's ETag (the next check asks If-None-Match: no download when nothing changed)
  std::string etag_url;  // the address it was for
  bool tests = false;    // Include Test Versions
  Release found;         // the newest version found then (tag "": none)
  std::string pending;   // a .pkg on the internal storage handed to the installer, deleted once it is installed
  std::string pending_version;
  int64_t pending_time = 0;
};
std::string SaveMemory(const Memory& memory);
Memory LoadMemory(const std::string& text);
// Whether a check by itself is due at `now`: none answered in the last day (or the clock went back).
bool CheckDue(const Memory& memory, int64_t now);
// Whether the version found is newer than the one running.
bool HasNewer(const Memory& memory, const std::string& running);
// Whether the start's notice shows: a newer version known, not closed with Later in this session, and - when a check
// was due at this start - that check answered (an offline start shows nothing).
bool NoticeDue(const Memory& memory, const std::string& running, bool closed, bool check_due, bool check_answered);
// Free space needed for a download of `size` bytes (unknown: -1): on the internal storage the PS4's installer copies
// it once more.
int64_t SpaceNeeded(int64_t size, bool internal);

// ---- The app's side ----

enum class Phase
{
  Idle,         // nothing asked yet
  Checking,     // asking GitHub
  Checked,      // its answer is in (newer or not), or the check failed (message)
  Downloading,  // the .pkg (done / total)
  Verifying,    // its SHA-256 (done / total)
  Installing,   // handed to the PS4's installer: it copies the package in the background (done / total)
  Installed,    // the PS4 has the whole package: EmuSuNi closes to finish (the player's choice)
  Saved,        // downloaded and checked, but the installer couldn't be asked: saved for GoldHEN's Package Installer
  Failed,       // the update failed (message)
};

struct Status
{
  Phase phase = Phase::Idle;
  std::string running;    // this version ("00.73")
  Release newest;         // the newest version known (tag "": none)
  bool newer = false;     // newest above running
  bool important = false;
  bool checked = false;   // a check answered (in this session or remembered)
  int64_t checked_at = 0;
  bool tests = false;
  std::string message;    // the last result, plain words (English: PS4Lang::T)
  std::string detail;     // more of it (where the .pkg is, what to do)
  int64_t done = 0, total = -1;  // bytes downloaded or checked
  std::string saved;      // where the .pkg is
  bool busy = false;      // the updater's thread is at work (a check, a download...)
  int serial = 0;         // changes with everything above
};

// At the start (the menu's first one): update.ini read; a .pkg left on the internal storage deleted when its version
// is the one running now (installed) or it is a day old; the day's check started when one is due.
void Start(const std::string& data_dir, const std::string& running);
// Settings -> Check for Updates: a check now (unless one or a download is running).
void Check();
// Update: download, check and hand the newest version's .pkg to the installer (a thread).
void StartUpdate();
// Stops a download.
void Cancel();
// Include Test Versions (remembered); a check follows.
void SetTests(bool on);
Status Get();
int Serial();
// The start's notice: wanted now (see NoticeDue)? Once shown (NoticeShown) not again in this session: Later only
// closes it until the next start.
bool NoticeWanted();
void NoticeShown();
// The app is closing (the thread stops and is waited for).
void Stop();
}  // namespace Update
