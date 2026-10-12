/*
 * vr_system - see vr_system.h.
 *
 * The library is reached by export offset (app_vr_bind); its parameters
 * were read from the 13.60 library (research/dump):
 *
 * sceVrHandInitialize(param): 0x40 bytes
 *   +0x00 size 0x40, +0x04 0
 *   +0x08 thread priority 256..767, +0x10 CPU mask <= 0x1fff
 *   +0x18 compute pipe <= 6, +0x1c queue <= 7
 *   +0x20 work memory, +0x28 its size (>= 60 MB), +0x2c >= 64 KB
 *   +0x30 0 = 60 Hz, 1 = 30 Hz; +0x34 input mode 0, 1 or 32; +0x38.. 0
 * sceVrHandStart(param): 0x10 bytes, +0 size, the rest 0.
 * sceVrHandGetResult(param, result): param 0x30 bytes (+0 size, +0x10 time,
 * +0x18 space 0/1); result 0xdb8 bytes, +8 the space, +0xc 0 unless tracking.
 */
#include "vr_system.h"

#include "app_vr.h"

#include "eng_boot_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);
uint64_t sceKernelGetProcessTime(void);

/* -- binding ---------------------------------------------------------------- */
static int (*sceVrHandInitialize)(const void *param);
static int (*sceVrHandFinalize)(void);
static int (*sceVrHandStart)(const void *param);
static int (*sceVrHandStop)(void);
static int (*sceVrHandGetResult)(const void *param, void *result);

static int s_hand_ok;

/* The app info hand tracking checks (+0x2c, +0x4b, bit 2 of +0x4f). */
static void log_app_info(void)
{
    uint8_t ai[0x98];
    memset(ai, 0, sizeof ai);
    int mib[4] = {1, 14, 35, getpid()};           /* kern.proc.appinfo */
    size_t len = sizeof ai;
    const int rc = sysctl(mib, 4, ai, &len, NULL, 0);
    char hex[0x98 * 2 + 1];
    for (size_t i = 0; i < sizeof ai; i++)
        snprintf(hex + i * 2, 3, "%02x", ai[i]);
    eng_bt("vr: app info rc %d len %zu: %.152s", rc, len, hex);
    eng_bt("vr: app info ... %s", hex + 152);
}

/* libSceSystemService exports it from its libSceShellCoreUtil library, which
 * the app's import table cannot name: the loader leaves such an import NULL. */
static int (*sceShellCoreUtilExitApp)(void);

int vr_system_exit_app(void)
{
    return sceShellCoreUtilExitApp ? sceShellCoreUtilExitApp() : -1;
}

void vr_system_preload(void)
{
    log_app_info();
    {
        static const char *const n[] = {"sceShellCoreUtilExitApp"};
        void *f[1];
        if (app_vr_bind("libSceSystemService", n, f, 1) == 0)
            *(void **)&sceShellCoreUtilExitApp = f[0];
    }
    static const char *const hand[] = {"sceVrHandInitialize", "sceVrHandFinalize", "sceVrHandStart",
                                       "sceVrHandStop", "sceVrHandGetResult"};
    void *h[5];
    if (app_vr_bind("libSceVrHand", hand, h, 5) == 0) {
        *(void **)&sceVrHandInitialize = h[0];
        *(void **)&sceVrHandFinalize = h[1];
        *(void **)&sceVrHandStart = h[2];
        *(void **)&sceVrHandStop = h[3];
        *(void **)&sceVrHandGetResult = h[4];
        s_hand_ok = 1;
    }
    eng_bt("vr: hand tracking %s", s_hand_ok ? "ready" : "unavailable");
}

/* -- hand tracking ------------------------------------------------------------ */
#define HAND_WORK      0x3c00000u           /* 60 MB, the library's minimum */
#define HAND_RESULT    0xdb8
static volatile int s_hand_want;            /* from the control thread */
static volatile int s_hand_dump;
static int s_hand_on;
static void *s_hand_mem;
static uint8_t s_hand_res[HAND_RESULT];
static vr_hand_t s_hands[2];

static volatile int s_hand_min_level;        /* test: credentials at least this */

void vr_hands_request(int what)
{
    if (what == 2) {
        s_hand_dump = 1;
    } else if (what >= 11 && what <= 13) {
        s_hand_min_level = what - 10;
        s_hand_want = 1;
    } else {
        s_hand_want = what != 0;
    }
}

int vr_hands_on(void)
{
    return s_hand_on;
}

/* The hand models. libSceVrHand asks the system's tracker daemon for them
 * through libSceVrTracker2 (export 0x7f50: a "set VR model" command, then the
 * file over /system_tmp/VrTracker2DomainSock), and the daemon sends PS5VR
 * nothing. They are the files in /system/priv/vr_hand_ro: model 2v+1 is
 * handCenter_%02d, 2v+2 handPose_%02d (libSceVrHand's own naming). So the
 * library's import of that export is pointed here, with the same contract:
 * 0 and *out = bytes, 0x8a740006 bad args, 0x8a740009 buffer too small. */
