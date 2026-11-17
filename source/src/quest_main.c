/* Meta Quest (standalone) frontend: Android NativeActivity + OpenXR on OpenGL ES.
 *
 * The game is shown on a large screen floating in front of the player, each eye seeing its own picture of the
 * stereo 3D mode, and the two hand controllers are the handlebars - the same as the Windows VR mode (win32_vr.c),
 * but everything runs on the headset.
 *
 * Threads:
 *   main (android_main)   the headset: OpenXR frames at the headset's rate, the controllers, the newest picture
 *   game                  the machine by the clock (60.05 frames a second), sound, the menus
 *   eye worker            the right eye's picture while the game thread draws the left (sho_stereo_set_worker)
 *   (AAudio's callback)   takes the sound out of a ring buffer
 *
 * Files are in the app's own folder on the headset, which a PC sees over USB as
 *   Quest 3 > Internal shared storage > Android > data > com.shangon.ultimate > files
 * shangon.zip (the arcade ROM) goes there; optionally the Mega Drive ROM, a music folder, and shangon.ini.
 * shangon.log is written there.
 *
 * Build: quest/build-quest.ps1 (Android NDK; no Gradle, no Java code). */
#include <jni.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/asset_manager.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include "third_party/openxr/openxr.h"
#include "third_party/openxr/openxr_platform.h"
#include "sho_core.h"
#include "sho_md.h"
#include "sho_mdgfx.h"
#include "sho_views.h"
#include "sho_pace.h"
#include "ini_file.h"
#include "launcher_core.h"
#include "sho_music.h"

#define TAG "SuperHangOn"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define PATHN 1024
#define AUDIO_RATE 48000
#define AUDIO_TARGET 3200            /* samples queued ahead (~67 ms) */
#define PIC_MAX_W (2 * SHO_MAX_W * SHO_3D_SUB)
#define TEX_SCALE 4                  /* headset pictures: columns and rows per game pixel (less if the headset's limit says so) */

extern int sho_recomp;

/* ------------------------------------------------------------------ */
/* shared between the threads (under g_lock)                           */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pic[PIC_MAX_W * SHO_H]; static int g_pic_fw, g_pic_eye_w, g_pic_depth, g_pic_seq; static double g_pic_aspect;   /* the newest picture */
static sho_input g_pad;                          /* the controllers as of the headset's last frame */
static int g_menu_flag;                          /* the game has a menu up */
static int g_y_edge;                             /* Y was pressed (the settings menu) */
static int g_want_recenter = 1;
/* settings the menus change (centimetres / degrees / choices) */
static int g_scr_height = -10, g_scr_dist = 200, g_scr_width = 320, g_steer_stick, g_tilt_angle = 30, g_swap_trig, g_twist, g_twist_angle = 40;
static int g_cfg_moved, g_cfg_idle;              /* the right stick moved the screen: save when it has rested */
static volatile int g_running = 1, g_save_now, g_session_running;
static char g_dir[PATHN];
static AAssetManager *g_assets;
static uint32_t *g_lpic; static int g_lpic_seq, g_launcher_on;   /* the launcher's picture (LC_IW x LC_IH), while it is up */

/* start-up and headset events go to logcat, to shangon.log in the game's folder and to a copy in the headset's
   Download folder (which a PC sees over a plain USB file connection, and the headset's Files app shows) */
void sho_diag_mirror(const char *path);
static void qlog(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", b);
    sho_diag("%s", b);
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec * 1e-9; }
static void path_join(char *out, const char *dir, const char *name) { snprintf(out, PATHN, "%s/%s", dir, name); }
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ------------------------------------------------------------------ */
/* a message picture (before the arcade's own letters can be used)     */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[][5] = {   /* ' ' - '_' (columns, bit 0 = top) */
    {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},{0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},
    {0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,8,0x3e,8,0x14},{8,8,0x3e,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
    {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},
    {0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0xc,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},{0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7f,0x41,0x41,0},{2,4,8,0x10,0x20},{0,0x41,0x41,0x7f,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
};
static uint32_t g_msg_fb[SHO_W * SHO_H];
static void msg_text(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 6) {
        int ch = (unsigned char)*s; if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) continue;
        for (int i = 0; i < 5; i++) for (int j = 0; j < 7; j++)
            if ((font5x7[ch - 32][i] >> j) & 1) { int X = x + i, Y = y + j; if (X >= 0 && X < SHO_W && Y >= 0 && Y < SHO_H) g_msg_fb[Y * SHO_W + X] = c; }
    }
}
static void publish(const uint32_t *fb, int fw, int eye_w, double aspect, int depth) {
    if (!fb || fw <= 0 || fw > PIC_MAX_W) return;
    pthread_mutex_lock(&g_lock);
    memcpy(g_pic, fb, (size_t)fw * SHO_H * 4);
    g_pic_fw = fw; g_pic_eye_w = eye_w; g_pic_aspect = aspect; g_pic_depth = depth; g_pic_seq = (g_pic_seq + 1) & 0x3fffffff;
    pthread_mutex_unlock(&g_lock);
}
/* up to 8 lines, centred, shown flat to both eyes */
static void show_message(const char *const *lines, int n) {
    for (int i = 0; i < SHO_W * SHO_H; i++) g_msg_fb[i] = 0x101828;
    for (int i = 0; i < n; i++) msg_text((SHO_W - (int)strlen(lines[i]) * 6) / 2, 112 - n * 6 + i * 12, lines[i], i == 0 ? 0xffe020 : 0xe0e0e0);
    publish(g_msg_fb, SHO_W, SHO_W, 4.0 / 3.0, 0);
}

/* ------------------------------------------------------------------ */
/* sound: the game thread writes a ring, AAudio's callback empties it  */
/* ------------------------------------------------------------------ */
#define RING 32768                   /* frames (a power of 2) */
static int16_t g_ring[RING * 2]; static volatile unsigned g_ring_w, g_ring_r;
static AAudioStream *g_audio;
static long audio_queued(void) { return (long)(unsigned)(__atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE) - __atomic_load_n(&g_ring_r, __ATOMIC_ACQUIRE)); }
static void audio_submit(const int16_t *s, int n) {
    unsigned w = g_ring_w;
    if (audio_queued() + n > RING - 64) return;
    for (int i = 0; i < n; i++, w++) { g_ring[(w & (RING - 1)) * 2] = s[i * 2]; g_ring[(w & (RING - 1)) * 2 + 1] = s[i * 2 + 1]; }
    __atomic_store_n(&g_ring_w, w, __ATOMIC_RELEASE);
}
static aaudio_data_callback_result_t audio_cb(AAudioStream *st, void *user, void *data, int32_t frames) {
    (void)st; (void)user;
    int16_t *o = (int16_t *)data; unsigned r = g_ring_r; long have = audio_queued();
    for (int i = 0; i < frames; i++) {
        if (i < have) { o[i * 2] = g_ring[(r & (RING - 1)) * 2]; o[i * 2 + 1] = g_ring[(r & (RING - 1)) * 2 + 1]; r++; }
        else { o[i * 2] = 0; o[i * 2 + 1] = 0; }
    }
    __atomic_store_n(&g_ring_r, r, __ATOMIC_RELEASE);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static int audio_init(void) {
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, AUDIO_RATE);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_audio);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !g_audio) { g_audio = NULL; return 0; }
    if (AAudioStream_requestStart(g_audio) != AAUDIO_OK) { AAudioStream_close(g_audio); g_audio = NULL; return 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the eye worker (the right eye's picture on a second core)           */
/* ------------------------------------------------------------------ */
static sem_t g_ew_go, g_ew_done; static void (*g_ew_job)(void);
static void thread_kind(int kind);
static void *eye_worker(void *a) { thread_kind(1); for (;;) { sem_wait(&g_ew_go); if (g_ew_job) g_ew_job(); sem_post(&g_ew_done); } return a; }
static void eye_worker_start(void (*job)(void)) { g_ew_job = job; sem_post(&g_ew_go); }
static void eye_worker_wait(void) { sem_wait(&g_ew_done); }

/* ------------------------------------------------------------------ */
/* the game thread                                                     */
/* ------------------------------------------------------------------ */
static char g_nv[PATHN], g_md_path[PATHN]; static int g_md_present, g_shape;
static void load_nvram(void) {
    FILE *f = fopen(g_nv, "rb"); if (!f) return; size_t n = fread(sho_nvram(), 1, sho_nvram_size(), f); (void)n; fclose(f);
    sho_nvram()[0x6052] = 0; sho_nvram()[0x6053] = 0;   /* (no credits kept: see linux_main.c) */
}
static void save_nvram(void) { FILE *f = fopen(g_nv, "wb"); if (!f) return; fwrite(sho_nvram(), 1, sho_nvram_size(), f); fclose(f); }
static void apply_game_settings(void) {
    int demo = ini_int("game", "demo_sounds", 1), diff = ini_int("game", "difficulty", 1), tim = ini_int("game", "time", 1);
    int free_play = ini_int("game", "free_play", 0);
    static const uint8_t dmap[4] = { 0x04, 0x06, 0x02, 0x00 }, tmap[4] = { 0x10, 0x18, 0x08, 0x00 };
    uint8_t dsw = 0xe0 | (demo ? 0 : 1) | dmap[diff & 3] | tmap[tim & 3];
    sho_set_dips(free_play ? 0x00 : 0xff, dsw);
}
static int md_on(void) { return g_md_present && ini_int("megadrive", "enhancements", 1); }
static void reset_to_title(void) {
    sho_md_before_reset();
    save_nvram(); sho_reset(); load_nvram(); apply_game_settings();
    sho_md_configure(md_on(), g_md_present && ini_int("megadrive", "controls", 0));
}
static void set_shape(int i) {
    g_shape = ((i % SHO_NSHAPES) + SHO_NSHAPES) % SHO_NSHAPES;
    sho_pause_set_display(sho_shapes[g_shape].name);
    sho_set_view_width(sho_shapes[g_shape].width);
    ini_set("video", "aspect", sho_shapes[g_shape].name);
}
static void save_stereo(void) {
    sho_stereo st; sho_get_stereo(&st);
    ini_set_int("video", "3d", st.mode); ini_set_int("video", "3d_depth", st.depth); ini_set_int("video", "3d_pop", st.pop); ini_set_int("video", "3d_swap", st.swap);
}
static void save_vr(void) {
    ini_set_int("vr", "screen_height", g_scr_height); ini_set_int("vr", "screen_distance", g_scr_dist); ini_set_int("vr", "screen_width", g_scr_width);
    ini_set_int("vr", "steering", g_steer_stick); ini_set_int("vr", "steer_angle", g_tilt_angle); ini_set_int("vr", "swap_triggers", g_swap_trig); ini_set_int("vr", "throttle", g_twist);
}
static void menu_input(int k) { int ch = sho_stereo_menu_key(k); if (ch == 1) save_stereo(); if (ch == 2) save_vr(); }
/* the Mega Drive ROM, if one is in the folder (any .md / .gen / .bin / .zip that checks out) */
static int find_md_rom(char *out) {
    char p[PATHN], msg[128];
    static const char *exts[] = { ".md", ".gen", ".bin", ".zip" };
    for (int i = 0; i < 4; i++) {
        DIR *d = opendir(g_dir); if (!d) return 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name; size_t l = strlen(nm), el = strlen(exts[i]);
            if (nm[0] == '.' || l <= el || strcasecmp(nm + l - el, exts[i]) != 0 || !strcasecmp(nm, "shangon.zip")) continue;
            path_join(p, g_dir, nm);
            struct stat st; if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 8 * 1024 * 1024) continue;
            if (mdgfx_check(p, msg, sizeof msg)) { closedir(d); snprintf(out, PATHN, "%s", p); return 1; }
        }
        closedir(d);
    }
    return 0;
}

