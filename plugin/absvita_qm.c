/*
 * absvita_qm.c - QuickMenuReborn plugin for ABSvita background playback
 *
 * Adds native Play/Pause, -15s, and +15s controls to the PS Vita Quick Menu
 * (hold PS button) using QuickMenuReborn and GrapheneCt's libSceMusic.
 */

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/types.h>

#include <music.h>
#include <quickmenureborn/qm_reborn.h>

#define SEP_ID        "absvita_qm_sep"
#define TITLE_ID      "absvita_qm_title"
#define ROW_PLANE_ID  "absvita_qm_row"
#define BTN_REW_ID    "absvita_qm_rew"
#define BTN_TOGGLE_ID "absvita_qm_toggle"
#define BTN_FWD_ID    "absvita_qm_fwd"

#define BGM_MOD_APP   "ux0:app/ABSV00001/module/libSceMusic.suprx"
#define BGM_MOD_TAI   "ur0:tai/libSceMusic.suprx"
#define STATE_PATH    "ux0:data/ABSvita/bgmusic.txt"
#define POS_PATH      "ux0:data/ABSvita/bgmusic_pos.txt"
#define LOG_PATH      "ux0:data/ABSvita/qm_debug.log"

#define SKIP_MS       15000u

enum {
    CMD_NONE      = 0,
    CMD_REFRESH   = 1,
    CMD_TOGGLE    = 2,
    CMD_SEEK_BACK = 3,
    CMD_SEEK_FWD  = 4
};

static SceUID g_worker_thid = -1;
static SceUID g_cmd_sema    = -1;
static SceUID g_music_mod   = -1;
static int    g_svc_inited  = 0;

static volatile int      g_running      = 1;
static volatile int      g_pending_cmd  = CMD_NONE;
static volatile int      g_has_state    = 0;
static volatile int      g_is_playing   = 0;
static volatile uint32_t g_saved_pos_ms = 0;
static volatile uint32_t g_saved_dur_ms = 0;

static char g_saved_uri[SCE_MUSIC_MAX_STRING_LENGTH];

static size_t qm_strlen(const char *s, size_t max_len) {
    size_t n = 0;
    while (n < max_len && s[n] != '\0') n++;
    return n;
}