#define VRHAND_GOT_MODEL      0xf4e68       /* libSceVrHand's slot for it */
#define VRTRACKER2_MODEL_FN   0x7f50

static int model_read(int id, void *buf, size_t size, size_t *out)
{
    if (!buf || !out || id < 1 || id > 6 || !((0x46 >> id) & 1))
        return (int)0x8a740006;
    char path[96];
    snprintf(path, sizeof path, "/system/priv/vr_hand_ro/%s_%02d.bin",
             (id & 1) ? "handCenter" : "handPose", (id & 1) ? (id - 1) / 2 : (id - 2) / 2);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        eng_bt("vr: hand model %s: open failed (0x%x)", path, errno);
        *out = 0;
        return 0;
    }
    size_t got = 0;
    for (;;) {
        const ssize_t n = read(fd, (uint8_t *)buf + got, size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (got == size)
            break;
    }
    char more;
    const int too_big = got == size && read(fd, &more, 1) == 1;
    close(fd);
    eng_bt("vr: hand model %d <- %s, %zu bytes%s", id, path, got, too_big ? " (buffer too small)" : "");
    *out = got;
    return too_big ? (int)0x8a740009 : 0;
}

static int s_models_patched;

static void patch_model_import(void)
{
    if (s_models_patched || !sceVrHandInitialize)
        return;
    uintptr_t hand, tracker;
    if (!app_vr_hand_model_bases(&hand, &tracker)) {
        eng_bt("vr: hand model import patch unavailable for this firmware");
        return;
    }
    uint64_t *slot = (uint64_t *)(hand + VRHAND_GOT_MODEL);
    if (!tracker || *slot != tracker + VRTRACKER2_MODEL_FN) {
        eng_bt("vr: hand model import not as expected (%p), left alone", (void *)*slot);
        return;
    }
    void *page = (void *)((uintptr_t)slot & ~(uintptr_t)0x3fff);
    if (mprotect(page, 0x4000, PROT_READ | PROT_WRITE) != 0) {
        eng_bt("vr: hand model import: mprotect failed (0x%x)", errno);
        return;
    }
    *slot = (uint64_t)(uintptr_t)model_read;
    mprotect(page, 0x4000, PROT_READ);
    s_models_patched = 1;
    eng_bt("vr: hand models read from /system/priv/vr_hand_ro");
}

/* One line to ps5vr-helper (127.0.0.1:9079), its answer into reply. */
static int helper_ask(const char *req, char *reply, size_t size)
{
    reply[0] = 0;
    const int sk = socket(AF_INET, SOCK_STREAM, 0);
    if (sk < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(9079);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = {2, 0};
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int n = -1;
    if (connect(sk, (struct sockaddr *)&a, sizeof a) == 0 &&
        send(sk, req, strlen(req), 0) == (ssize_t)strlen(req))
        n = (int)recv(sk, reply, size - 1, 0);
    close(sk);
    if (n > 0)
        reply[n] = 0;
    return n > 0 ? 0 : -1;
}

/* An app may not open the headset cameras (/dev/hmd2_image, _leddet answer it
 * 0x8a721003, and initialisation then stops at 0x8a8700ff): ps5vr-helper
 * raises PS5VR's credentials a step at a time until it can. */
static int cameras_open(void)
{
    for (int level = 0; level <= 3; level++) {
        if (level > 0) {
            char req[48], reply[160];
            snprintf(req, sizeof req, "elevate %d %d\n", (int)getpid(), level);
            if (helper_ask(req, reply, sizeof reply) != 0) {
                eng_bt("vr: hand tracking needs ps5vr-helper, which is not running");
                return 0;
            }
            eng_bt("vr: ps5vr-helper: %s", reply);
        }
        /* the camera images and the controller-LED detector */
        static const char *const kDevices[] = {"/dev/hmd2_image", "/dev/hmd2_leddet"};
        int open_n = 0;
        for (int d = 0; d < 2; d++) {
            const int fd = open(kDevices[d], O_RDWR);
            if (fd >= 0) {
                close(fd);
                open_n++;
            } else {
                eng_bt("vr: %s refused at level %d (0x%x)", kDevices[d], level, errno);
            }
        }
        if (open_n == 2 && level >= s_hand_min_level) {
            eng_bt("vr: headset cameras open to PS5VR (level %d)", level);
            app_vr_camera_probe();
            return 1;
        }
    }
    return 0;
}

static int hands_start(void)
{
    if (!s_hand_mem) {
        int64_t off = 0;
        void *va = NULL;
        if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, HAND_WORK, 0x10000, 12, &off) != 0 ||
            sceKernelMapDirectMemory(&va, HAND_WORK, 0x33, 0, off, 0x10000) != 0) {
            eng_bt("vr: hand tracking memory failed");
            return -1;
        }
        s_hand_mem = va;
    }
    uint8_t p[0x40];
    memset(p, 0, sizeof p);
    *(uint32_t *)(p + 0x00) = sizeof p;
    *(int32_t *)(p + 0x08) = 0x1c0;                 /* priority */
    *(uint64_t *)(p + 0x10) = 0x1fff;               /* CPUs */
    *(uint32_t *)(p + 0x18) = 5;                    /* compute pipe */
    *(uint32_t *)(p + 0x1c) = 0;                    /* queue */
    *(void **)(p + 0x20) = s_hand_mem;
    *(uint32_t *)(p + 0x28) = HAND_WORK;
    *(uint32_t *)(p + 0x2c) = 0x10000;
    if (!cameras_open())
        return -1;
    patch_model_import();
    int rc = sceVrHandInitialize(p);
    eng_bt("vr: hand tracking initialise -> 0x%08x", rc);
    if (rc != 0)
        return -1;
    uint8_t sp[0x10];
    memset(sp, 0, sizeof sp);
    *(uint32_t *)sp = sizeof sp;
    rc = sceVrHandStart(sp);
    eng_bt("vr: hand tracking start -> 0x%08x", rc);
    if (rc != 0) {
        sceVrHandFinalize();
        return -1;
    }
    return 0;
}