/* Files packed into the APK (the build script puts the player's ROMs there, so nothing has to be copied to the
   headset by hand): each is written to the game's folder once, if it is not there already. */
static void unpack_assets(void) {
    if (!g_assets) return;
    AAssetDir *d = AAssetManager_openDir(g_assets, ""); if (!d) return;
    const char *nm; static char buf[65536];
    while ((nm = AAssetDir_getNextFileName(d)) != NULL) {
        char p[PATHN], tmp[PATHN]; path_join(p, g_dir, nm);
        if (!strcmp(nm, "launcher_bg.png") || file_exists(p)) continue;   /* (the launcher's picture is read from the APK) */
        AAsset *a = AAssetManager_open(g_assets, nm, AASSET_MODE_STREAMING); if (!a) continue;
        snprintf(tmp, sizeof tmp, "%s.part", p);
        FILE *f = fopen(tmp, "wb"); int n, ok = f != NULL;
        while (ok && (n = AAsset_read(a, buf, sizeof buf)) > 0) if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) ok = 0;
        if (f) fclose(f);
        AAsset_close(a);
        if (ok) rename(tmp, p); else remove(tmp);
        sho_diag("unpacked %s: %s", nm, ok ? "ok" : "failed");
    }
    AAssetDir_close(d);
}
/* ---- the launcher (launcher_core.c, as on the PC): its picture on the big screen, the controllers as its keys ---- */
static void publish_launcher(int force) {
    static double last;
    double t = now_s();
    if (!force && t - last < 1.0 / 30) return;
    last = t;
    const uint32_t *img = lc_image(); if (!img || !g_lpic) return;
    pthread_mutex_lock(&g_lock);
    memcpy(g_lpic, img, (size_t)LC_IW * LC_IH * 4); g_lpic_seq++; g_launcher_on = 1;
    pthread_mutex_unlock(&g_lock);
}
static void present_loading(void) { publish_launcher(1); }
static int host_pad_count(void) { return 0; }
static const char *host_pad_name(int i) { (void)i; return ""; }
static int host_pad_read(int i, sho_pad *out) { (void)i; (void)out; return 0; }
static const char *host_key_name(int code) { (void)code; return ""; }
static void host_save(void) {}
static const lc_controls_host g_controls_host = { host_pad_count, host_pad_name, host_pad_read, host_key_name, host_save };
static char g_rom_err[2048];
static int rom_check(char *msg, int msglen, char *detail, int detlen) {
    char p[PATHN]; path_join(p, g_dir, "shangon.zip");
    if (!file_exists(p)) {
        snprintf(msg, msglen, "shangon.zip is not in the game's folder");
        snprintf(detail, detlen, "Build the APK with it next to build-quest.ps1");
        snprintf(g_rom_err, sizeof g_rom_err, "shangon.zip is not in the folder");
        return 0;
    }
    if (sho_load_roms(p, g_rom_err, sizeof g_rom_err) != 0) {
        snprintf(msg, msglen, "shangon.zip could not be used");
        snprintf(detail, detlen, "%.120s", g_rom_err);
        return 0;
    }
    snprintf(msg, msglen, "ROM OK: shangon.zip verified");
    snprintf(detail, detlen, "All 25 files match Super Hang-On (MAME 'shangon')");
    g_rom_err[0] = 0;
    return 1;
}
static int md_rom_check(char *msg, int msglen, char *detail, int detlen) {
    char p[PATHN];
    if (find_md_rom(p)) {
        const char *nm = strrchr(p, '/'); nm = nm ? nm + 1 : p;
        snprintf(msg, msglen, "Mega Drive ROM OK: %.80s verified", nm);
        snprintf(detail, detlen, "Super Hang-On (World) - Mega Drive options available");
        return 1;
    }
    snprintf(msg, msglen, "No Mega Drive ROM found");
    snprintf(detail, detlen, "Build the APK with it next to build-quest.ps1 to enable its options");
    return 0;
}
/* Original Mode's screens and courses from the Mega Drive ROM: read while the launcher is up (the arcade has not
   started yet, so the shared 68000 core is free) */
