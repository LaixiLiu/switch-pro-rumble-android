/*
 * switch_rumble_bridge - Nintendo Switch Pro Controller rumble bridge for Android
 *
 * Creates a uinput mirror of the real Bluetooth controller with FF_RUMBLE
 * capability, grabs the real evdev node, forwards all input events, and
 * translates force-feedback requests into Nintendo 0x10 rumble reports
 * written directly to the controller's hidraw node.
 *
 * FF protocol (per kernel drivers/input/misc/uinput.c):
 *   read() delivers:
 *     {EV_UINPUT(0x0101), UI_FF_UPLOAD(1), request_id}  -> UI_BEGIN_FF_UPLOAD /
 *        UI_END_FF_UPLOAD ioctl round-trip carries struct ff_effect
 *     {EV_UINPUT, UI_FF_ERASE(2), request_id}           -> UI_BEGIN/END_FF_ERASE
 *     {EV_FF, effect_id, 1|0}                           -> play / stop (direct)
 *     {EV_FF, FF_GAIN, gain}                            -> ignored
 *
 * Rumble encoding: SDL3 tables (SDL_hidapi_switch.c), derived from
 * dekuNukem/Nintendo_Switch_Reverse_Engineering rumble_data_table.md.
 * Frequencies fixed at SDL's experimentally-determined values
 * (high band 0x0074, low band 0x3D).
 *
 * Usage:
 *   switch_rumble_bridge [--hidraw /dev/hidrawN] [--evdev /dev/input/eventN]
 *                        [--selftest [strong weak ms]]
 * Auto-discovers a Switch Pro Controller (057e:2009) when no paths given.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <linux/uinput.h>

/*
 * Note: kernel 6.1 struct uinput_ff_upload has no effect_id field — the
 * assigned effect id arrives inside uinput_ff_upload.effect.id. bionic's
 * frozen header matches this, so the stock structs are used directly.
 */

#define BT_NINTENDO_PRO_VENDOR 0x057e
#define BT_NINTENDO_PRO_PRODUCT 0x2009
#define MAX_FX 64
#define REFRESH_MS 50
#define AUTOSTOP_GRACE_MS 300

static volatile sig_atomic_t g_quit = 0;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[%lld] ", now_ms());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ------------------------------------------------------------------ */
/* SDL3 amplitude tables (SDL_hidapi_switch.c EncodeRumbleHighAmplitude /
 * EncodeRumbleLowAmplitude), input 0..65535, output protocol encoding. */
static const unsigned short k_hfa[101][2] = {
    {0,0x0},{514,0x2},{775,0x4},{921,0x6},{1096,0x8},{1303,0x0a},{1550,0x0c},
    {1843,0x0e},{2192,0x10},{2606,0x12},{3100,0x14},{3686,0x16},{4383,0x18},
    {5213,0x1a},{6199,0x1c},{7372,0x1e},{7698,0x20},{8039,0x22},{8395,0x24},
    {8767,0x26},{9155,0x28},{9560,0x2a},{9984,0x2c},{10426,0x2e},{10887,0x30},
    {11369,0x32},{11873,0x34},{12398,0x36},{12947,0x38},{13520,0x3a},{14119,0x3c},
    {14744,0x3e},{15067,0x40},{15397,0x42},{15734,0x44},{16079,0x46},{16431,0x48},
    {16790,0x4a},{17158,0x4c},{17534,0x4e},{17918,0x50},{18310,0x52},{18711,0x54},
    {19121,0x56},{19540,0x58},{19967,0x5a},{20405,0x5c},{20851,0x5e},{21308,0x60},
    {21775,0x62},{22251,0x64},{22739,0x66},{23236,0x68},{23745,0x6a},{24265,0x6c},
    {24797,0x6e},{25340,0x70},{25894,0x72},{26462,0x74},{27041,0x76},{27633,0x78},
    {28238,0x7a},{28856,0x7c},{29488,0x7e},{30134,0x80},{30794,0x82},{31468,0x84},
    {32157,0x86},{32861,0x88},{33581,0x8a},{34316,0x8c},{35068,0x8e},{35836,0x90},
    {36620,0x92},{37422,0x94},{38242,0x96},{39079,0x98},{39935,0x9a},{40809,0x9c},
    {41703,0x9e},{42616,0xa0},{43549,0xa2},{44503,0xa4},{45477,0xa6},{46473,0xa8},
    {47491,0xaa},{48531,0xac},{49593,0xae},{50679,0xb0},{51789,0xb2},{52923,0xb4},
    {54082,0xb6},{55266,0xb8},{56476,0xba},{57713,0xbc},{58977,0xbe},{60268,0xc0},
    {61588,0xc2},{62936,0xc4},{64315,0xc6},{65535,0xc8}
};

