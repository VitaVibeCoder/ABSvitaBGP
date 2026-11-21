#include "bgmusic.h"

#include <ctype.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "music.h"
#include "strutil.h"
#include "sys.h"

#define BGM_MOD_PATH "app0:module/libSceMusic.suprx"

static struct {
    SceUID mod;
    bool svc_inited;
    bool active;
    char state_path[256];
    char pos_path[256];
    char item_id[40];
    double duration;
    char orig_path[512];
    char play_path[512];
} G = {-1, false, false, {0}, {0}, {0}, 0, {0}, {0}};

static bool ends_with_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return false;
    const char *p = s + ls - lf;
    for (size_t i = 0; i < lf; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

static void save_state(void) {
    if (!G.state_path[0]) return;
    FILE *f = fopen(G.state_path, "wb");
    if (!f) return;
    fprintf(f, "%s\n%.3f\n%s\n%s\n", G.item_id, G.duration, G.orig_path, G.play_path);
    fclose(f);
}

static void clear_state(void) {
    G.item_id[0] = 0;
    G.duration = 0;
    G.orig_path[0] = 0;
    G.play_path[0] = 0;
    G.active = false;
    if (G.state_path[0]) remove(G.state_path);
    if (G.pos_path[0]) remove(G.pos_path);
}

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static bool load_state(void) {
    if (!G.state_path[0]) return false;
    FILE *f = fopen(G.state_path, "rb");
    if (!f) return false;
    char dline[64];
    bool ok = fgets(G.item_id, sizeof G.item_id, f) &&
              fgets(dline, sizeof dline, f) &&
              fgets(G.orig_path, sizeof G.orig_path, f) &&
              fgets(G.play_path, sizeof G.play_path, f);
    fclose(f);
    if (!ok) { clear_state(); return false; }
    trim_nl(G.item_id);
    trim_nl(dline);
    trim_nl(G.orig_path);
    trim_nl(G.play_path);
    G.duration = atof(dline);
    return G.item_id[0] != 0 && G.play_path[0] != 0;
}

/* Read paused/skipped position persisted by absvita_qm.suprx */
static uint32_t load_plugin_pos_ms(void) {
    if (!G.pos_path[0]) return 0;
    FILE *f = fopen(G.pos_path, "rb");
    if (!f) return 0;
    char ms_line[64];
    char uri_line[512];
    uri_line[0] = 0;
    if (!fgets(ms_line, sizeof ms_line, f)) { fclose(f); return 0; }
    (void)fgets(uri_line, sizeof uri_line, f);
    fclose(f);
    trim_nl(ms_line);
    trim_nl(uri_line);
    if (uri_line[0] &&
        strcmp(uri_line, G.play_path) != 0 &&
        (!G.item_id[0] || strstr(uri_line, G.item_id) == NULL)) {
        return 0;
    }
    unsigned long ms = strtoul(ms_line, NULL, 10);
    return (uint32_t)ms;
}

static void restore_renamed_file(void) {
    if (G.orig_path[0] && G.play_path[0] && strcmp(G.orig_path, G.play_path) != 0) {
        int r = rename(G.play_path, G.orig_path);
        dbg("bgmusic: restore rename '%s' -> '%s' rc=%d", G.play_path, G.orig_path, r);
    }
}

static int ensure_service(void) {
    if (G.mod < 0) return -1;
    if (G.svc_inited) return 0;
    int32_t rc = sceMusicPlayerServiceInitialize(SCE_MUSIC_PLAYER_SERVICE_INIT_TYPE_0);
    dbg("bgmusic: init(type 0) -> %08x", (unsigned)rc);
    if (rc < 0 && rc != (int32_t)SCE_MUSIC_PLAYER_SERVICE_ERROR_ALREADY_INITIALIZED) {
        rc = sceMusicPlayerServiceInitialize(SCE_MUSIC_PLAYER_SERVICE_INIT_TYPE_1);
        dbg("bgmusic: init(type 1) -> %08x", (unsigned)rc);
    }
    if (rc < 0 && rc != (int32_t)SCE_MUSIC_PLAYER_SERVICE_ERROR_ALREADY_INITIALIZED)
        return (int)rc;
    G.svc_inited = true;
    return 0;
}

void bgmusic_init(const char *data_dir) {
    snprintf(G.state_path, sizeof G.state_path, "%s/bgmusic.txt", data_dir);
    snprintf(G.pos_path, sizeof G.pos_path, "%s/bgmusic_pos.txt", data_dir);
    G.mod = sceKernelLoadStartModule(BGM_MOD_PATH, 0, NULL, 0, NULL, NULL);
    dbg("bgmusic: load %s -> %08x", BGM_MOD_PATH, (unsigned)G.mod);
    if (load_state()) {
        dbg("bgmusic: loaded saved state id=%s path=%s", G.item_id, G.play_path);
        G.active = true;
    }
}

bool bgmusic_reconcile(char *item_id, size_t id_sz, double *pos_s, double *dur_s) {
    if (!G.item_id[0] || G.mod < 0) return false;
    if (ensure_service() < 0) return false;

    char uri[SCE_MUSIC_MAX_STRING_LENGTH];
    memset(uri, 0, sizeof uri);
    int32_t urc = sceMusicPlayerServiceGetUri(uri, sizeof uri);

    SceMusicPlayerServicePlayStatusExtension ext;
    memset(&ext, 0, sizeof ext);
    int32_t src = sceMusicPlayerServiceGetPlayStatusExtension(&ext);
    dbg("bgmusic: reconcile urc=%08x uri='%s' src=%08x state=%d pos=%ums dur=%ums",
        (unsigned)urc, uri, (unsigned)src, ext.currentState, ext.currentTimeMs, ext.durationMs);

    uint32_t plugin_ms = load_plugin_pos_ms();
    bool uri_matches = (urc == 0 && uri[0] &&
                        (strcmp(uri, G.play_path) == 0 || strstr(uri, G.item_id) != NULL));

    if (!uri_matches) {
        if (plugin_ms > 0) {
            /* Paused via QuickMenuReborn plugin and MusicCore cleared its URI */
            if (item_id && id_sz) str_copy(item_id, id_sz, G.item_id);
            if (pos_s) *pos_s = plugin_ms / 1000.0;
            if (dur_s) *dur_s = G.duration;
            G.active = false;
            return true;
        }
        /* SceShell moved on to another track or cleared its URI */
        restore_renamed_file();
        clear_state();
        return false;
    }

    if (src != 0 && plugin_ms == 0) return false;
    if (ext.currentState != SCE_MUSIC_COMMAND_PLAY) G.active = false;

    uint32_t best_ms = (ext.currentState == SCE_MUSIC_COMMAND_PLAY && ext.currentTimeMs > 0)
                           ? ext.currentTimeMs
                           : (ext.currentTimeMs > 0 ? ext.currentTimeMs : plugin_ms);
    if (best_ms == 0) return false;

    if (item_id && id_sz) str_copy(item_id, id_sz, G.item_id);
    if (pos_s) *pos_s = best_ms / 1000.0;
    if (dur_s) *dur_s = G.duration > 0 ? G.duration : (ext.durationMs / 1000.0);
    return true;
}

int bgmusic_start(const char *item_id, const char *track_path, double pos_s, double dur_s,
                  char *err, size_t err_sz) {
    if (err && err_sz) err[0] = 0;
    if (G.mod < 0) {
        if (err && err_sz) str_copy(err, err_sz, "libSceMusic module not loaded");
        return -1;
    }

    /* Release ABSvita's own BGM port so SceShell's MusicCore can take audio focus */
    sys_release_bgm_port();

    int rc = ensure_service();
    if (rc < 0) {
        sys_acquire_bgm_port();
        if (err && err_sz) snprintf(err, err_sz, "Music service init failed (%08x)", (unsigned)rc);
        return rc;
    }

    /* If we previously renamed a different book's file, stop and restore it first */
    if (G.play_path[0] && strcmp(G.orig_path, track_path) != 0) {
        sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_STOP, 0);
        sceKernelDelayThread(100 * 1000);
        restore_renamed_file();
    }

    char play_path[512];
    str_copy(play_path, sizeof play_path, track_path);

    SceMusicOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.unk_00 = -1;

    sceMusicPlayerServiceSetRepeatMode(SCE_MUSIC_REPEAT_MODE_DISABLE);
    int32_t set_rc = sceMusicPlayerServiceSetUri(play_path, &opt);
    dbg("bgmusic: SetUri('%s') -> %08x", play_path, (unsigned)set_rc);

    /* MusicCore supports AAC (.m4a) and may reject the .m4b extension by name */
    if (set_rc < 0 && ends_with_ci(track_path, ".m4b")) {
        size_t len = strlen(play_path);
        play_path[len - 1] = 'a'; /* .m4b -> .m4a */
        int rrc = rename(track_path, play_path);
        dbg("bgmusic: rename '%s' -> '%s' rc=%d", track_path, play_path, rrc);
        if (rrc == 0) {
            set_rc = sceMusicPlayerServiceSetUri(play_path, &opt);
            dbg("bgmusic: SetUri('%s') -> %08x", play_path, (unsigned)set_rc);
            if (set_rc < 0) rename(play_path, track_path);
        }
    }

    if (set_rc < 0) {
        sys_acquire_bgm_port();
        if (err && err_sz) snprintf(err, err_sz, "MusicCore rejected file (%08x)", (unsigned)set_rc);
        return (int)set_rc;
    }

    sceMusicPlayerServiceSetVolume(SCE_MUSIC_MAX_VOLUME);

    uint32_t seek_ms = pos_s > 1.0 ? (uint32_t)(pos_s * 1000.0) : 0;
    if (seek_ms > 0) {
        int32_t sk = sceMusicPlayerServiceSetSeekTime(seek_ms);
        dbg("bgmusic: pre-play SetSeekTime(%u) -> %08x", seek_ms, (unsigned)sk);
    }

    int32_t play_rc = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_PLAY, 0);
    dbg("bgmusic: PLAY -> %08x", (unsigned)play_rc);
    if (play_rc < 0) {
        if (strcmp(play_path, track_path) != 0) rename(play_path, track_path);
        sys_acquire_bgm_port();
        if (err && err_sz) snprintf(err, err_sz, "MusicCore play failed (%08x)", (unsigned)play_rc);
        return (int)play_rc;
    }

    if (seek_ms > 0) {
        sceKernelDelayThread(200 * 1000);
        SceMusicPlayerServicePlayStatusExtension ext;
        memset(&ext, 0, sizeof ext);
        int32_t st_rc = sceMusicPlayerServiceGetPlayStatusExtension(&ext);
        dbg("bgmusic: post-play status rc=%08x state=%d pos=%ums",
            (unsigned)st_rc, ext.currentState, ext.currentTimeMs);
        if (st_rc == 0 && ext.currentTimeMs + 3000 < seek_ms) {
            sceMusicPlayerServiceSetSeekTime(seek_ms);
            int32_t sk_rc = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_SEEK, 0);
            dbg("bgmusic: post-play SEEK(%u) -> %08x", seek_ms, (unsigned)sk_rc);
        }
    }

    str_copy(G.item_id, sizeof G.item_id, item_id);
    G.duration = dur_s;
    str_copy(G.orig_path, sizeof G.orig_path, track_path);
    str_copy(G.play_path, sizeof G.play_path, play_path);
    G.active = true;
    if (G.pos_path[0]) remove(G.pos_path);
    save_state();
    return 0;
}

bool bgmusic_stop(char *item_id, size_t id_sz, double *pos_s, double *dur_s) {
    bool got_pos = false;
    if (G.mod >= 0 && G.active) {
        if (ensure_service() == 0) {
            got_pos = bgmusic_reconcile(item_id, id_sz, pos_s, dur_s);
            int32_t rc = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_STOP, 0);
            dbg("bgmusic: STOP -> %08x", (unsigned)rc);
            sceKernelDelayThread(100 * 1000);
        }
    }
    restore_renamed_file();
    clear_state();
    sys_acquire_bgm_port();
    return got_pos;
}

bool bgmusic_active(void) { return G.active; }

void bgmusic_shutdown(void) {
    if (G.svc_inited) {
        /* Disconnect client WITHOUT sending STOP so SceShell keeps playing under games */
        int32_t rc = sceMusicPlayerServiceTerminate();
        dbg("bgmusic: terminate (keep playing=%d) -> %08x", (int)G.active, (unsigned)rc);
        G.svc_inited = false;
    }
}