static volatile int g_md_busy;
static void *md_capture_thread(void *a) {
    char merr[256]; double t0 = now_s();
    int ok = mdgfx_capture(g_md_path, merr, sizeof merr) != 0;
    if (!ok) qlog("game: Mega Drive ROM not used: %s", merr); else qlog("game: Original Mode ready (%.1f s)", now_s() - t0);
    __atomic_store_n(&g_md_busy, 0, __ATOMIC_RELEASE);
    return a;
}
/* the launcher until PLAY: 1 = play, 0 = quit */
static int run_launcher(void) {
    g_lpic = (uint32_t *)malloc((size_t)LC_IW * LC_IH * 4);
    if (!g_lpic) return 1;
    launcher_settings ls; memset(&ls, 0, sizeof ls);
    static const char *views[SHO_NSHAPES];
    for (int i = 0; i < SHO_NSHAPES; i++) views[i] = sho_shapes[i].long_name;
    ls.view_names = views; ls.view_count = SHO_NSHAPES; ls.view_desc = "THE SHAPE OF THE SCREEN";
    ls.demo_sounds = ini_int("game", "demo_sounds", 1);
    ls.difficulty = ini_int("game", "difficulty", 1) & 3;
    ls.time_adj = ini_int("game", "time", 1) & 3;
    {   char a[32]; ini_str("video", "aspect", "16:9", a, sizeof a); int sh = sho_shape_find(a); ls.view = sh < 0 ? 0 : sh; }
    ls.arrange_music = ini_int("audio", "external_music", 1) != 0;
    ls.md_enhancements = ini_int("megadrive", "enhancements", 1) != 0;
    ls.md_controls = ini_int("megadrive", "controls", 0) != 0;
    ls.md_music = ini_int("megadrive", "music", 0) != 0;
    ls.game_dir = "ANDROID/DATA/COM.SHANGON.ULTIMATE/FILES";
    int found = 0;
    for (int i = 0; i < 4; i++) { char n[128], mp[PATHN]; snprintf(n, sizeof n, "music/%s.mp3", sho_music_tracks[i].title); path_join(mp, g_dir, n); if (file_exists(mp)) found++; }
    if (found == 4) snprintf(ls.music_note, sizeof ls.music_note, "Arrange: all 4 tracks found in the music folder");
    else if (found) snprintf(ls.music_note, sizeof ls.music_note, "Arrange: %d of 4 tracks found (others play regular)", found);
    else snprintf(ls.music_note, sizeof ls.music_note, "Arrange tracks not found: regular music will play");
    ls.md_check = md_rom_check;
    pthread_t mdt; int md_thread = 0;
    if (ls.md_enhancements && find_md_rom(g_md_path)) {
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        g_md_busy = 1; md_thread = pthread_create(&mdt, &a, md_capture_thread, NULL) == 0;
        if (!md_thread) g_md_busy = 0;
        pthread_attr_destroy(&a);
    }
    {   /* the launcher picture (res/launcher_bg.png, packed into the APK) */
        unsigned char *png = NULL; size_t png_len = 0;
        AAsset *as = g_assets ? AAssetManager_open(g_assets, "launcher_bg.png", AASSET_MODE_BUFFER) : NULL;
        if (as) { off_t n = AAsset_getLength(as); png = (unsigned char *)malloc((size_t)n); if (png && AAsset_read(as, png, (size_t)n) == (int)n) png_len = (size_t)n; AAsset_close(as); }
        lc_set_controls_host(&g_controls_host);
        lc_begin(&ls, rom_check, png, png_len);
    }
    lc_clock = now_s; lc_present_hook = present_loading;
    qlog("game: launcher up");
    int prev[7] = { 1, 1, 1, 1, 1, 1, 1 }, held[4] = { 0, 0, 0, 0 };
    while (g_running && lc_result() == LC_RUNNING) {
        sho_input in;
        pthread_mutex_lock(&g_lock); in = g_pad; g_menu_flag = 1; g_y_edge = 0; pthread_mutex_unlock(&g_lock);
        int d[4] = { in.nav_up, in.nav_down, in.nav_left, in.nav_right };
        static const int dk[4] = { LK_UP, LK_DOWN, LK_LEFT, LK_RIGHT };
        for (int i = 0; i < 4; i++) {   /* the left stick: a push, then repeating while held */
            if (!d[i]) { held[i] = 0; continue; }
            int h = ++held[i]; if (h == 1 || (h > 24 && h % 5 == 0)) lc_key(dk[i]);
        }
        int b[3] = { in.pad_a, in.start, in.pad_b };
        if (b[0] && !prev[4]) lc_key(LK_PAD_A);                         /* A (or the trigger): change / PLAY */
        if (b[1] && !prev[5]) lc_key(LK_PAD_START);                     /* Menu: PLAY from any line */
        if (b[2] && !prev[6] && lc_page() != 0) lc_key(LK_ESC);        /* B: back out of the controls screen (not quit) */
        prev[4] = b[0]; prev[5] = b[1]; prev[6] = b[2];
        lc_tick(now_s());
        publish_launcher(0);
        struct timespec ts = { 0, 16 * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
    if (!g_running || lc_result() != LC_PLAY) {
        if (md_thread) pthread_join(mdt, NULL);
        lc_end(); return 0;
    }
    lc_finish(&ls);
    ini_set_int("audio", "external_music", ls.arrange_music);
    ini_set_int("game", "demo_sounds", ls.demo_sounds);
    ini_set_int("game", "difficulty", ls.difficulty);
    ini_set_int("game", "time", ls.time_adj);
    ini_set("video", "aspect", sho_shapes[ls.view >= 0 && ls.view < SHO_NSHAPES ? ls.view : 0].name);
    ini_set_int("megadrive", "enhancements", ls.md_enhancements);
    ini_set_int("megadrive", "controls", ls.md_controls);
    ini_set_int("megadrive", "music", ls.md_music);
    {   int have[4];
        for (int i = 0; i < 4; i++) { char n[128], mp[PATHN]; snprintf(n, sizeof n, "music/%s.mp3", sho_music_tracks[i].title); path_join(mp, g_dir, n); have[i] = ls.arrange_music && file_exists(mp); }
        lc_load_begin(have, md_thread && g_md_busy);
    }
    lc_load_step(0, 0, "CHECKING THE ARCADE ROM");
    {   char m[256], dt[256]; rom_check(m, sizeof m, dt, sizeof dt); }
    if (ini_int("audio", "external_music", 1)) {
        char md[PATHN], me[512]; path_join(md, g_dir, "music");
        if (file_exists(md)) { sho_music_load_progress = lc_music_progress; sho_load_music(md, AUDIO_RATE, me, sizeof me); sho_music_load_progress = NULL; }
    }
    {   char tmp[PATHN]; g_md_present = find_md_rom(tmp); }   /* the Mega Drive options only apply with the Mega Drive ROM there */
    if (md_thread) {
        while (g_md_busy) { lc_load_step(2, 0, "PREPARING ORIGINAL MODE"); struct timespec ts = { 0, 20 * 1000 * 1000 }; nanosleep(&ts, NULL); }
        pthread_join(mdt, NULL);
    } else if (ls.md_enhancements && find_md_rom(g_md_path)) {   /* turned on in the launcher: read it now */
        lc_load_step(2, 0, "PREPARING ORIGINAL MODE");
        g_md_busy = 1; md_capture_thread(NULL);
    }
    return 1;
}
static void launcher_done(void) {
    if (!g_launcher_on) return;
    lc_load_step(3, 1.0f, "STARTING THE GAME"); lc_present_hook = NULL; lc_end();
    pthread_mutex_lock(&g_lock); g_launcher_on = 0; pthread_mutex_unlock(&g_lock);
}
static void *game_thread(void *arg) {
    (void)arg;
    char p[PATHN], err[2048];
    thread_kind(0);
    qlog("game: starting");
    unpack_assets();
    path_join(p, g_dir, "shangon.ini"); ini_set_path(p);
    path_join(g_nv, g_dir, "shangon.nv");
    sho_recomp = 1;

    int launcher = ini_int("launcher", "show", 1);
    if (launcher && !run_launcher()) { qlog("game: quit from the launcher"); g_running = 0; return NULL; }
    /* the arcade ROM: wait for it if it is not there yet (the launcher has checked it already) */
    for (int tries = 0; g_running && !launcher; tries++) {
        path_join(p, g_dir, "shangon.zip");
        err[0] = 0;
        if (file_exists(p) && sho_load_roms(p, err, sizeof err) == 0) break;
        if (tries == 0) sho_diag("ROM: %s", file_exists(p) ? err : "shangon.zip is not in the folder");
        const char *lines[8] = { "SUPER HANG-ON ULTIMATE", "", file_exists(p) ? "SHANGON.ZIP COULD NOT BE USED" : "SHANGON.ZIP IS NOT THERE YET", "",
                                 "BUILD THE APK WITH SHANGON.ZIP NEXT TO THE BUILD SCRIPT", "OR COPY IT TO ANDROID / DATA / COM.SHANGON.ULTIMATE / FILES", "",
                                 "THE GAME STARTS BY ITSELF WHEN IT IS THERE" };
        show_message(lines, 8);
        struct timespec ts = { 2, 0 }; nanosleep(&ts, NULL);
    }
    if (!g_running) return NULL;
    qlog("game: arcade ROM loaded");
    if (!launcher) {
    {   const char *lines[3] = { "SUPER HANG-ON ULTIMATE", "", "STARTING" }; show_message(lines, 3); }
    if (ini_int("audio", "external_music", 1)) { char md[PATHN], me[512]; path_join(md, g_dir, "music"); if (file_exists(md)) sho_load_music(md, AUDIO_RATE, me, sizeof me); }
    {   char tmp[PATHN]; g_md_present = find_md_rom(tmp); }
    if (ini_int("megadrive", "enhancements", 1) && find_md_rom(g_md_path)) {   /* Original Mode from the Mega Drive ROM */
        const char *lines[5] = { "SUPER HANG-ON ULTIMATE", "", "PREPARING ORIGINAL MODE", "", "(THE FIRST START TAKES LONGEST)" }; show_message(lines, 5);
        qlog("game: reading the Mega Drive ROM %s", g_md_path);
        g_md_busy = 1; md_capture_thread(NULL);
    }
    }
    {   int m = ini_int("audio", "music_level", 10), s = ini_int("audio", "sfx_level", 10); sho_set_sound_levels(m, s); }
    sho_reset();
    load_nvram();
    apply_game_settings();
    {   char sav[PATHN]; path_join(sav, g_dir, "shangon_original.sav"); sho_md_set_save_path(sav);
        sho_md_configure(md_on(), g_md_present && ini_int("megadrive", "controls", 0));
        sho_md_set_music(md_on() && ini_int("megadrive", "music", 0));
        if (sho_md_music_active()) sho_silence_music(); }
    {   char a[32]; ini_str("video", "aspect", "16:9", a, sizeof a); int s = sho_shape_find(a); set_shape(s < 0 ? 0 : s); }
    {   sho_stereo st = { ini_int("video", "3d", SHO_3D_FULL_SBS), ini_int("video", "3d_depth", 13), ini_int("video", "3d_pop", 8), ini_int("video", "3d_swap", 0) };
        sho_set_stereo(&st); }
    sho_pause_enable_3d(1);
    sho_stereo_menu_hint("STICK CHANGES Y CLOSES");
    {   /* the screen and the controllers: in the settings menu (Y) */
        static const char *const steer_names[2] = { "TILT", "STICK" }, *const trig_names[2] = { "NORMAL", "SWAPPED" }, *const thr_names[2] = { "TRIGGER", "TWIST" };
        pthread_mutex_lock(&g_lock);
        g_scr_dist = ini_int("vr", "screen_distance", 200); g_scr_width = ini_int("vr", "screen_width", 320);
        { char hs[32]; ini_str("vr", "screen_height", "-10", hs, sizeof hs); g_scr_height = atoi(hs); }
        g_steer_stick = ini_int("vr", "steering", 0) != 0; g_tilt_angle = ini_int("vr", "steer_angle", 30); g_swap_trig = ini_int("vr", "swap_triggers", 0) != 0;
        g_twist = ini_int("vr", "throttle", 0) != 0; g_twist_angle = ini_int("vr", "twist_angle", 40);
        if (g_tilt_angle < 10) g_tilt_angle = 10; if (g_tilt_angle > 60) g_tilt_angle = 60;
        pthread_mutex_unlock(&g_lock);
        sho_stereo_menu_extra("SCREEN HEIGHT", &g_scr_height, -300, 300, 5);
        sho_stereo_menu_extra("SCREEN DIST", &g_scr_dist, 50, 2000, 10);
        sho_stereo_menu_extra("SCREEN WIDTH", &g_scr_width, 50, 4000, 10);
        sho_stereo_menu_extra_choice("THROTTLE", &g_twist, 2, thr_names);
        sho_stereo_menu_extra_choice("STEERING", &g_steer_stick, 2, steer_names);
        sho_stereo_menu_extra("TILT ANGLE", &g_tilt_angle, 10, 60, 5);
        sho_stereo_menu_extra_choice("TRIGGERS", &g_swap_trig, 2, trig_names);
    }
    {   pthread_t t; sem_init(&g_ew_go, 0, 0); sem_init(&g_ew_done, 0, 0);
        if (pthread_create(&t, NULL, eye_worker, NULL) == 0) sho_stereo_set_worker(eye_worker_start, eye_worker_wait); }

    launcher_done();
    int have_audio = audio_init();
    if (!have_audio) qlog("sound: AAudio could not be started");
    qlog("game: running");
    static int16_t samples[2 * 1100];
    double period = 1.0 / SHO_FPS, next_frame = now_s(), audio_frac = 0;
    int audio_target = AUDIO_TARGET, paused = 0, pause_menu = 0, ptick = 0, start_prev = 0, frames_since_save = 0, want_pause3d = 0, close_pause = 0, underruns = 0;
    int had_session = 0;
    long game_frames = 0, diag_frames = 0; double diag_t = now_s(), diag_t0 = diag_t, tm_game = 0, tm_pic = 0; int tm_frames = 0, tm_loops = 0;

    while (g_running) {
        double t = now_s();
        if (t < next_frame) { struct timespec ts; double d = next_frame - t; ts.tv_sec = (time_t)d; ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9); nanosleep(&ts, NULL); t = now_s(); }
        int frames = 1; next_frame += period;
        while (t >= next_frame && frames < 3) { frames++; next_frame += period; }   /* a slow pass is made up for */
        if (t - next_frame > 0.1) next_frame = t;
        if (g_session_running) had_session = 1;
        else if (had_session) {      /* the headset is off the head or asleep: the game waits (nobody sees or hears it) */
            struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); next_frame = now_s(); continue;
        }

        double tg0 = now_s();
        for (int f = 0; f < frames; f++) {
            sho_input in; memset(&in, 0, sizeof in);
            int y_edge;
            pthread_mutex_lock(&g_lock);
            in = g_pad; y_edge = g_y_edge; g_y_edge = 0;
            g_menu_flag = sho_md_overlay_active() || pause_menu || sho_stereo_menu_shown();
            int moved = g_cfg_moved && g_cfg_idle >= 30; if (moved) g_cfg_moved = 0;
            pthread_mutex_unlock(&g_lock);
            if (moved) ini_set_int("vr", "screen_height", g_scr_height);   /* the right stick moved the screen: keep it */

            /* Y: the settings. In an Original Mode race they are the 3D page of the game's pause menu, elsewhere the small menu */
            if (y_edge) {
                if (sho_stereo_menu_shown()) sho_stereo_menu(0);
                else if (paused && pause_menu) close_pause = 1;
                else if (!paused && sho_md_pause_allowed()) want_pause3d = 1;
                else sho_stereo_menu(1);
            }
            if (sho_stereo_menu_shown()) {   /* the left stick works it (first push, then repeating); B closes it */
                static int held[4]; int d[4] = { in.nav_up, in.nav_down, in.nav_left, in.nav_right };
                for (int i = 0; i < 4; i++) { if (!d[i]) { held[i] = 0; continue; } int h = ++held[i]; if (h == 1 || (h > 20 && h % 4 == 0)) menu_input(i); }
                if (in.pad_b) sho_stereo_menu(0);
                in.nav_up = in.nav_down = in.nav_left = in.nav_right = 0; in.pad_b = 0;
            }
            {   int start_edge = in.start && !start_prev; start_prev = in.start;
                if (!paused && (start_edge || want_pause3d) && sho_md_pause_allowed()) {
                    paused = 1; pause_menu = sho_pause_begin(); ptick = 0;
                    if (want_pause3d && pause_menu) sho_pause_goto_3d();
                } else if (paused && pause_menu) {
                    int act = sho_pause_update(&in, ptick++);
                    if (act == 3 || act == 4) { set_shape(g_shape + (act == 3 ? 1 : -1)); act = 0; }
                    if (act == 5) { save_stereo(); act = 0; }
                    if (act == 6) { save_vr(); act = 0; }
                    if (close_pause) act = 1;
                    if (act) { sho_pause_end(); int m, s; sho_get_sound_levels(&m, &s); ini_set_int("audio", "music_level", m); ini_set_int("audio", "sfx_level", s);
                               paused = 0; pause_menu = 0; if (act == 2) reset_to_title(); }
                } else if (paused && !pause_menu) paused = 0;
                want_pause3d = 0; close_pause = 0;
                if (paused) in.start = 0;
            }
            if (!paused) {
                sho_set_input(&in); sho_run_frame();
                if (sho_md_exit_requested()) g_running = 0;
                int rq = sho_md_reset_requested();
                if (rq == 1) reset_to_title();
                else if (rq == 2) { sho_md_before_reset(); save_nvram(); sho_reset(); load_nvram(); apply_game_settings(); }
            }
            if (have_audio) {
                long q = audio_queued();
                if (!paused && game_frames > 120 && sho_audio_target_step(q, &audio_target)) { underruns++; sho_diag("sound: queue ran dry (%ld samples); cushion now %d ms", q, audio_target * 1000 / AUDIO_RATE); }
                double corr = (double)(audio_target - q) / audio_target * 0.01;
                if (corr > 0.01) corr = 0.01; if (corr < -0.01) corr = -0.01;
                audio_frac += AUDIO_RATE * period * (1.0 + corr);
                int n = (int)audio_frac; audio_frac -= n; if (n > 1100) n = 1100;
                if (paused) memset(samples, 0, sizeof samples); else sho_audio(samples, n);
                if (q < audio_target * 4) audio_submit(samples, n);
            }
            if (++frames_since_save >= 60 * 60 || g_save_now) { save_nvram(); frames_since_save = 0; g_save_now = 0; }
            game_frames++;
        }
        double tg1 = now_s(); tm_game += tg1 - tg0; tm_frames += frames; tm_loops++;
        {   /* the newest picture to the headset */
            sho_stereo st; sho_get_stereo(&st);
            int fw; const uint32_t *fb = sho_present_frame(&fw);
            publish(fb, fw, sho_width(), sho_shapes[g_shape].aspect, st.mode != SHO_3D_OFF && !st.swap ? st.depth : 0);
        }
        tm_pic += now_s() - tg1;
        if (t - diag_t >= 30.0) {
            double fps = (game_frames - diag_frames) / (t - diag_t);
            sho_diag("%4.0f s: speed %.1f%% (%.2f game frames a second), game %.1f ms a frame, 3D picture %.1f ms (a pass has %.1f), sound cushion %d ms, ran dry %d times",
                     t - diag_t0, fps / SHO_FPS * 100.0, fps, tm_frames ? tm_game * 1000.0 / tm_frames : 0.0, tm_loops ? tm_pic * 1000.0 / tm_loops : 0.0, 1000.0 / SHO_FPS,
                     audio_target * 1000 / AUDIO_RATE, underruns);
            tm_game = tm_pic = 0; tm_frames = tm_loops = 0; diag_t = t; diag_frames = game_frames;
        }
    }
    save_nvram();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* the headset (main thread): EGL, OpenXR, the controllers             */
/* ------------------------------------------------------------------ */
static EGLDisplay egl_dpy; static EGLConfig egl_cfg; static EGLContext egl_ctx; static EGLSurface egl_surf;
static XrInstance inst; static XrSystemId sysid; static XrSession sess; static XrSpace local_space, view_space;
static XrSwapchain chain[2]; static GLuint chain_tex[2][16], chain_fbo[2][16]; static uint32_t chain_n[2];   /* one picture per eye */
static int tex_sx = TEX_SCALE, tex_sy = TEX_SCALE, tex_w, tex_h;
static XrSwapchain lchain; static GLuint lchain_fbo[16], ltex; static uint32_t lchain_n;   /* the launcher's picture (1920 x 1080) */
static GLuint src_tex, prog; static GLint u_tex, u_uvs;
static PFN_xrSetAndroidApplicationThreadKHR xr_set_thread;      /* (optional: tells the headset which threads matter) */
static XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
static int frame_begun, should_render, have_screen;
static XrTime disp_time; static XrDuration disp_period;
static XrPosef screen_pose = { { 0, 0, 0, 1 }, { 0, 0, -2 } };
static float anchor_x, anchor_y, anchor_z, anchor_fx, anchor_fz = -1;
static XrActionSet aset;
static XrAction a_grip, a_trigger, a_squeeze, a_a, a_b, a_x, a_y, a_menu, a_stick, a_recenter, a_rstick;
static XrPath hand_path[2]; static XrSpace grip_space[2];
static int menu_prev, recenter_prev, y_prev, twist_on; static XrQuaternionf twist_ref;

/* tells the headset what the calling thread is for (0 the game, 1 a helper), so it runs on the fast cores */
static void thread_kind(int kind) {
    if (xr_set_thread && sess) xr_set_thread(sess, kind ? XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR : XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, (uint32_t)gettid());
}
static int egl_init(void) {
    egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, NULL, NULL)) return 0;
    const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(egl_dpy, ca, &egl_cfg, 1, &n) || n < 1) return 0;
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, xa);
    if (egl_ctx == EGL_NO_CONTEXT) return 0;
    const EGLint sa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    egl_surf = eglCreatePbufferSurface(egl_dpy, egl_cfg, sa);
    if (egl_surf == EGL_NO_SURFACE) return 0;
    return eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx) == EGL_TRUE;
}
static GLuint make_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOGE("shader: %s", log); }
    return s;
}
static int gl_init(void) {
    /* one triangle over the whole target; the game's picture sampled pixel for pixel (its bytes are B, G, R, x) */
    /* (uvs = where in the source texture this eye's picture starts, and its width; the picture's first line is its top,
       the target's first line its bottom) */
    static const char *vs = "#version 300 es\nuniform vec2 uvs; out vec2 uv;\nvoid main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n uv = vec2(uvs.x + p.x * uvs.y, 1.0 - p.y); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char *fs = "#version 300 es\nprecision mediump float;\nuniform sampler2D tex; in vec2 uv; out vec4 o;\nvoid main() { o = vec4(texture(tex, uv).bgr, 1.0); }\n";
    GLuint v = make_shader(GL_VERTEX_SHADER, vs), f = make_shader(GL_FRAGMENT_SHADER, fs);
    prog = glCreateProgram(); glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok); if (!ok) return 0;
    u_tex = glGetUniformLocation(prog, "tex"); u_uvs = glGetUniformLocation(prog, "uvs");
    return 1;
}
/* the texture the game's picture is put in: sRGB like the headset's image, so colours pass through unchanged */
static void make_src_tex(int srgb) {
    glGenTextures(1, &src_tex); glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, PIC_MAX_W, SHO_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static XrPath xpath(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(inst, s, &p); return p; }
static XrAction make_action(const char *name, const char *nice, XrActionType type, int hands) {
    XrActionCreateInfo ci; memset(&ci, 0, sizeof ci); ci.type = XR_TYPE_ACTION_CREATE_INFO; ci.actionType = type;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name); snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", nice);
    if (hands) { ci.countSubactionPaths = 2; ci.subactionPaths = hand_path; }
    XrAction a = XR_NULL_HANDLE; xrCreateAction(aset, &ci, &a); return a;
}
static int make_actions(void) {
    hand_path[0] = xpath("/user/hand/left"); hand_path[1] = xpath("/user/hand/right");
    XrActionSetCreateInfo si; memset(&si, 0, sizeof si); si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    snprintf(si.actionSetName, sizeof si.actionSetName, "ride"); snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Riding");
    if (XR_FAILED(xrCreateActionSet(inst, &si, &aset))) return 0;
    a_grip = make_action("handlebar", "Handlebar grip", XR_ACTION_TYPE_POSE_INPUT, 1);
    a_trigger = make_action("pedals", "Throttle / brake", XR_ACTION_TYPE_FLOAT_INPUT, 1);
    a_squeeze = make_action("turbo", "Turbo / twist grip", XR_ACTION_TYPE_FLOAT_INPUT, 1);
    a_a = make_action("select", "Select / accelerate", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    a_b = make_action("back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    a_x = make_action("coin", "Insert coin", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    a_y = make_action("settings", "Settings menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    a_menu = make_action("pause", "Start / pause", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    a_stick = make_action("menu_move", "Menu directions / steering", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    a_recenter = make_action("recenter", "Recentre the screen", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    a_rstick = make_action("screen_move", "Move the screen up / down", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    XrActionSuggestedBinding touch[] = {
        { a_grip, xpath("/user/hand/left/input/grip/pose") }, { a_grip, xpath("/user/hand/right/input/grip/pose") },
        { a_trigger, xpath("/user/hand/left/input/trigger/value") }, { a_trigger, xpath("/user/hand/right/input/trigger/value") },
        { a_squeeze, xpath("/user/hand/left/input/squeeze/value") }, { a_squeeze, xpath("/user/hand/right/input/squeeze/value") },
        { a_a, xpath("/user/hand/right/input/a/click") }, { a_b, xpath("/user/hand/right/input/b/click") },
        { a_x, xpath("/user/hand/left/input/x/click") }, { a_y, xpath("/user/hand/left/input/y/click") },
        { a_menu, xpath("/user/hand/left/input/menu/click") },
        { a_stick, xpath("/user/hand/left/input/thumbstick") }, { a_recenter, xpath("/user/hand/right/input/thumbstick/click") },
        { a_rstick, xpath("/user/hand/right/input/thumbstick") },
    };
    XrInteractionProfileSuggestedBinding sb; memset(&sb, 0, sizeof sb); sb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    sb.interactionProfile = xpath("/interaction_profiles/oculus/touch_controller"); sb.suggestedBindings = touch; sb.countSuggestedBindings = sizeof touch / sizeof touch[0];
    XrResult r = xrSuggestInteractionProfileBindings(inst, &sb);
    if (XR_FAILED(r)) sho_diag("VR: controller bindings not taken (%d)", (int)r);
    XrSessionActionSetsAttachInfo at; memset(&at, 0, sizeof at); at.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO; at.countActionSets = 1; at.actionSets = &aset;
    if (XR_FAILED(xrAttachSessionActionSets(sess, &at))) return 0;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sp; memset(&sp, 0, sizeof sp); sp.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        sp.action = a_grip; sp.subactionPath = hand_path[h]; sp.poseInActionSpace.orientation.w = 1;
        xrCreateActionSpace(sess, &sp, &grip_space[h]);
    }
    return 1;
}

static void xr_fail(const char *what, XrResult r) {
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (inst) xrResultToString(inst, r, name);
    qlog("xr: %s failed: %s (%d)", what, name[0] ? name : "error", (int)r);
}
static const char *xr_init(struct android_app *app) {
    XrResult r;
    {   /* the loader needs the app's Java side before anything else */
        PFN_xrInitializeLoaderKHR init = NULL;
        if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init)) || !init) return "OPENXR LOADER HAS NO INITIALIZE";
        XrLoaderInitInfoAndroidKHR li; memset(&li, 0, sizeof li); li.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
        li.applicationVM = app->activity->vm; li.applicationContext = app->activity->clazz;
        if (XR_FAILED(r = init((const XrLoaderInitInfoBaseHeaderKHR *)&li))) { xr_fail("xrInitializeLoaderKHR", r); return "THE OPENXR LOADER COULD NOT START"; }
        qlog("xr: loader started");
    }
    const char *exts[4] = { "XR_KHR_android_create_instance", "XR_KHR_opengl_es_enable" }; uint32_t next = 2; int has_perf = 0, has_thr = 0;
    {   /* optional: ask for full processor speed, and say which threads are the important ones */
        /* (every OpenXR list is asked for twice: how long it is, then the list - a list that does not fit comes back empty) */
        uint32_t np = 0; XrExtensionProperties *props = NULL;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &np, NULL)) && np && (props = (XrExtensionProperties *)calloc(np, sizeof *props)) != NULL) {
            for (uint32_t i = 0; i < np; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        }
        if (props && XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, np, &np, props)))
            for (uint32_t i = 0; i < np; i++) {
                if (!strcmp(props[i].extensionName, "XR_EXT_performance_settings")) has_perf = 1;
                if (!strcmp(props[i].extensionName, "XR_KHR_android_thread_settings")) has_thr = 1;
            }
        free(props);
        if (has_perf) exts[next++] = "XR_EXT_performance_settings";
        if (has_thr) exts[next++] = "XR_KHR_android_thread_settings";
    }
    XrInstanceCreateInfoAndroidKHR ai; memset(&ai, 0, sizeof ai); ai.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    ai.applicationVM = app->activity->vm; ai.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ici; memset(&ici, 0, sizeof ici); ici.type = XR_TYPE_INSTANCE_CREATE_INFO; ici.next = &ai;
    snprintf(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "Super Hang-On Ultimate");
    snprintf(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "sho");
    ici.applicationInfo.applicationVersion = 1; ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = next; ici.enabledExtensionNames = exts;
    if (XR_FAILED(r = xrCreateInstance(&ici, &inst))) { inst = XR_NULL_HANDLE; xr_fail("xrCreateInstance", r); return "OPENXR COULD NOT START"; }
    qlog("xr: instance made (performance settings %s, thread settings %s)", has_perf ? "yes" : "no", has_thr ? "yes" : "no");
    XrSystemGetInfo sgi; memset(&sgi, 0, sizeof sgi); sgi.type = XR_TYPE_SYSTEM_GET_INFO; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    for (int tries = 0; XR_FAILED(r = xrGetSystem(inst, &sgi, &sysid)); tries++) {   /* (the headset can take a moment to be ready) */
        if (tries >= 20) { xr_fail("xrGetSystem", r); return "NO HEADSET FOUND"; }
        struct timespec ts = { 0, 100 * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
    {   PFN_xrGetOpenGLESGraphicsRequirementsKHR req = NULL;
        xrGetInstanceProcAddr(inst, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req);
        XrGraphicsRequirementsOpenGLESKHR gr; memset(&gr, 0, sizeof gr); gr.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
        if (!req || XR_FAILED(r = req(inst, sysid, &gr))) { xr_fail("xrGetOpenGLESGraphicsRequirementsKHR", r); return "OPENGL ES NOT ACCEPTED"; } }
    XrGraphicsBindingOpenGLESAndroidKHR gb; memset(&gb, 0, sizeof gb); gb.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    gb.display = egl_dpy; gb.config = egl_cfg; gb.context = egl_ctx;
    XrSessionCreateInfo sci; memset(&sci, 0, sizeof sci); sci.type = XR_TYPE_SESSION_CREATE_INFO; sci.next = &gb; sci.systemId = sysid;
    if (XR_FAILED(r = xrCreateSession(inst, &sci, &sess))) { sess = XR_NULL_HANDLE; xr_fail("xrCreateSession", r); return "THE HEADSET SESSION COULD NOT START"; }
    qlog("xr: session made");
    if (has_perf) {   /* the machine being emulated needs the processor far more than the picture needs the graphics chip */
        PFN_xrPerfSettingsSetPerformanceLevelEXT perf = NULL;
        xrGetInstanceProcAddr(inst, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&perf);
        if (perf) { perf(sess, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT); perf(sess, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT); }
    }
    if (has_thr) {
        xrGetInstanceProcAddr(inst, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&xr_set_thread);
        if (xr_set_thread) xr_set_thread(sess, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid());
    }
    {   uint32_t n = 0; XrEnvironmentBlendMode modes[32];
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(inst, sysid, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, NULL)) && n && n <= 32 &&
            XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(inst, sysid, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes)) && n) blend = modes[0]; }
    XrReferenceSpaceCreateInfo rci; memset(&rci, 0, sizeof rci); rci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO; rci.poseInReferenceSpace.orientation.w = 1;
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(r = xrCreateReferenceSpace(sess, &rci, &local_space))) { xr_fail("xrCreateReferenceSpace", r); return "NO PLAY SPACE"; }
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; xrCreateReferenceSpace(sess, &rci, &view_space);

    int64_t fmts[512]; uint32_t nf = 0; int64_t fmt = 0;
    if (XR_FAILED(r = xrEnumerateSwapchainFormats(sess, 0, &nf, NULL)) || nf > 512 || XR_FAILED(r = xrEnumerateSwapchainFormats(sess, nf, &nf, fmts))) {
        xr_fail("xrEnumerateSwapchainFormats", r); nf = 0; }
    static const int64_t want[2] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
    for (int w = 0; w < 2 && !fmt; w++) for (uint32_t i = 0; i < nf; i++) if (fmts[i] == want[w]) { fmt = want[w]; break; }
    if (!fmt) {
        char list[400] = ""; for (uint32_t i = 0; i < nf && i < 40; i++) { size_t l = strlen(list); snprintf(list + l, sizeof list - l, "%s0x%llx", i ? " " : "", (unsigned long long)fmts[i]); }
        qlog("xr: none of the %u picture formats is 8-bit RGBA: %s", (unsigned)nf, list); return "NO PICTURE FORMAT"; }
    qlog("xr: picture format 0x%llx (of %u)", (unsigned long long)fmt, (unsigned)nf);
    make_src_tex(fmt == GL_SRGB8_ALPHA8);
    {   /* one picture per eye, as big as the headset allows (Quest headsets take at most 4096 pixels a side) */
        XrSystemProperties sp; memset(&sp, 0, sizeof sp); sp.type = XR_TYPE_SYSTEM_PROPERTIES;
        int mw = 4096, mh = 4096;
        if (XR_SUCCEEDED(xrGetSystemProperties(inst, sysid, &sp))) {
            if (sp.graphicsProperties.maxSwapchainImageWidth > 0) mw = (int)sp.graphicsProperties.maxSwapchainImageWidth;
            if (sp.graphicsProperties.maxSwapchainImageHeight > 0) mh = (int)sp.graphicsProperties.maxSwapchainImageHeight;
            qlog("xr: headset \"%s\", pictures up to %d x %d, %u layers", sp.systemName, mw, mh, (unsigned)sp.graphicsProperties.maxLayerCount);
        }
        while (tex_sx > 1 && SHO_MAX_W * tex_sx > mw) tex_sx--;
        while (tex_sy > 1 && SHO_H * tex_sy > mh) tex_sy--;
        tex_w = SHO_MAX_W * tex_sx; tex_h = SHO_H * tex_sy;
    }
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sc; memset(&sc, 0, sizeof sc); sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = fmt; sc.sampleCount = 1; sc.width = (uint32_t)tex_w; sc.height = (uint32_t)tex_h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        if (XR_FAILED(r = xrCreateSwapchain(sess, &sc, &chain[e]))) { xr_fail("xrCreateSwapchain", r); return "THE HEADSET PICTURE COULD NOT BE MADE"; }
        XrSwapchainImageOpenGLESKHR imgs[16]; memset(imgs, 0, sizeof imgs);
        for (int i = 0; i < 16; i++) imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        if (XR_FAILED(r = xrEnumerateSwapchainImages(chain[e], 0, &chain_n[e], NULL)) || chain_n[e] > 16 ||
            XR_FAILED(r = xrEnumerateSwapchainImages(chain[e], chain_n[e], &chain_n[e], (XrSwapchainImageBaseHeader *)imgs))) { xr_fail("xrEnumerateSwapchainImages", r); return "THE HEADSET PICTURE COULD NOT BE HAD"; }
        glGenFramebuffers((GLsizei)chain_n[e], chain_fbo[e]);
        for (uint32_t i = 0; i < chain_n[e]; i++) {
            chain_tex[e][i] = imgs[i].image;
            glBindFramebuffer(GL_FRAMEBUFFER, chain_fbo[e][i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, chain_tex[e][i], 0);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) qlog("gl: picture %d/%u is not drawable (0x%x)", e, (unsigned)i, (unsigned)st);
        }
    }
    {   /* the launcher's */
        XrSwapchainCreateInfo sc; memset(&sc, 0, sizeof sc); sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = fmt; sc.sampleCount = 1; sc.width = LC_IW; sc.height = LC_IH; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        XrSwapchainImageOpenGLESKHR imgs[16]; memset(imgs, 0, sizeof imgs);
        for (int i = 0; i < 16; i++) imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        if (XR_FAILED(r = xrCreateSwapchain(sess, &sc, &lchain)) || XR_FAILED(r = xrEnumerateSwapchainImages(lchain, 0, &lchain_n, NULL)) || lchain_n > 16 ||
            XR_FAILED(r = xrEnumerateSwapchainImages(lchain, lchain_n, &lchain_n, (XrSwapchainImageBaseHeader *)imgs))) { xr_fail("launcher picture", r); lchain = XR_NULL_HANDLE; }
        else {
            glGenFramebuffers((GLsizei)lchain_n, lchain_fbo);
            for (uint32_t i = 0; i < lchain_n; i++) { glBindFramebuffer(GL_FRAMEBUFFER, lchain_fbo[i]); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imgs[i].image, 0); }
            glGenTextures(1, &ltex); glBindTexture(GL_TEXTURE_2D, ltex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, fmt == GL_SRGB8_ALPHA8 ? GL_SRGB8_ALPHA8 : GL_RGBA8, LC_IW, LC_IH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!make_actions()) return "THE CONTROLLERS COULD NOT BE SET UP";
    qlog("xr: ready (format 0x%x, 2 pictures of %d x %d, %u + %u images)", (unsigned)fmt, tex_w, tex_h, (unsigned)chain_n[0], (unsigned)chain_n[1]);
    return NULL;
}