static const unsigned short k_lfa[101][2] = {
    {0,0x0040},{514,0x8040},{775,0x0041},{921,0x8041},{1096,0x0042},{1303,0x8042},
    {1550,0x0043},{1843,0x8043},{2192,0x0044},{2606,0x8044},{3100,0x0045},{3686,0x8045},
    {4383,0x0046},{5213,0x8046},{6199,0x0047},{7372,0x8047},{7698,0x0048},{8039,0x8048},
    {8395,0x0049},{8767,0x8049},{9155,0x004a},{9560,0x804a},{9984,0x004b},{10426,0x804b},
    {10887,0x004c},{11369,0x804c},{11873,0x004d},{12398,0x804d},{12947,0x004e},{13520,0x804e},
    {14119,0x004f},{14744,0x804f},{15067,0x0050},{15397,0x8050},{15734,0x0051},{16079,0x8051},
    {16431,0x0052},{16790,0x8052},{17158,0x0053},{17534,0x8053},{17918,0x0054},{18310,0x8054},
    {18711,0x0055},{19121,0x8055},{19540,0x0056},{19967,0x8056},{20405,0x0057},{20851,0x8057},
    {21308,0x0058},{21775,0x8058},{22251,0x0059},{22739,0x8059},{23236,0x005a},{23745,0x805a},
    {24265,0x005b},{24797,0x805b},{25340,0x005c},{25894,0x805c},{26462,0x005d},{27041,0x805d},
    {27633,0x005e},{28238,0x805e},{28856,0x005f},{29488,0x805f},{30134,0x0060},{30794,0x8060},
    {31468,0x0061},{32157,0x8061},{32861,0x0062},{33581,0x8062},{34316,0x0063},{35068,0x8063},
    {35836,0x0064},{36620,0x8064},{37422,0x0065},{38242,0x8065},{39079,0x0066},{39935,0x8066},
    {40809,0x0067},{41703,0x8067},{42616,0x0068},{43549,0x8068},{44503,0x0069},{45477,0x8069},
    {46473,0x006a},{47491,0x806a},{48531,0x006b},{49593,0x806b},{50679,0x006c},{51789,0x806c},
    {52923,0x006d},{54082,0x806d},{55266,0x006e},{56476,0x806e},{57713,0x006f},{58977,0x806f},
    {60268,0x0070},{61588,0x8070},{62936,0x0071},{64315,0x8071},{65535,0x0072}
};

static unsigned short amp_lookup(const unsigned short tab[][2], unsigned short mag)
{
    int i;
    for (i = 0; i < 101; i++)
        if (mag <= tab[i][0])
            return tab[i][1];
    return tab[100][1];
}

/* Frequency codes: SDL3 uses 150 Hz (0x074 / 0x3D), but this unit does not
 * respond to that band — empirically verified working is the Linux kernel
 * default pair 160 Hz low / 320 Hz high (codes 0x40 / 0x001).
 * Encoding (both drivers agree): byte0 = HF code high byte; byte1 = HF code
 * bit0 + HF amplitude; byte2 = LF code + LF amplitude bit 8; byte3 = LF amp. */
#define FREQ_LOW_CODE  0x40
#define FREQ_HIGH_CODE 0x0001

static unsigned char g_rumble_seq = 0;

static int g_verbose_writes = 0;

