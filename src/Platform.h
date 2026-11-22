// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.25, the owner: "¿en este registro viene la versión de hackeo de la ps4? ... más compatible con ps5") The start's
// "platform:" line (src/main.cpp's LogPlatform): what EmuSuNi runs on, from what is read without risk - nothing is asked
// of the jailbreak (a call a HEN lacks could close the app), nothing of the player's:
// - the console: a PS5 runs a PS4 app on its own Zen 2 CPU (CPUID's family 0x17; the PS4's and PS4 Pro's Jaguar 0x16);
//   a PS4 Pro that runs this app in its Pro mode says so (sceKernelIsNeoMode) - in its base mode it looks like a PS4;
// - the system software (sceKernelGetSystemSwVersion), the CPU's and the TSC's clocks, the kernel's hw.model;
// - the jailbreak as far as it shows: its folder in /data (GoldHEN, etaHEN...) and the modules loaded into this process
//   that aren't the system's .sprx (a HEN's own - _fix_505.prx, plugins - and the package's). Not its version: GoldHEN
//   tells it only to a call to it.
// The words are made here (Line), apart from the reading, so the PC's test checks them.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Platform
{
struct Facts
{
  uint32_t cpuid_eax1 = 0;            // CPUID leaf 1's EAX: the CPU's family and model
  std::string cpu_brand;              // CPUID's brand string ("": none)
  int neo = -1;                       // sceKernelIsNeoMode (1: a PS4 Pro in its Pro mode; -1: not known)
  uint32_t sw_version = 0;            // sceKernelGetSystemSwVersion's (0x12520000: 12.52; 0: not known)
  uint64_t cpu_hz = 0, tsc_hz = 0;    // (0: not known)
  std::string hw_model;               // sysctl hw.model ("": not given)
  std::vector<std::string> folders;   // the jailbreaks' folders found in /data
  std::vector<std::string> modules;   // the names of the modules loaded into this process
};

inline int CpuFamily(uint32_t eax)
{
  const int base = static_cast<int>((eax >> 8) & 0xF);
  return base == 0xF ? base + static_cast<int>((eax >> 20) & 0xFF) : base;
}

inline int CpuModel(uint32_t eax)
{
  const int model = static_cast<int>((eax >> 4) & 0xF);
  return CpuFamily(eax) >= 0xF ? model | static_cast<int>(((eax >> 16) & 0xF) << 4) : model;
}

inline std::string Console(const Facts& f)
{
  const int family = CpuFamily(f.cpuid_eax1);
  if (family == 0x17)
    return "PS5 (its PS4 mode)";
  if (family == 0x16)
    return f.neo == 1 ? "PS4 Pro (Pro mode)" : "PS4 (or a PS4 Pro in its base mode)";
  return f.cpuid_eax1 ? "not a PS4 nor a PS5 CPU" : "unknown";
}

inline bool SystemModule(const std::string& name)
{
  return name.size() > 5 && name.compare(name.size() - 5, 5, ".sprx") == 0;
}

inline std::string Line(const Facts& f)
{
  char text[256];
  std::string line = Console(f);
  if (f.sw_version)
  {
    std::snprintf(text, sizeof(text), "; system %x.%02x", f.sw_version >> 24, (f.sw_version >> 16) & 0xFF);
    line += text;
  }
  std::snprintf(text, sizeof(text), "; CPU family %#x model %#x", CpuFamily(f.cpuid_eax1), CpuModel(f.cpuid_eax1));
  line += text;
  if (!f.cpu_brand.empty())
    line += " \"" + f.cpu_brand + "\"";
  if (f.cpu_hz)
  {
    std::snprintf(text, sizeof(text), ", %llu MHz", static_cast<unsigned long long>(f.cpu_hz / 1000000));
    line += text;
  }
  if (f.tsc_hz)
  {
    std::snprintf(text, sizeof(text), "; TSC %llu MHz", static_cast<unsigned long long>(f.tsc_hz / 1000000));
    line += text;
  }
  if (f.neo >= 0)
    line += f.neo ? "; Pro mode" : "; not Pro mode";
  if (!f.hw_model.empty())
    line += "; hw.model \"" + f.hw_model + "\"";
  line += "; jailbreak's folder:";
  for (const std::string& folder : f.folders)
    line += " /data/" + folder;
  if (f.folders.empty())
    line += " none found";
  std::string own;
  int system = 0;
  for (const std::string& name : f.modules)
  {
    if (SystemModule(name))
      system++;
    else
      own += (own.empty() ? "" : ", ") + name;
  }
  std::snprintf(text, sizeof(text), "; modules: %d of the system's, the others: ", system);
  line += text + (own.empty() ? std::string("none") : own);
  return line;
}
}  // namespace Platform