static void poll_xr_events(void) {
    XrEventDataBuffer ev;
    for (;;) {
        memset(&ev, 0, sizeof ev); ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(inst, &ev) != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *s = (const XrEventDataSessionStateChanged *)&ev;
            qlog("xr: session state %d", (int)s->state);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi; memset(&bi, 0, sizeof bi); bi.type = XR_TYPE_SESSION_BEGIN_INFO; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(sess, &bi);
                if (XR_SUCCEEDED(r)) { g_session_running = 1; pthread_mutex_lock(&g_lock); g_want_recenter = 1; pthread_mutex_unlock(&g_lock); qlog("xr: session begun"); }
                else xr_fail("xrBeginSession", r);
            } else if (s->state == XR_SESSION_STATE_STOPPING) { xrEndSession(sess); g_session_running = 0; g_save_now = 1; }
            else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) { g_session_running = 0; g_running = 0; }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { g_session_running = 0; g_running = 0; }
    }
}

static float get_float(XrAction a, int hand) {
    XrActionStateGetInfo gi; memset(&gi, 0, sizeof gi); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.action = a; gi.subactionPath = hand_path[hand];
    XrActionStateFloat st; memset(&st, 0, sizeof st); st.type = XR_TYPE_ACTION_STATE_FLOAT;
    return XR_SUCCEEDED(xrGetActionStateFloat(sess, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static int get_bool(XrAction a) {
    XrActionStateGetInfo gi; memset(&gi, 0, sizeof gi); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.action = a;
    XrActionStateBoolean st; memset(&st, 0, sizeof st); st.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    return XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &st)) && st.isActive && st.currentState;
}
static void get_stick(XrAction a, float *x, float *y) {
    XrActionStateGetInfo gi; memset(&gi, 0, sizeof gi); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.action = a;
    XrActionStateVector2f st; memset(&st, 0, sizeof st); st.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    *x = *y = 0;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(sess, &gi, &st)) && st.isActive) { *x = st.currentState.x; *y = st.currentState.y; }
}
/* the controllers as handlebars (see win32_vr.c for the reasoning); no lock held while the runtime is asked */
static void read_controllers(void) {
    int steer_stick, swap_trig, twist, menu; float tilt_angle, twist_angle, afx, afz;
    pthread_mutex_lock(&g_lock);
    steer_stick = g_steer_stick; swap_trig = g_swap_trig; twist = g_twist; menu = g_menu_flag; tilt_angle = (float)g_tilt_angle; twist_angle = (float)g_twist_angle; afx = anchor_fx; afz = anchor_fz;
    pthread_mutex_unlock(&g_lock);
    sho_input in; memset(&in, 0, sizeof in);
    XrSpaceLocation l[2]; int ok = 1;
    for (int h = 0; h < 2; h++) {
        memset(&l[h], 0, sizeof l[h]); l[h].type = XR_TYPE_SPACE_LOCATION;
        if (!grip_space[h] || XR_FAILED(xrLocateSpace(grip_space[h], local_space, disp_time, &l[h])) || !(l[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) ok = 0;
    }
    if (ok && !steer_stick) {
        float dx = l[1].pose.position.x - l[0].pose.position.x, dy = l[1].pose.position.y - l[0].pose.position.y, dz = l[1].pose.position.z - l[0].pose.position.z;
        float flat = sqrtf(dx * dx + dz * dz);
        if (flat > 0.08f) {
            float deg = atan2f(-dy, flat) * 57.29578f, dead = 1.5f, s = deg > dead ? (deg - dead) : deg < -dead ? (deg + dead) : 0.0f;
            s /= (tilt_angle - dead); if (s > 1) s = 1; if (s < -1) s = -1;
            in.steer = s;
        }
    }
    float thr = get_float(a_trigger, swap_trig ? 0 : 1), brk = get_float(a_trigger, swap_trig ? 1 : 0), sel = thr;
    float sq_l = get_float(a_squeeze, 0), sq_r = get_float(a_squeeze, 1);
    if (twist) {
        float tl = get_float(a_trigger, 0), tr = get_float(a_trigger, 1);
        brk = tl > tr ? tl : tr; sel = tr; thr = 0;
        int held = sq_r > 0.5f && (l[1].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && !menu;
        if (!held) twist_on = 0;
        else {
            XrQuaternionf q = l[1].pose.orientation;
            if (!twist_on) { twist_ref = q; twist_on = 1; }
            XrQuaternionf r = twist_ref, d;
            d.w = q.w * r.w + q.x * r.x + q.y * r.y + q.z * r.z;
            d.x = -q.w * r.x + q.x * r.w - q.y * r.z + q.z * r.y;
            d.y = -q.w * r.y + q.x * r.z + q.y * r.w - q.z * r.x;
            d.z = -q.w * r.z - q.x * r.y + q.y * r.x + q.z * r.w;
            float ax = -afz, az = afx;
            if (ok) { float bx = l[1].pose.position.x - l[0].pose.position.x, bz = l[1].pose.position.z - l[0].pose.position.z, n = sqrtf(bx * bx + bz * bz);
                      if (n > 0.08f) { ax = bx / n; az = bz / n; } }
            float deg = 2.0f * atan2f(d.x * ax + d.z * az, d.w) * 57.29578f;
            if (deg > 180) deg -= 360; if (deg < -180) deg += 360;
            float dead = 3.0f; thr = deg > dead ? (deg - dead) / (twist_angle - dead) : 0.0f; if (thr > 1) thr = 1;
        }
    }
    in.throttle = thr; in.brake = brk;
    if (sq_l > 0.6f || (!twist && sq_r > 0.6f)) in.turbo = 1;
    in.pad_a = get_bool(a_a); in.pad_b = get_bool(a_b); in.coin = get_bool(a_x);
    int yb = get_bool(a_y), y_e = yb && !y_prev; y_prev = yb;
    int m = get_bool(a_menu); if (m) in.start = 1; menu_prev = m;
    float sx, sy; get_stick(a_stick, &sx, &sy);
    if (sy > 0.6f) in.nav_up = 1; if (sy < -0.6f) in.nav_down = 1; if (sx < -0.6f) in.nav_left = 1; if (sx > 0.6f) in.nav_right = 1;
    if (steer_stick && !menu) { float dead = 0.12f; in.steer = sx > dead ? (sx - dead) / (1 - dead) : sx < -dead ? (sx + dead) / (1 - dead) : 0.0f; }
    float rx, ry; get_stick(a_rstick, &rx, &ry);
    float dh = fabsf(ry) > 0.5f && fabsf(rx) < 0.5f ? (ry > 0 ? 30.0f : -30.0f) * (disp_period > 0 ? (float)disp_period * 1e-9f : 0.011f) : 0.0f;   /* cm */
    if (menu && sel > 0.6f) in.pad_a = 1;
    int rc = get_bool(a_recenter), rc_e = rc && !recenter_prev; recenter_prev = rc;
    static float h_frac;
    pthread_mutex_lock(&g_lock);
    g_pad = in;
    if (y_e) g_y_edge = 1;
    if (rc_e) g_want_recenter = 1;
    if (dh != 0 && have_screen) {      /* the right stick moves the screen up / down */
        h_frac += dh; int whole = (int)h_frac; h_frac -= (float)whole;
        g_scr_height += whole; if (g_scr_height < -300) g_scr_height = -300; if (g_scr_height > 300) g_scr_height = 300;
        g_cfg_moved = 1; g_cfg_idle = 0;
    } else if (g_cfg_idle < 1000) g_cfg_idle++;
    pthread_mutex_unlock(&g_lock);
}

static void xr_frame(void) {
    static uint32_t tpic[PIC_MAX_W * SHO_H]; static int fw, eye_w, depth, seq = -1, tex_seq = -2; static double aspect;
    XrFrameState fs; memset(&fs, 0, sizeof fs); fs.type = XR_TYPE_FRAME_STATE;
    if (XR_FAILED(xrWaitFrame(sess, NULL, &fs)) || XR_FAILED(xrBeginFrame(sess, NULL))) return;
    should_render = fs.shouldRender != 0; disp_time = fs.predictedDisplayTime; disp_period = fs.predictedDisplayPeriod;
    XrActiveActionSet as; memset(&as, 0, sizeof as); as.actionSet = aset;
    XrActionsSyncInfo si; memset(&si, 0, sizeof si); si.type = XR_TYPE_ACTIONS_SYNC_INFO; si.countActiveActionSets = 1; si.activeActionSets = &as;
    xrSyncActions(sess, &si);
    read_controllers();

    /* the screen: in front of where the head points when asked (level, upright), then where the settings put it */
    pthread_mutex_lock(&g_lock); int rc = g_want_recenter; pthread_mutex_unlock(&g_lock);
    if (rc && view_space) {
        XrSpaceLocation loc; memset(&loc, 0, sizeof loc); loc.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(xrLocateSpace(view_space, local_space, disp_time, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            XrQuaternionf q = loc.pose.orientation;
            float fx = -2.0f * (q.x * q.z + q.w * q.y), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y)), len = sqrtf(fx * fx + fz * fz);
            if (len < 0.01f) { fx = 0; fz = -1; len = 1; }
            pthread_mutex_lock(&g_lock);
            anchor_x = loc.pose.position.x; anchor_y = loc.pose.position.y; anchor_z = loc.pose.position.z; anchor_fx = fx / len; anchor_fz = fz / len;
            g_want_recenter = 0; have_screen = 1;
            pthread_mutex_unlock(&g_lock);
        }
    }
    float scr_w, scr_d, scr_h, afx, afz;
    pthread_mutex_lock(&g_lock);
    scr_w = g_scr_width / 100.0f; scr_d = g_scr_dist / 100.0f; scr_h = g_scr_height / 100.0f; afx = anchor_fx; afz = anchor_fz;
    if (scr_w < 0.5f) scr_w = 0.5f; if (scr_d < 0.5f) scr_d = 0.5f;
    {   float yaw = atan2f(-afx, -afz);
        screen_pose.orientation.x = 0; screen_pose.orientation.y = sinf(yaw * 0.5f); screen_pose.orientation.z = 0; screen_pose.orientation.w = cosf(yaw * 0.5f);
        screen_pose.position.x = anchor_x + afx * scr_d; screen_pose.position.y = anchor_y + scr_h; screen_pose.position.z = anchor_z + afz * scr_d; }
    if (g_pic_seq != seq && g_pic_fw > 0) { memcpy(tpic, g_pic, (size_t)g_pic_fw * SHO_H * 4); fw = g_pic_fw; eye_w = g_pic_eye_w; depth = g_pic_depth; aspect = g_pic_aspect; seq = g_pic_seq; }
    pthread_mutex_unlock(&g_lock);

    XrFrameEndInfo fe; memset(&fe, 0, sizeof fe); fe.type = XR_TYPE_FRAME_END_INFO; fe.displayTime = disp_time; fe.environmentBlendMode = blend;
    XrCompositionLayerQuad quad[2]; const XrCompositionLayerBaseHeader *layers[2]; int nl = 0;
    int launcher_up; pthread_mutex_lock(&g_lock); launcher_up = g_launcher_on && lchain; pthread_mutex_unlock(&g_lock);
    if (launcher_up && should_render && have_screen) {   /* the launcher: one flat picture, 16:9 */
        static int lseq = -1;
        uint32_t idx = 0; XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wi.timeout = XR_INFINITE_DURATION;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(lchain, NULL, &idx))) {
            if (XR_SUCCEEDED(xrWaitSwapchainImage(lchain, &wi)) && idx < lchain_n) {
                glBindTexture(GL_TEXTURE_2D, ltex);
                pthread_mutex_lock(&g_lock);
                if (lseq != g_lpic_seq && g_lpic) { glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LC_IW, LC_IH, GL_RGBA, GL_UNSIGNED_BYTE, g_lpic); lseq = g_lpic_seq; }
                pthread_mutex_unlock(&g_lock);
                glBindFramebuffer(GL_FRAMEBUFFER, lchain_fbo[idx]);
                glViewport(0, 0, LC_IW, LC_IH); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
                glUseProgram(prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ltex); glUniform1i(u_tex, 0);
                glUniform2f(u_uvs, 0.0f, 1.0f);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            xrReleaseSwapchainImage(lchain, NULL);
            XrCompositionLayerQuad *q = &quad[0]; memset(q, 0, sizeof *q); q->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            q->space = local_space; q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q->subImage.swapchain = lchain; q->subImage.imageRect.extent.width = LC_IW; q->subImage.imageRect.extent.height = LC_IH;
            q->pose = screen_pose; q->size.width = scr_w; q->size.height = scr_w * 9.0f / 16.0f;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)q;
        }
    } else if (should_render && have_screen && seq >= 0 && eye_w > 0 && eye_w <= SHO_MAX_W) {
        int stereo = fw >= 2 * eye_w, tw = eye_w * tex_sx, ne = stereo ? 2 : 1;
        float w = scr_w, h = aspect > 0 ? (float)(scr_w / aspect) : scr_w * 9.0f / 16.0f;
        glBindTexture(GL_TEXTURE_2D, src_tex);
        if (tex_seq != seq) { glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, SHO_H, GL_RGBA, GL_UNSIGNED_BYTE, tpic); tex_seq = seq; }
        for (int e = 0; e < ne; e++) {
            uint32_t idx = 0; XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wi.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(xrAcquireSwapchainImage(chain[e], NULL, &idx))) continue;
            if (XR_FAILED(xrWaitSwapchainImage(chain[e], &wi)) || idx >= chain_n[e]) { xrReleaseSwapchainImage(chain[e], NULL); continue; }
            /* this eye's half of the picture (or the whole of it) stretched over tw x tex_h of its headset picture */
            int ew = stereo ? fw / 2 : fw;
            glBindFramebuffer(GL_FRAMEBUFFER, chain_fbo[e][idx]);
            glViewport(0, 0, tw, tex_h); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
            glUseProgram(prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src_tex); glUniform1i(u_tex, 0);
            glUniform2f(u_uvs, (float)(e * ew) / (float)PIC_MAX_W, (float)ew / (float)PIC_MAX_W);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            xrReleaseSwapchainImage(chain[e], NULL);
            {
                XrCompositionLayerQuad *q = &quad[nl]; memset(q, 0, sizeof *q); q->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                q->space = local_space; q->eyeVisibility = stereo ? (e ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT) : XR_EYE_VISIBILITY_BOTH;
                q->subImage.swapchain = chain[e]; q->subImage.imageArrayIndex = 0;
                q->subImage.imageRect.offset.x = 0; q->subImage.imageRect.offset.y = 0;
                q->subImage.imageRect.extent.width = tw; q->subImage.imageRect.extent.height = tex_h;
                q->pose = screen_pose; q->size.width = w; q->size.height = h;
                if (stereo) {      /* the backdrop never further apart than the eyes: see win32_vr.c */
                    float excess = (float)depth * w / (float)eye_w - 0.063f;
                    if (excess > 0) { float sh = (e ? -0.5f : 0.5f) * excess; q->pose.position.x += -afz * sh; q->pose.position.z += afx * sh; }
                }
                layers[nl++] = (const XrCompositionLayerBaseHeader *)q;
            }
        }
    }
    fe.layerCount = (uint32_t)nl; fe.layers = nl ? layers : NULL;
    XrResult er = xrEndFrame(sess, &fe);
    {   static int logged_first, logged_err;
        if (nl && !logged_first) { logged_first = 1; qlog("xr: first picture shown (%d layer%s)", nl, nl > 1 ? "s" : ""); }
        if (XR_FAILED(er) && logged_err < 5) { logged_err++; xr_fail("xrEndFrame", er); }
    }
}

