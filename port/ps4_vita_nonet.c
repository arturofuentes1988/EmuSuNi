// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// The PS Vita core without a network. Vita3K gives a game's sockets (SceNet, SceHttp, ad hoc play) to the
// system's own: a game with online features would connect from the PS4 to whatever it likes - Sony's servers among
// them, which a jailbroken PS4 must never reach (the owner's rule of 2026-11-13: no connection to Sony's servers,
// ever). So the core has no way out: scripts/build-vita-core.sh links it with ld.lld -r --wrap=<each of these>, and
// every call of the core's that would send something, take something in or ask a name server comes here and fails
// as on a console whose network is down, without asking the system. The script checks that the object asks the
// system for none of them.
// A socket itself is still made: it reaches nothing, and the core waits on it and closes it as before (a socket
// that isn't one would be a number the core's select() can't take).
// The first calls go to the log ("vita: net: ..."), with the name a game asked for.
#include <errno.h>
#include <netdb.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "ps4_vita_sys.h"

// One line for each of the first calls (a game that keeps trying doesn't fill the log).
static void refused(const char *what, const char *name)
{
  static unsigned s_said;
  const unsigned said = __atomic_fetch_add(&s_said, 1, __ATOMIC_RELAXED);
  if (said < 12)
  {
    emusuni_vita_log("vita: net: the game asked for the network (%s%s%s): the PS Vita's network is off in EmuSuNi%s",
                     what, name ? " " : "", name ? name : "", said == 11 ? " (not said again)" : "");
  }
}

static int down(const char *what)
{
  refused(what, NULL);
  errno = ENETDOWN;
  return -1;
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
  (void)fd, (void)address, (void)length;
  return down("connect");
}

ssize_t __wrap_send(int fd, const void *data, size_t size, int flags)
{
  (void)fd, (void)data, (void)size, (void)flags;
  return down("send");
}

ssize_t __wrap_sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t length)
{
  (void)fd, (void)data, (void)size, (void)flags, (void)to, (void)length;
  return down("sendto");
}

// (18-11, after 01.76, the "netbind" switch.) A PS Vita without a network still lets a game give its socket a
// port: nothing comes or goes, and the game finds that out by its own clock. Refused here, LittleBigPlanet closed
// its socket and went on asking a socket that wasn't one for what had arrived, for ever, at "Connecting to
// LittleBigPlanet PS Vita Online". With the switch the answer is "done" and nothing is bound: the socket has no
// port, nothing reaches it, and what the game sends from it fails as before.
int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length)
{
  (void)fd, (void)address, (void)length;
  if (emusuni_vita_switch(EMUSUNI_VITA_NETBIND))
  {
    static unsigned s_said;
    if (__atomic_fetch_add(&s_said, 1, __ATOMIC_RELAXED) < 4)
      emusuni_vita_log("vita: net: the game gave a socket a port: answered \"done\", nothing bound (the netbind switch)");
    return 0;
  }
  return down("bind");
}

int __wrap_listen(int fd, int backlog)
{
  (void)fd, (void)backlog;
  return down("listen");
}

int __wrap_accept(int fd, struct sockaddr *address, socklen_t *length)
{
  (void)fd, (void)address, (void)length;
  return down("accept");
}

// (Below zero whatever the system's numbers: Vita3K's HTTP takes only that for a failure.)
int __wrap_getaddrinfo(const char *name, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
  (void)service, (void)hints;
  refused("the address of", name ? name : "(no name)");
  if (result)
    *result = NULL;
  return EAI_FAIL < 0 ? EAI_FAIL : -EAI_FAIL;
}

struct hostent *__wrap_gethostbyname(const char *name)
{
  refused("the address of", name ? name : "(no name)");
  return NULL;
}

struct hostent *__wrap_gethostbyaddr(const void *address, socklen_t length, int type)
{
  (void)address, (void)length, (void)type;
  refused("the name of an address", NULL);
  return NULL;
}