static int write_report(int hidraw_fd, const unsigned char motor[4])
{
    unsigned char rep[10];
    ssize_t w;
    rep[0] = 0x10;
    rep[1] = g_rumble_seq++ & 0x0F;
    memcpy(rep + 2, motor, 4);
    memcpy(rep + 6, motor, 4);
    w = write(hidraw_fd, rep, sizeof(rep));
    if (w != (ssize_t)sizeof(rep)) {
        /* caller treats ENODEV/EBADF as link loss and reconnects */
        return -1;
    }
    if (g_verbose_writes)
        logmsg("hidraw wr seq=%u motor=%02x %02x %02x %02x",
               rep[1], motor[0], motor[1], motor[2], motor[3]);
    return 0;
}

/* Global strength multiplier in percent (100 = as the game asked).
 * Applied to the magnitude before table lookup, so weak game rumbles get
 * boosted; the protocol ceiling (0xC8/0x0072) still caps the output. */
static int g_gain_pct = 100;

static int send_rumble(int hidraw_fd, unsigned short strong, unsigned short weak)
{
    unsigned int s = (unsigned int)strong * (unsigned int)g_gain_pct / 100;
    unsigned int w = (unsigned int)weak * (unsigned int)g_gain_pct / 100;
    unsigned short enc_h, enc_l;
    unsigned char motor[4];

    if (s > 65535) s = 65535;
    if (w > 65535) w = 65535;
    enc_h = amp_lookup(k_hfa, (unsigned short)w);
    enc_l = amp_lookup(k_lfa, (unsigned short)s);
    motor[0] = (FREQ_HIGH_CODE >> 8) & 0xFF;
    motor[1] = (unsigned char)((FREQ_HIGH_CODE & 0xFF) + enc_h);
    motor[2] = (unsigned char)(FREQ_LOW_CODE + ((enc_l >> 8) & 0x80));
    motor[3] = enc_l & 0xFF;
    return write_report(hidraw_fd, motor);
}

static int send_neutral(int hidraw_fd)
{
    static const unsigned char neutral[4] = { 0x00, 0x01, 0x40, 0x40 };
    return write_report(hidraw_fd, neutral);
}

/* ------------------------------------------------------------------ */
/* Device discovery */

static int test_bit(const unsigned char *bits, unsigned int n);

/* The controller exposes several input nodes (gamepad, IMU). Pick the one
 * carrying key events: that is the gamepad node. IMU-only nodes are avoided. */