/* ------------------------------------------------------------------ */
static int g_resumed, g_destroy;
static void on_app_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    if (cmd == APP_CMD_RESUME) g_resumed = 1;
    else if (cmd == APP_CMD_PAUSE) { g_resumed = 0; g_save_now = 1; }
    else if (cmd == APP_CMD_DESTROY) g_destroy = 1;
}

void android_main(struct android_app *app) {
    JNIEnv *env = NULL; (*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL);
    app->onAppCmd = on_app_cmd;
    snprintf(g_dir, sizeof g_dir, "%s", app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath);
    mkdir(g_dir, 0770);
    g_assets = app->activity->assetManager;
    {   char p[PATHN]; path_join(p, g_dir, "shangon.log"); sho_diag_open(p);
        sho_diag_mirror("/sdcard/Download/SuperHangOn-log.txt"); }
    qlog("Super Hang-On Ultimate (Quest) - files in %s", g_dir);

    const char *fail = NULL;
    if (!egl_init()) fail = "EGL COULD NOT START";
    else if (!gl_init()) fail = "OPENGL ES COULD NOT START";
    else qlog("gl: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (!fail) fail = xr_init(app);
    if (fail) {   /* nothing can be shown: say why in the log and close, rather than leave the headset waiting */
        qlog("START FAILED: %s", fail);
        if (sess) { xrDestroySession(sess); sess = XR_NULL_HANDLE; }
        ANativeActivity_finish(app->activity);
    }
    double fail_t = now_s();

    pthread_t gt; int game_on = 0, finishing = 0;
    if (!fail) {   /* the game: a big stack (the Mega Drive capture and the recompiled code like room) */
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        game_on = pthread_create(&gt, &a, game_thread, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    while (!g_destroy && !app->destroyRequested) {
        int events; struct android_poll_source *source;
        int timeout = (!fail && g_session_running) ? 0 : 100;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = 0;
        }
        if (fail) { if (now_s() - fail_t > 3.0) { qlog("closing"); exit(0); } continue; }   /* (if the system has not closed it by then) */
        poll_xr_events();
        if (!g_running) { if (!finishing) { ANativeActivity_finish(app->activity); finishing = 1; } continue; }   /* EXIT GAME, or the headset ended the session */
        if (g_session_running) xr_frame();
    }
    g_running = 0;
    if (game_on) pthread_join(gt, NULL);
    if (g_audio) { AAudioStream_requestStop(g_audio); AAudioStream_close(g_audio); }
    if (sess) xrDestroySession(sess);
    if (inst) xrDestroyInstance(inst);
    (*app->activity->vm)->DetachCurrentThread(app->activity->vm);
}