static void hands_tick(void)
{
    if (!s_hand_ok)
        return;
    if (s_hand_want && !s_hand_on) {
        s_hand_on = hands_start() == 0;
        if (!s_hand_on)
            s_hand_want = 0;
    } else if (!s_hand_want && s_hand_on) {
        eng_bt("vr: hand tracking stop 0x%08x finalize 0x%08x", sceVrHandStop(), sceVrHandFinalize());
        s_hand_on = 0;
    }
    if (!s_hand_on)
        return;
    /* One hand per call (+0x18). The result: +0 time (us), +8 the hand, +0xc
     * its state (3 tracked), then 26 joints of 0x60 bytes in OpenXR order
     * (palm, wrist, thumb 4, four fingers 5 each): position at +0 (tracker
     * space, metres), orientation at +0x10, radius at +0x30. */
    static int s_state[2] = {-1, -1};
    for (int h = 0; h < 2; h++) {
        uint8_t gp[0x30];
        memset(gp, 0, sizeof gp);
        *(uint32_t *)gp = sizeof gp;
        *(uint64_t *)(gp + 0x10) = app_vr_frame_time();
        *(uint32_t *)(gp + 0x18) = (uint32_t)h;
        if (sceVrHandGetResult(gp, s_hand_res) != 0) {
            s_hands[h].tracked = 0;
            continue;
        }
        const int state = (int)*(const uint32_t *)(s_hand_res + 0xc);
        s_state[h] = state;
        /* Tracking drops for a frame or two now and then: keep the last pose
         * for 15 frames (0.25 s) so a pointer does not flicker. */
        static int s_lost_frames[2];
        if (state != 3) {
            if (s_hands[h].tracked && ++s_lost_frames[h] <= 15)
                continue;                       /* hold the last pose */
            s_hands[h].tracked = 0;
            continue;
        }
        s_lost_frames[h] = 0;
        s_hands[h].tracked = 1;
        for (int j = 0; j < VR_HAND_JOINTS; j++) {
            const float *p = (const float *)(s_hand_res + 0x10 + j * 0x60);
            s_hands[h].joint[j][0] = p[0];
            s_hands[h].joint[j][1] = p[1];
            s_hands[h].joint[j][2] = p[2];
            s_hands[h].radius[j] = p[12];
        }
        if (s_hand_dump && h == 1) {
            s_hand_dump = 0;
            FILE *fp = fopen("/data/ps5vr/hand.bin", "wb");
            if (fp) {
                fwrite(s_hand_res, 1, sizeof s_hand_res, fp);
                fclose(fp);
            }
            eng_bt("vr: hand result saved (%s)", fp ? "ok" : "failed");
        }
    }
}

int vr_hands_get(int hand, vr_hand_t *out)
{
    if (!s_hand_on || hand < 0 || hand > 1 || !s_hands[hand].tracked)
        return 0;
    *out = s_hands[hand];
    return 1;
}

void vr_system_shutdown(void)
{
    s_hand_want = 0;
    hands_tick();
}

void vr_system_tick(void)
{
    hands_tick();
    static unsigned s_frames, s_seen[2];
    if (s_hand_on) {
        s_frames++;
        for (int h = 0; h < 2; h++)
            s_seen[h] += s_hands[h].tracked != 0;
        if (s_frames == 600) {                  /* every 10 s */
            eng_bt("vr: hands tracked %u%% / %u%% of the last 10 s", s_seen[0] * 100 / s_frames,
                   s_seen[1] * 100 / s_frames);
            s_frames = s_seen[0] = s_seen[1] = 0;
        }
    }
}
