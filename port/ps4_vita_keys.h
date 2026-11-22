/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE). */

/* The PS Vita's keys from a file of the player's, for the core built without them inside (EMUSUNI_VITA_NOKEYS:
 * scripts/build.sh with-vita-nokeys, scripts/build-vita-core.sh with VITA_CORE_NOKEYS=1).
 *
 * Vita3K's source has Sony's keys written in it: those of the system's modules and packages (the firmware's
 * install, an encrypted game's modules), of the store's packages, of a game card's image, of the encrypted file
 * system of a game (psvpfstools) and the table the system's registry is scrambled with. The build with them
 * (with-vita) has them in the eboot. This one has none: every table is zeros and the list of the modules' keys is
 * empty until emusuni_vita_keys_load() reads them from <system>/vita3k/vita-keys.txt (scripts/make-vita-keys.py
 * makes that file out of Vita3K's source), and only when the file's SHA-256 is the one the build was made with:
 * a wrong key makes Vita3K read past its buffers, a wrong file is the same as none.
 *
 * Without the file no game starts: the firmware can't be installed, and one installed before doesn't help, for
 * the system's modules stay encrypted on the disk and every game loads them (libc.suprx first). What needs a key
 * says so in the log ("vita: keys: ...") and fails as a file that can't be read does: the core then asks the
 * frontend to close the game, nothing breaks. The app (src/Vita.cpp) looks at the file itself first and tells the
 * player before the core is asked.
 *
 * Called with no game running (the core: at retro_load_game, before its load job starts). */

#ifndef EMUSUNI_PS4_VITA_KEYS_H
#define EMUSUNI_PS4_VITA_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the file again. 1: it is the one expected, the keys are in. 0: none are (missing, or not the file). */
int emusuni_vita_keys_load(const char *path);
/* 1 after a load that found the file right. */
int emusuni_vita_keys_ready(void);
/* For the places that need a key: 0 when the keys are in; else 1, and a line in the log that `what` needs them. */
int emusuni_vita_keys_missing(const char *what);
/* The keys of the system's modules and packages, in the file's order: 1 while there is one at `index`.
 * set: Vita3K's register_keys type (0, 1). names: its KeyType, SceType and SelfType names. versions: the first and
 * the last system version it is for. The strings stay until the next load. */
int emusuni_vita_keys_sce(int index, int *set, const char **names, int *revision, const char **key, const char **iv,
                          unsigned long long *versions);
/* The table the registry's file is scrambled with (zeros without the file). */
extern unsigned char emusuni_vita_registry_xor[16];

#ifdef __cplusplus
}
#endif

#endif
