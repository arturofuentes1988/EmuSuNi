// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The free space of the PS4's data partition, for the cores that ask (PPSSPP's memory stick: a PSP
// game refuses to save on a full stick, and players saw "not enough space"). The PS4 libc's statvfs
// fills nothing: it calls statfs64 into a buffer of its own and returns only its sign, so a core
// reading the struct it passed got zeros. Its statfs writes the kernel's own struct statfs (FreeBSD's,
// 2344 bytes) into whatever it is given, so the kernel's layout is read here from a buffer big enough,
// checked by its version; the C library's layout is tried too, and a plain 2 GiB stands in when
// neither can be read (the data partition is big and the saves are small).

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

void emusuni_log_line(const char* text);  // src/Log.cpp

int emusuni_free_disk_space(const char* path, int64_t* out)
{
  enum { FREEBSD_STATFS_VERSION = 0x20030518 };
  const int64_t ASSUMED = (int64_t)2 << 30, MOST = (int64_t)1 << 44;
  static int logged = 0;
  int64_t space = ASSUMED;
  const char* how = "assumed";
  const int fd = open(path, O_RDONLY);
  if (fd >= 0)
  {
    unsigned char buf[4096];
    memset(buf, 0, sizeof(buf));
    if (fstatfs(fd, (struct statfs*)buf) == 0)
    {
      // The kernel's layout: f_version, f_type, then f_flags, f_bsize, f_iosize, f_blocks, f_bfree,
      // f_bavail as 64-bit words.
      uint32_t version;
      uint64_t bsize;
      int64_t bavail;
      memcpy(&version, buf, sizeof(version));
      memcpy(&bsize, buf + 16, sizeof(bsize));
      memcpy(&bavail, buf + 48, sizeof(bavail));
      const struct statfs* libc = (const struct statfs*)buf;
      if (version == FREEBSD_STATFS_VERSION && bsize >= 512 && bsize <= 65536 && bavail >= 0 &&
          bavail * (int64_t)bsize < MOST)
      {
        space = bavail * (int64_t)bsize;
        how = "the kernel's statfs";
      }
      else if (libc->f_bsize >= 512 && libc->f_bsize <= 65536 && (int64_t)libc->f_bavail >= 0 &&
               (int64_t)libc->f_bavail * (int64_t)libc->f_bsize < MOST)
      {
        space = (int64_t)libc->f_bavail * (int64_t)libc->f_bsize;
        how = "the C library's statfs";
      }
      else
      {
        how = "statfs unreadable, assumed";
      }
    }
    else
    {
      how = "fstatfs failed, assumed";
    }
    close(fd);
  }
  else
  {
    how = "can't open the folder, assumed";
  }
  if (!logged)
  {
    char line[320];
    snprintf(line, sizeof(line), "disk: free space at %s: %lld MiB (%s)", path, (long long)(space >> 20), how);
    emusuni_log_line(line);
    logged = 1;
  }
  *out = space;
  return 0;
}

// The same reading for the updater (src/Update.cpp, 00.73), which needs to know whether it is one: 1 and *out when the
// free space of the drive or partition that has `path` could be read, else 0 (nothing assumed).
int emusuni_disk_space_known(const char* path, int64_t* out)
{
  enum { FREEBSD_STATFS_VERSION = 0x20030518 };
  const int64_t MOST = (int64_t)1 << 44;
  const int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  unsigned char buf[4096];
  memset(buf, 0, sizeof(buf));
  int known = 0;
  if (fstatfs(fd, (struct statfs*)buf) == 0)
  {
    uint32_t version;
    uint64_t bsize;
    int64_t bavail;
    memcpy(&version, buf, sizeof(version));
    memcpy(&bsize, buf + 16, sizeof(bsize));
    memcpy(&bavail, buf + 48, sizeof(bavail));
    const struct statfs* libc = (const struct statfs*)buf;
    if (version == FREEBSD_STATFS_VERSION && bsize >= 512 && bsize <= 65536 && bavail >= 0 &&
        bavail * (int64_t)bsize < MOST)
    {
      *out = bavail * (int64_t)bsize;
      known = 1;
    }
    else if (libc->f_bsize >= 512 && libc->f_bsize <= 65536 && (int64_t)libc->f_bavail >= 0 &&
             (int64_t)libc->f_bavail * (int64_t)libc->f_bsize < MOST)
    {
      *out = (int64_t)libc->f_bavail * (int64_t)libc->f_bsize;
      known = 1;
    }
  }
  close(fd);
  return known;
}
