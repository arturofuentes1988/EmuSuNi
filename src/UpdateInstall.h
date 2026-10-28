// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The updater's hand-over to the PS4's package installer (00.73, src/UpdateInstall.cpp): a downloaded .pkg installed
// the way homebrew stores do it - a background task of the PS4's own installer (BGFT: RegisterTaskByStorageEx, then
// StartTask), which its notifications show (Notifications > Downloads). The installer copies the package; the copy
// is followed here.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace Update::Install
{
// Hands the package at `path` (as the app sees it: /usb0/..., /data/...; the installer is given the system's view of
// it: /mnt/usb0/..., /user/data/...) to the PS4's installer. False, with *error (for the log), when it couldn't be
// asked; never throws. A file `data_dir`/update-keep-tasks keeps the PS4's old task of EmuSuNi when the installer
// says the same application is installed already (then this fails).
bool Start(const std::string& path, const std::string& version, const std::string& data_dir, std::string* error);

enum class Result
{
  Installed,  // the PS4 copied the whole package without an error
  Failed,     // its task reported an error (*state)
  Unknown,    // it can't be followed (no progress the app can read), or `stop` was set
};
// Follows the installation Start began: `progress` (optional) gets the bytes copied and the package's size (`size`:
// the downloaded file's, to tell which way the PS4 reports them). Polls every quarter of a second; for the updater's
// thread. *state says how it ended (for the log).
Result Follow(int64_t size, std::atomic<bool>* stop, std::string* state, void (*progress)(int64_t done, int64_t total));
}  // namespace Update::Install