static int find_pro_controller(char *hidraw_out, size_t hidraw_len,
                               char *evdev_out, size_t evdev_len)
{
    char path[PATH_MAX], link[PATH_MAX], hid_sys[PATH_MAX];
    char best[PATH_MAX] = "";
    int best_score = -1;
    DIR *d;
    struct dirent *de;

    hid_sys[0] = '\0';
    d = opendir("/sys/class/hidraw");
    if (!d)
        return -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "hidraw", 6) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device", de->d_name);
        if (!realpath(path, link))
            continue;
        if (strstr(link, "0005:057E:2009.")) {
            snprintf(hidraw_out, hidraw_len, "/dev/%s", de->d_name);
            strncpy(hid_sys, link, sizeof(hid_sys) - 1);
            break;
        }
    }
    closedir(d);
    if (!hid_sys[0])
        return -1;

    d = opendir("/sys/class/input");
    if (!d)
        return -1;
    while ((de = readdir(d))) {
        char name[128] = "";
        unsigned char evbits[EV_MAX / 8 + 1];
        unsigned char keybits[KEY_CNT / 8];
        int fd, score = 0;

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s/device", de->d_name);
        if (!realpath(path, link))
            continue;
        if (strncmp(link, hid_sys, strlen(hid_sys)) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(evbits, 0, sizeof(evbits));
        memset(keybits, 0, sizeof(keybits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        close(fd);

        if (test_bit(evbits, EV_ABS))
            score += 1;
        if (test_bit(evbits, EV_KEY))
            score += 2;
        if (strstr(name, "IMU") || strstr(name, "Motion"))
            score -= 20;
        if (score > best_score) {
            best_score = score;
            strncpy(best, path, sizeof(best) - 1);
        }
    }
    closedir(d);
    if (best_score < 0 || !best[0])
        return -1;
    strncpy(evdev_out, best, evdev_len - 1);
    evdev_out[evdev_len - 1] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* FF effect bookkeeping */

struct fx_state {
    int used, active;
    unsigned short type, strong, weak, length;
    long long start_ms;
};

static struct fx_state g_fx[MAX_FX];
static int g_fx_max = MAX_FX;

static int combine_active(unsigned short *strong, unsigned short *weak)
{
    int i, any = 0;
    *strong = *weak = 0;
    for (i = 0; i < g_fx_max; i++) {
        if (g_fx[i].used && g_fx[i].active) {
            if (g_fx[i].strong > *strong) *strong = g_fx[i].strong;
            if (g_fx[i].weak > *weak) *weak = g_fx[i].weak;
            any = 1;
        }
    }
    return any;
}

/* ------------------------------------------------------------------ */
/* uinput mirror setup */

struct dev_caps {
    char name[80];
    char uniq[80];
    struct input_id id;
    unsigned char evbits[EV_MAX / 8 + 1];
    unsigned char keybits[KEY_CNT / 8];
    unsigned char absbits[ABS_CNT / 8];
    unsigned char mscbits[MSC_MAX / 8 + 1];
    unsigned char propbits[INPUT_PROP_CNT / 8 + 1];
    struct input_absinfo absinfo[ABS_CNT];
};

static int read_caps(int fd, struct dev_caps *c)
{
    unsigned int i;
    memset(c, 0, sizeof(*c));
    if (ioctl(fd, EVIOCGNAME(sizeof(c->name) - 1), c->name) < 0)
        c->name[0] = '\0';
    if (ioctl(fd, EVIOCGUNIQ(sizeof(c->uniq) - 1), c->uniq) < 0)
        c->uniq[0] = '\0';
    if (ioctl(fd, EVIOCGID, &c->id) < 0)
        memset(&c->id, 0, sizeof(c->id));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(c->evbits)), c->evbits) < 0)
        return -1;
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(c->keybits)), c->keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(c->absbits)), c->absbits);
    ioctl(fd, EVIOCGBIT(EV_MSC, sizeof(c->mscbits)), c->mscbits);
    ioctl(fd, EVIOCGPROP(sizeof(c->propbits)), c->propbits);
    for (i = 0; i < ABS_CNT; i++)
        if (c->absbits[i / 8] & (1 << (i % 8)))
            if (ioctl(fd, EVIOCGABS(i), &c->absinfo[i]) < 0)
                memset(&c->absinfo[i], 0, sizeof(c->absinfo[i]));
    return 0;
}

static int test_bit(const unsigned char *bits, unsigned int n)
{
    return bits[n / 8] & (1 << (n % 8));
}

static int set_bit_ioctl(int uinput_fd, int set, unsigned int bit)
{
    return ioctl(uinput_fd, set, (int)(long)bit) == -1 ? -1 : 0;
}