static void qm_strcpy(char *dst, size_t dst_sz, const char *src) {
    if (dst_sz == 0) return;
    size_t i = 0;
    while (i + 1 < dst_sz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint32_t qm_parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static void qm_log(const char *msg) {
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, msg, qm_strlen(msg, 256));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

static void qm_log_hex(const char *tag, int32_t val) {
    char buf[128];
    sceClibSnprintf(buf, sizeof(buf), "%s: 0x%08x", tag, (unsigned)val);
    qm_log(buf);
}

/* Read line 4 (play_path) from ux0:data/ABSvita/bgmusic.txt */
static int load_uri_from_bgmusic_state(char *out, size_t out_sz) {
    SceUID fd = sceIoOpen(STATE_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[1536];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int line = 0;
    char *p = buf;
    while (*p && line < 3) {
        if (*p == '\n') line++;
        p++;
    }
    if (line < 3 || !*p) return -1;

    size_t i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && i + 1 < out_sz) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* Load last paused position if written by an earlier Quick Menu pause */
static void load_saved_pos_file(void) {
    SceUID fd = sceIoOpen(POS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return;

    char buf[1200];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    uint32_t ms = qm_parse_u32(buf);
    if (ms > 0) g_saved_pos_ms = ms;

    char *nl = buf;
    while (*nl && *nl != '\n') nl++;
    if (*nl == '\n') {
        nl++;
        char tmp[SCE_MUSIC_MAX_STRING_LENGTH];
        size_t i = 0;
        while (nl[i] && nl[i] != '\n' && nl[i] != '\r' && i + 1 < sizeof(tmp)) {
            tmp[i] = nl[i];
            i++;
        }
        tmp[i] = '\0';
        if (i > 0 && g_saved_uri[0] == '\0') {
            qm_strcpy(g_saved_uri, sizeof(g_saved_uri), tmp);
        }
    }
}

/* Persist paused/skipped position so ABSvita reconciles accurately on reopen */
static void save_pos_file(uint32_t pos_ms, const char *uri) {
    SceUID fd = sceIoOpen(POS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;

    char line[64];
    sceClibSnprintf(line, sizeof(line), "%u\n", pos_ms);
    sceIoWrite(fd, line, qm_strlen(line, sizeof(line)));
    if (uri && uri[0]) {
        sceIoWrite(fd, uri, qm_strlen(uri, SCE_MUSIC_MAX_STRING_LENGTH));
        sceIoWrite(fd, "\n", 1);
    }
    sceIoClose(fd);
}

static int ensure_music_service(void) {
    if (g_svc_inited) return 0;

    if (g_music_mod < 0) {
        g_music_mod = sceKernelLoadStartModule(BGM_MOD_APP, 0, NULL, 0, NULL, NULL);
        qm_log_hex("load app libSceMusic", g_music_mod);
        if (g_music_mod < 0) {
            g_music_mod = sceKernelLoadStartModule(BGM_MOD_TAI, 0, NULL, 0, NULL, NULL);
            qm_log_hex("load tai libSceMusic", g_music_mod);
        }
        if (g_music_mod < 0) return -1;
    }

    int32_t rc = sceMusicPlayerServiceInitialize(SCE_MUSIC_PLAYER_SERVICE_INIT_TYPE_0);
    qm_log_hex("init type 0", rc);
    if (rc < 0 && rc != (int32_t)SCE_MUSIC_PLAYER_SERVICE_ERROR_ALREADY_INITIALIZED) {
        rc = sceMusicPlayerServiceInitialize(SCE_MUSIC_PLAYER_SERVICE_INIT_TYPE_1);
        qm_log_hex("init type 1", rc);
    }
    if (rc < 0 && rc != (int32_t)SCE_MUSIC_PLAYER_SERVICE_ERROR_ALREADY_INITIALIZED) {
        return (int)rc;
    }

    g_svc_inited = 1;
    return 0;
}

static void refresh_status_from_service(SceMusicPlayerServicePlayStatusExtension *out_ext) {
    sceClibMemset(out_ext, 0, sizeof(*out_ext));
    int32_t src = sceMusicPlayerServiceGetPlayStatusExtension(out_ext);
    if (src != 0) {
        qm_log_hex("GetPlayStatusExtension err", src);
        return;
    }

    char uri[SCE_MUSIC_MAX_STRING_LENGTH];
    sceClibMemset(uri, 0, sizeof(uri));
    int32_t urc = sceMusicPlayerServiceGetUri(uri, sizeof(uri));
    if (urc == 0 && uri[0] != '\0') {
        qm_strcpy(g_saved_uri, sizeof(g_saved_uri), uri);
    } else if (g_saved_uri[0] == '\0') {
        load_uri_from_bgmusic_state(g_saved_uri, sizeof(g_saved_uri));
    }

    g_has_state = 1;
    g_is_playing = (out_ext->currentState == SCE_MUSIC_COMMAND_PLAY) ? 1 : 0;
    if (out_ext->currentTimeMs > 0) {
        g_saved_pos_ms = out_ext->currentTimeMs;
    }
    if (out_ext->durationMs > 0) {
        g_saved_dur_ms = out_ext->durationMs;
    }
}

static void do_toggle_play_pause(void) {
    if (ensure_music_service() < 0) return;

    SceMusicPlayerServicePlayStatusExtension ext;
    refresh_status_from_service(&ext);

    if (ext.currentState == SCE_MUSIC_COMMAND_PLAY) {
        /* Pause: record exact timestamp, persist for ABSvita resync, then STOP */
        if (ext.currentTimeMs > 0) g_saved_pos_ms = ext.currentTimeMs;
        if (ext.durationMs > 0) g_saved_dur_ms = ext.durationMs;
        save_pos_file(g_saved_pos_ms, g_saved_uri);

        int32_t rc = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_STOP, 0);
        qm_log_hex("STOP", rc);
        g_is_playing = 0;
    } else {
        /* Resume: ensure URI is set, seek to saved position, then PLAY */
        char cur_uri[SCE_MUSIC_MAX_STRING_LENGTH];
        sceClibMemset(cur_uri, 0, sizeof(cur_uri));
        int32_t urc = sceMusicPlayerServiceGetUri(cur_uri, sizeof(cur_uri));

        if (urc != 0 || cur_uri[0] == '\0') {
            if (g_saved_uri[0] == '\0') {
                load_uri_from_bgmusic_state(g_saved_uri, sizeof(g_saved_uri));
            }
            if (g_saved_uri[0] != '\0') {
                SceMusicOpt opt;
                sceClibMemset(&opt, 0, sizeof(opt));
                opt.unk_00 = -1;
                int32_t set_rc = sceMusicPlayerServiceSetUri(g_saved_uri, &opt);
                qm_log_hex("SetUri", set_rc);
            }
        }

        if (g_saved_pos_ms == 0) {
            load_saved_pos_file();
        }

        sceMusicPlayerServiceSetRepeatMode(SCE_MUSIC_REPEAT_MODE_DISABLE);
        sceMusicPlayerServiceSetVolume(SCE_MUSIC_MAX_VOLUME);

        uint32_t seek_ms = g_saved_pos_ms;
        if (seek_ms > 0) {
            int32_t sk = sceMusicPlayerServiceSetSeekTime(seek_ms);
            qm_log_hex("pre-play SetSeekTime", sk);
        }

        int32_t prc = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_PLAY, 0);
        qm_log_hex("PLAY", prc);

        if (prc >= 0) {
            g_is_playing = 1;
            if (seek_ms > 0) {
                sceKernelDelayThread(200 * 1000);
                SceMusicPlayerServicePlayStatusExtension post;
                sceClibMemset(&post, 0, sizeof(post));
                if (sceMusicPlayerServiceGetPlayStatusExtension(&post) == 0 &&
                    post.currentTimeMs + 3000u < seek_ms) {
                    sceMusicPlayerServiceSetSeekTime(seek_ms);
                    int32_t sk2 = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_SEEK, 0);
                    qm_log_hex("post-play SEEK", sk2);
                }
            }
        }
    }
}

static void do_seek_delta(int forward) {
    if (ensure_music_service() < 0) return;

    SceMusicPlayerServicePlayStatusExtension ext;
    refresh_status_from_service(&ext);

    uint32_t base = (ext.currentState == SCE_MUSIC_COMMAND_PLAY && ext.currentTimeMs > 0)
                        ? ext.currentTimeMs
                        : g_saved_pos_ms;
    uint32_t dur = (ext.durationMs > 0) ? ext.durationMs : g_saved_dur_ms;

    uint32_t target = base;
    if (forward) {
        target = base + SKIP_MS;
        if (dur > 1000u && target > dur - 1000u) target = dur - 1000u;
    } else {
        target = (base > SKIP_MS) ? (base - SKIP_MS) : 0u;
    }

    g_saved_pos_ms = target;
    save_pos_file(target, g_saved_uri);

    if (ext.currentState == SCE_MUSIC_COMMAND_PLAY) {
        sceMusicPlayerServiceSetSeekTime(target);
        int32_t sk = sceMusicPlayerServiceSetPlaybackCommand(SCE_MUSIC_COMMAND_SEEK, 0);
        qm_log_hex("delta SEEK", sk);
    }
}

static int worker_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    qm_log("absvita_qm worker started");
    load_uri_from_bgmusic_state(g_saved_uri, sizeof(g_saved_uri));
    load_saved_pos_file();

    while (g_running) {
        if (sceKernelWaitSema(g_cmd_sema, 1, NULL) < 0) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        if (!g_running) break;

        int cmd = g_pending_cmd;
        g_pending_cmd = CMD_NONE;

        if (cmd == CMD_REFRESH) {
            if (ensure_music_service() == 0) {
                SceMusicPlayerServicePlayStatusExtension ext;
                refresh_status_from_service(&ext);
            }
        } else if (cmd == CMD_TOGGLE) {
            do_toggle_play_pause();
        } else if (cmd == CMD_SEEK_BACK) {
            do_seek_delta(0);
        } else if (cmd == CMD_SEEK_FWD) {
            do_seek_delta(1);
        }
    }

    return sceKernelExitDeleteThread(0);
}

static void dispatch_cmd(int cmd) {
    g_pending_cmd = cmd;
    if (g_cmd_sema >= 0) {
        sceKernelSignalSema(g_cmd_sema, 1);
    }
}

static void format_title_label(char *buf, size_t sz, uint32_t pos_ms) {
    if (pos_ms == 0) {
        qm_strcpy(buf, sz, "ABSvita");
        return;
    }
    uint32_t sec = pos_ms / 1000u;
    uint32_t h = sec / 3600u;
    uint32_t m = (sec % 3600u) / 60u;
    uint32_t s = sec % 60u;
    if (h > 0) {
        sceClibSnprintf(buf, sz, "ABSvita  %u:%02u:%02u", h, m, s);
    } else {
        sceClibSnprintf(buf, sz, "ABSvita  %02u:%02u", m, s);
    }
}

static void update_ui_labels(int playing_hint, uint32_t pos_ms) {
    char title[64];
    format_title_label(title, sizeof(title), pos_ms);
    QuickMenuRebornSetWidgetLabel(TITLE_ID, title);

    if (g_has_state) {
        QuickMenuRebornSetWidgetLabel(BTN_TOGGLE_ID, playing_hint ? "Pause" : "Play");
    } else {
        QuickMenuRebornSetWidgetLabel(BTN_TOGGLE_ID, "Play / Pause");
    }
}

ONLOAD_HANDLER(on_qm_load) {
    (void)id;
    update_ui_labels(g_is_playing, g_saved_pos_ms);
    dispatch_cmd(CMD_REFRESH);
}

BUTTON_HANDLER(on_toggle_press) {
    (void)id; (void)hash; (void)eventId; (void)userDat;
    int next_playing = g_has_state ? !g_is_playing : 0;
    if (g_has_state) {
        QuickMenuRebornSetWidgetLabel(BTN_TOGGLE_ID, next_playing ? "Pause" : "Play");
    }
    dispatch_cmd(CMD_TOGGLE);
}

BUTTON_HANDLER(on_rew_press) {
    (void)id; (void)hash; (void)eventId; (void)userDat;
    uint32_t pos = g_saved_pos_ms;
    pos = (pos > SKIP_MS) ? (pos - SKIP_MS) : 0u;
    update_ui_labels(g_is_playing, pos);
    dispatch_cmd(CMD_SEEK_BACK);
}

BUTTON_HANDLER(on_fwd_press) {
    (void)id; (void)hash; (void)eventId; (void)userDat;
    uint32_t pos = g_saved_pos_ms + SKIP_MS;
    if (g_saved_dur_ms > 1000u && pos > g_saved_dur_ms - 1000u) {
        pos = g_saved_dur_ms - 1000u;
    }
    update_ui_labels(g_is_playing, pos);
    dispatch_cmd(CMD_SEEK_FWD);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    g_cmd_sema = sceKernelCreateSema("absvita_qm_sema", 0, 0, 1, NULL);
    g_worker_thid = sceKernelCreateThread(
        "absvita_qm_worker", worker_thread, 0x10000100, 32 * 1024, 0, 0, NULL);
    if (g_worker_thid >= 0) {
        sceKernelStartThread(g_worker_thid, 0, NULL);
    }

    QuickMenuRebornSeparator(SEP_ID, SCE_SEPARATOR_HEIGHT);

    QuickMenuRebornRegisterWidget(TITLE_ID, NULL, text);
    QuickMenuRebornSetWidgetSize(TITLE_ID, SCE_PLANE_WIDTH, 44.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetColor(TITLE_ID, 1.0f, 1.0f, 1.0f, 1.0f);
    QuickMenuRebornSetWidgetPosition(TITLE_ID, 0.0f, 0.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetLabel(TITLE_ID, "ABSvita");

    QuickMenuRebornRegisterWidget(ROW_PLANE_ID, NULL, plane);
    QuickMenuRebornSetWidgetSize(ROW_PLANE_ID, SCE_PLANE_WIDTH, 92.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetColor(ROW_PLANE_ID, 1.0f, 1.0f, 1.0f, 0.0f);

    QuickMenuRebornRegisterWidget(BTN_REW_ID, ROW_PLANE_ID, button);
    QuickMenuRebornSetWidgetSize(BTN_REW_ID, 190.0f, 72.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetColor(BTN_REW_ID, 1.0f, 1.0f, 1.0f, 1.0f);
    QuickMenuRebornSetWidgetPosition(BTN_REW_ID, -260.0f, 0.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetLabel(BTN_REW_ID, "-15s");
    QuickMenuRebornRegisterEventHanlder(BTN_REW_ID, QMR_BUTTON_RELEASE_ID, on_rew_press, NULL);

    QuickMenuRebornRegisterWidget(BTN_TOGGLE_ID, ROW_PLANE_ID, button);
    QuickMenuRebornSetWidgetSize(BTN_TOGGLE_ID, 260.0f, 72.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetColor(BTN_TOGGLE_ID, 1.0f, 1.0f, 1.0f, 1.0f);
    QuickMenuRebornSetWidgetPosition(BTN_TOGGLE_ID, 0.0f, 0.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetLabel(BTN_TOGGLE_ID, "Play / Pause");
    QuickMenuRebornRegisterEventHanlder(BTN_TOGGLE_ID, QMR_BUTTON_RELEASE_ID, on_toggle_press, NULL);
    QuickMenuRebornAssignOnLoadHandler(on_qm_load, BTN_TOGGLE_ID);

    QuickMenuRebornRegisterWidget(BTN_FWD_ID, ROW_PLANE_ID, button);
    QuickMenuRebornSetWidgetSize(BTN_FWD_ID, 190.0f, 72.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetColor(BTN_FWD_ID, 1.0f, 1.0f, 1.0f, 1.0f);
    QuickMenuRebornSetWidgetPosition(BTN_FWD_ID, 260.0f, 0.0f, 0.0f, 0.0f);
    QuickMenuRebornSetWidgetLabel(BTN_FWD_ID, "+15s");
    QuickMenuRebornRegisterEventHanlder(BTN_FWD_ID, QMR_BUTTON_RELEASE_ID, on_fwd_press, NULL);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    g_running = 0;
    if (g_cmd_sema >= 0) {
        sceKernelSignalSema(g_cmd_sema, 1);
        sceKernelDelayThread(50 * 1000);
        sceKernelDeleteSema(g_cmd_sema);
        g_cmd_sema = -1;
    }

    QuickMenuRebornUnregisterWidget(BTN_REW_ID);
    QuickMenuRebornUnregisterWidget(BTN_TOGGLE_ID);
    QuickMenuRebornUnregisterWidget(BTN_FWD_ID);
    QuickMenuRebornUnregisterWidget(ROW_PLANE_ID);
    QuickMenuRebornUnregisterWidget(TITLE_ID);
    QuickMenuRebornRemoveSeparator(SEP_ID);

    return SCE_KERNEL_STOP_SUCCESS;
}
