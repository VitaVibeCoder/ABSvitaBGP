/* Background playback via SceShell's MusicCore (libSceMusic).
 * Lets a downloaded single-file audiobook keep playing under games while
 * ABSvita is closed, and reconciles the position back into the progress queue
 * when ABSvita is reopened. */
#ifndef ABSVITA_BGMUSIC_H
#define ABSVITA_BGMUSIC_H
#include <stdbool.h>
#include <stddef.h>

/* Loads app0:module/libSceMusic.suprx and reads any saved handoff state. */
void bgmusic_init(const char *data_dir);

/* Queries SceShell for any progress made while ABSvita was closed.
 * Returns true if a valid position (> 0) was retrieved for a known book. */
bool bgmusic_reconcile(char *item_id, size_t id_sz, double *pos_s, double *dur_s);

/* Hands off a downloaded local audio file to SceShell's MusicPlayerService
 * at pos_s. Automatically renames .m4b -> .m4a if MusicCore rejects .m4b.
 * Returns 0 on success, < 0 on failure (and writes a short user message to err). */
int bgmusic_start(const char *item_id, const char *track_path, double pos_s, double dur_s,
                  char *err, size_t err_sz);

/* Stops SceShell playback (if active), restores any .m4a -> .m4b rename,
 * re-acquires ABSvita's BGM port, and optionally returns the last position. */
bool bgmusic_stop(char *item_id, size_t id_sz, double *pos_s, double *dur_s);

/* True when ABSvita has handed playback off to SceShell and hasn't stopped it. */
bool bgmusic_active(void);

/* Disconnects the MusicPlayerService client WITHOUT stopping playback, so
 * SceShell keeps playing after ABSvita exits to launch a game. */
void bgmusic_shutdown(void);

#endif