static int create_mirror(int uinput_fd, const struct dev_caps *c, int fx_max)
{
    struct uinput_setup setup;
    unsigned int i, ev;

    memset(&setup, 0, sizeof(setup));
    strncpy(setup.name, c->name, sizeof(setup.name) - 1);
    setup.id = c->id;
    setup.ff_effects_max = fx_max;
    if (ioctl(uinput_fd, UI_DEV_SETUP, &setup) == -1) {
        logmsg("UI_DEV_SETUP failed: %s", strerror(errno));
        return -1;
    }

    for (ev = 0; ev < EV_MAX; ev++) {
        if (!test_bit(c->evbits, ev))
            continue;
        if (ev == EV_KEY || ev == EV_ABS || ev == EV_MSC) {
            if (ioctl(uinput_fd, UI_SET_EVBIT, (int)ev) == -1) {
                logmsg("UI_SET_EVBIT(%u) failed: %s", ev, strerror(errno));
                return -1;
            }
        }
        /* EV_SYN is implicit; anything else is not mirrored */
    }

    for (i = 0; i < KEY_CNT; i++)
        if (test_bit(c->keybits, i))
            if (set_bit_ioctl(uinput_fd, UI_SET_KEYBIT, i))
                return -1;
    for (i = 0; i < ABS_CNT; i++)
        if (test_bit(c->absbits, i)) {
            struct uinput_abs_setup abs;
            if (set_bit_ioctl(uinput_fd, UI_SET_ABSBIT, i))
                return -1;
            memset(&abs, 0, sizeof(abs));
            abs.code = (__u16)i;
            abs.absinfo = c->absinfo[i];
            if (ioctl(uinput_fd, UI_ABS_SETUP, &abs) == -1) {
                logmsg("UI_ABS_SETUP(%u) failed: %s", i, strerror(errno));
                return -1;
            }
        }
    for (i = 0; i <= MSC_MAX; i++)
        if (test_bit(c->mscbits, i))
            if (set_bit_ioctl(uinput_fd, UI_SET_MSCBIT, i))
                return -1;
    for (i = 0; i < INPUT_PROP_CNT; i++)
        if (test_bit(c->propbits, i))
            if (set_bit_ioctl(uinput_fd, UI_SET_PROPBIT, i))
                return -1;

    if (ioctl(uinput_fd, UI_SET_EVBIT, (int)EV_FF) == -1) {
        logmsg("UI_SET_EVBIT(EV_FF) failed: %s", strerror(errno));
        return -1;
    }
    if (ioctl(uinput_fd, UI_SET_FFBIT, (int)FF_RUMBLE) == -1) {
        logmsg("UI_SET_FFBIT(FF_RUMBLE) failed: %s", strerror(errno));
        return -1;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) == -1) {
        logmsg("UI_DEV_CREATE failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Resolve the virtual device's first event node via UI_GET_SYSNAME. */
static int mirror_event_node(int uinput_fd, char *out, size_t out_len)
{
    char sysname[64];
    char dirpath[PATH_MAX], path[PATH_MAX], link[PATH_MAX];
    DIR *d;
    struct dirent *de;

    if (ioctl(uinput_fd, UI_GET_SYSNAME(63), sysname) < 0) {
        logmsg("UI_GET_SYSNAME failed: %s", strerror(errno));
        return -1;
    }
    snprintf(dirpath, sizeof(dirpath), "/sys/class/input/%s", sysname);
    d = opendir(dirpath);
    if (!d)
        return -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
        if (realpath(path, link)) {
            snprintf(out, out_len, "/dev/input/%s", de->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Input forwarding */

static int forward_resync(int evfd, int uinput_fd, const struct dev_caps *c)
{
    unsigned char keybits[KEY_CNT / 8];
    struct input_event ev;
    unsigned int i;

    logmsg("resync after SYN_DROPPED");
    if (ioctl(evfd, EVIOCGKEY(sizeof(keybits)), keybits) >= 0)
        for (i = 0; i < KEY_CNT; i++)
            if (test_bit(c->keybits, i) && test_bit(keybits, i)) {
                memset(&ev, 0, sizeof(ev));
                ev.type = EV_KEY; ev.code = i; ev.value = 1;
                if (write(uinput_fd, &ev, sizeof(ev)) < 0)
                    return -1;
            }
    for (i = 0; i < ABS_CNT; i++)
        if (test_bit(c->absbits, i)) {
            struct input_absinfo ai;
            if (ioctl(evfd, EVIOCGABS(i), &ai) >= 0) {
                memset(&ev, 0, sizeof(ev));
                ev.type = EV_ABS; ev.code = i; ev.value = ai.value;
                if (write(uinput_fd, &ev, sizeof(ev)) < 0)
                    return -1;
            }
        }
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    if (write(uinput_fd, &ev, sizeof(ev)) < 0)
        return -1;
    return 0;
}

static int forward_batch(int evfd, int uinput_fd, const struct dev_caps *c)
{
    struct input_event evs[32];
    ssize_t n;
    int i, dropped = 0;

    n = read(evfd, evs, sizeof(evs));
    if (n <= 0) {
        if (n == 0 || errno == EAGAIN)
            return 0;
        logmsg("evdev read error: %s", strerror(errno));
        return -1;
    }
    for (i = 0; i < (int)(n / sizeof(evs[0])); i++) {
        if (evs[i].type == EV_SYN && evs[i].code == SYN_DROPPED) {
            dropped = 1;
            break;
        }
    }
    if (dropped)
        return forward_resync(evfd, uinput_fd, c);
    if (write(uinput_fd, evs, (size_t)n) != n) {
        logmsg("uinput forward write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Self-test: simulate the exact kernel path the framework uses
 * (EVIOCSFF + write EV_FF) against our own virtual device.
 *
 * EVIOCSFF blocks until the uinput owner answers UI_FF_UPLOAD, so this
 * MUST run in a separate process: the parent is the owner and only
 * answers from its poll loop. */

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int selftest_child(int uinput_fd, unsigned short strong,
                          unsigned short weak, int length_ms)
{
    char node[PATH_MAX];
    struct ff_effect eff;
    struct input_event ev;
    int fd, fx = -1, ok = 0;

    sleep_ms(800);
    if (mirror_event_node(uinput_fd, node, sizeof(node)) != 0) {
        logmsg("selftest: cannot resolve virtual event node");
        return 1;
    }
    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        logmsg("selftest: open %s failed: %s", node, strerror(errno));
        return 1;
    }
    logmsg("selftest: virtual node %s", node);

    memset(&eff, 0, sizeof(eff));
    eff.type = FF_RUMBLE;
    eff.id = -1;
    eff.u.rumble.strong_magnitude = strong;
    eff.u.rumble.weak_magnitude = weak;
    eff.replay.length = (unsigned short)length_ms;
    if (ioctl(fd, EVIOCSFF, &eff) != 0) {
        logmsg("selftest: EVIOCSFF failed: %s", strerror(errno));
        close(fd);
        return 1;
    }
    fx = eff.id;
    logmsg("selftest: uploaded effect id=%d strong=0x%04x weak=0x%04x len=%dms",
           fx, strong, weak, length_ms);

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF; ev.code = fx; ev.value = 1;
    if (write(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        logmsg("selftest: play sent");
        ok = 1;
    } else {
        logmsg("selftest: play write failed: %s", strerror(errno));
    }

    sleep_ms(length_ms + 800);

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF; ev.code = fx; ev.value = 0;
    if (write(fd, &ev, sizeof(ev)) == sizeof(ev))
        logmsg("selftest: stop sent");
    sleep_ms(300);
    ioctl(fd, EVIOCRMFF, fx);
    close(fd);
    logmsg("selftest: child complete");
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Open and grab the controller. Used at startup and for reconnects. */

static int open_and_grab(const char *hidraw_path, const char *evdev_path,
                         int *hidraw_out, int *ev_out, struct dev_caps *caps)
{
    int hfd, efd;

    hfd = open(hidraw_path, O_WRONLY | O_CLOEXEC);
    if (hfd < 0)
        return -1;
    efd = open(evdev_path, O_RDWR | O_CLOEXEC);
    if (efd < 0) {
        close(hfd);
        return -1;
    }
    if (read_caps(efd, caps) != 0) {
        close(hfd);
        close(efd);
        return -1;
    }
    if (ioctl(efd, EVIOCGRAB, 1) == -1) {
        logmsg("EVIOCGRAB failed: %s", strerror(errno));
        close(hfd);
        close(efd);
        return -1;
    }
    *hidraw_out = hfd;
    *ev_out = efd;
    return 0;
}

static void drop_link(int *evfd, int *hidraw_fd, int *connected,
                      int *was_active, const char *why)
{
    if (*evfd >= 0)
        close(*evfd);
    if (*hidraw_fd >= 0)
        close(*hidraw_fd);
    *evfd = -1;
    *hidraw_fd = -1;
    *connected = 0;
    *was_active = 0;
    logmsg("controller link lost (%s); waiting for reconnect", why);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *hidraw_path = NULL, *evdev_path = NULL;
    char hidraw_auto[64], evdev_auto[64];
    int hidraw_fd = -1, evfd = -1, uinput_fd = -1;
    struct dev_caps caps;
    int was_active = 0;
    long long last_send = 0;
    int i;
    int selftest = 0, st_len = 1000, st_pid = -1, st_status = 0;
    unsigned short st_strong = 0xFFFF, st_weak = 0xFFFF;
    int connected = 1;
    long long last_reconnect_try = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hidraw") && i + 1 < argc)
            hidraw_path = argv[++i];
        else if (!strcmp(argv[i], "--evdev") && i + 1 < argc)
            evdev_path = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) {
            selftest = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') st_strong = (unsigned short)strtoul(argv[++i], NULL, 0);
            if (i + 1 < argc && argv[i + 1][0] != '-') st_weak = (unsigned short)strtoul(argv[++i], NULL, 0);
            if (i + 1 < argc && argv[i + 1][0] != '-') st_len = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) {
            int v = (int)strtol(argv[++i], NULL, 10);
            if (v >= 10 && v <= 300)
                g_gain_pct = v;
            else
                logmsg("ignoring out-of-range gain %d%% (allowed 10..300)", v);
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    if (!hidraw_path || !evdev_path) {
        if (find_pro_controller(hidraw_auto, sizeof(hidraw_auto),
                                evdev_auto, sizeof(evdev_auto)) != 0) {
            logmsg("Switch Pro Controller not found (need 0005:057E:2009.* with an input node)");
            return 1;
        }
        if (!hidraw_path) hidraw_path = hidraw_auto;
        if (!evdev_path) evdev_path = evdev_auto;
    }
    logmsg("hidraw=%s evdev=%s", hidraw_path, evdev_path);

    if (open_and_grab(hidraw_path, evdev_path, &hidraw_fd, &evfd, &caps) != 0) {
        logmsg("failed to open/grab %s + %s: %s", hidraw_path, evdev_path, strerror(errno));
        return 1;
    }
    logmsg("mirroring \"%s\" uniq=%s bustype=0x%04x vendor=0x%04x product=0x%04x",
           caps.name, caps.uniq, caps.id.bustype, caps.id.vendor, caps.id.product);
    logmsg("grabbed real device (games will now see the mirror only)");

    uinput_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (uinput_fd < 0) {
        logmsg("open /dev/uinput failed: %s", strerror(errno));
        return 1;
    }
    if (create_mirror(uinput_fd, &caps, MAX_FX) != 0) {
        logmsg("failed to create uinput mirror");
        return 1;
    }
    logmsg("uinput mirror created with FF_RUMBLE, effects_max=%d, gain=%d%%",
           MAX_FX, g_gain_pct);
    g_fx_max = MAX_FX;

    send_neutral(hidraw_fd);

    if (selftest) {
        st_pid = fork();
        if (st_pid == 0) {
            _exit(selftest_child(uinput_fd, st_strong, st_weak, st_len));
        }
        if (st_pid < 0)
            logmsg("fork for selftest failed: %s", strerror(errno));
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_quit) {
        struct pollfd pfd[2];
        int r;
        long long t = now_ms();
        unsigned short strong, weak;
        int any_active;

        /* reconnect if the controller dropped off the bus */
        if (!connected && t - last_reconnect_try >= 2000) {
            char hp[64], ep[64];
            last_reconnect_try = t;
            if (find_pro_controller(hp, sizeof(hp), ep, sizeof(ep)) == 0) {
                struct dev_caps nc;
                int nh = -1, ne = -1;
                if (open_and_grab(hp, ep, &nh, &ne, &nc) == 0) {
                    hidraw_fd = nh;
                    evfd = ne;
                    connected = 1;
                    logmsg("controller reconnected: hidraw=%s evdev=%s", hp, ep);
                    send_neutral(hidraw_fd);
                }
            }
        }

        pfd[0].fd = connected ? evfd : -1;
        pfd[0].events = POLLIN;
        pfd[1].fd = uinput_fd;
        pfd[1].events = POLLIN;
        r = poll(pfd, 2, 10);
        if (r < 0 && errno != EINTR) {
            logmsg("poll failed: %s", strerror(errno));
            break;
        }

        if (connected && (pfd[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            int lost = 0;
            if (pfd[0].revents & POLLIN)
                if (forward_batch(evfd, uinput_fd, &caps) != 0)
                    lost = 1;
            if (pfd[0].revents & (POLLERR | POLLHUP))
                lost = 1;
            if (lost)
                drop_link(&evfd, &hidraw_fd, &connected, &was_active, "evdev");
        }

        if (pfd[1].revents & (POLLIN | POLLERR)) {
            struct input_event ev;
            ssize_t nread;
            while ((nread = read(uinput_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
                    struct uinput_ff_upload up;
                    memset(&up, 0, sizeof(up));
                    up.request_id = (unsigned int)ev.value;
                    if (ioctl(uinput_fd, UI_BEGIN_FF_UPLOAD, &up) == 0) {
                        int id = (int)up.effect.id;
                        if (id >= 0 && id < g_fx_max) {
                            g_fx[id].used = 1;
                            g_fx[id].active = 0;
                            g_fx[id].type = up.effect.type;
                            g_fx[id].strong = up.effect.u.rumble.strong_magnitude;
                            g_fx[id].weak = up.effect.u.rumble.weak_magnitude;
                            g_fx[id].length = up.effect.replay.length;
                            logmsg("ff upload id=%d type=0x%02x strong=0x%04x weak=0x%04x len=%ums",
                                   id, up.effect.type,
                                   g_fx[id].strong, g_fx[id].weak, g_fx[id].length);
                        }
                        up.retval = 0;
                        if (ioctl(uinput_fd, UI_END_FF_UPLOAD, &up) != 0)
                            logmsg("UI_END_FF_UPLOAD failed: %s", strerror(errno));
                    } else {
                        logmsg("UI_BEGIN_FF_UPLOAD failed: %s", strerror(errno));
                    }
                } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
                    struct uinput_ff_erase er;
                    memset(&er, 0, sizeof(er));
                    er.request_id = (unsigned int)ev.value;
                    if (ioctl(uinput_fd, UI_BEGIN_FF_ERASE, &er) == 0) {
                        if ((int)er.effect_id >= 0 && (int)er.effect_id < g_fx_max) {
                            g_fx[er.effect_id].used = 0;
                            g_fx[er.effect_id].active = 0;
                        }
                        er.retval = 0;
                        ioctl(uinput_fd, UI_END_FF_ERASE, &er);
                        logmsg("ff erase id=%u", er.effect_id);
                    }
                } else if (ev.type == EV_FF) {
                    if (ev.code == FF_GAIN) {
                        logmsg("ff gain=%d (ignored)", ev.value);
                    } else if (ev.code < (unsigned)g_fx_max && g_fx[ev.code].used) {
                        int id = ev.code;
                        if (ev.value > 0) {
                            g_fx[id].active = 1;
                            g_fx[id].start_ms = t;
                            logmsg("ff play id=%d strong=0x%04x weak=0x%04x",
                                   id, g_fx[id].strong, g_fx[id].weak);
                        } else {
                            g_fx[id].active = 0;
                            logmsg("ff stop id=%d", id);
                        }
                    }
                }
            }
        }

        /* safety: auto-stop effects whose declared length has long elapsed */
        for (i = 0; i < g_fx_max; i++) {
            if (g_fx[i].used && g_fx[i].active && g_fx[i].length > 0 &&
                t > g_fx[i].start_ms + g_fx[i].length + AUTOSTOP_GRACE_MS) {
                logmsg("auto-stop effect %d (len %ums elapsed)", i, g_fx[i].length);
                g_fx[i].active = 0;
            }
        }

        if (connected) {
            any_active = combine_active(&strong, &weak);
            if (any_active) {
                if (t - last_send >= REFRESH_MS || !was_active) {
                    if (send_rumble(hidraw_fd, strong, weak) != 0) {
                        if (errno == ENODEV || errno == EBADF || errno == ENXIO) {
                            drop_link(&evfd, &hidraw_fd, &connected, &was_active, "hidraw write");
                            continue;
                        }
                    }
                    last_send = t;
                }
                was_active = 1;
            } else if (was_active) {
                send_neutral(hidraw_fd);
                send_neutral(hidraw_fd);
                was_active = 0;
                logmsg("rumble off");
            }
        } else {
            was_active = 0;
        }

        if (st_pid > 0) {
            int w = waitpid(st_pid, &st_status, WNOHANG);
            if (w == st_pid) {
                logmsg("selftest child exited status=%d", WEXITSTATUS(st_status));
                st_pid = -1;
                g_quit = 1;
            }
        }
    }

    logmsg("shutting down");
    if (hidraw_fd >= 0)
        send_neutral(hidraw_fd);
    if (st_pid > 0) {
        kill(st_pid, SIGTERM);
        waitpid(st_pid, &st_status, 0);
    }
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    if (evfd >= 0)
        close(evfd);
    if (hidraw_fd >= 0)
        close(hidraw_fd);
    return 0;
}
