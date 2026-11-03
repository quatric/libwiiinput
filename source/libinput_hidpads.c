/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-FileCopyrightText: FIX94 and the Nintendont contributors, used with permission <https://github.com/FIX94/Nintendont> */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB game controllers with a Nintendont controller.ini layout, through
 * libogc's USB.  See input_wii.h and libinput_hid.h.
 *
 * Which pads are known and how their reports read come from Nintendont's
 * controllerconfigs, through RiftWii (github.com/KakarottoCake/Riftwii,
 * wii/controllers/hidpads.cpp and runtime/rtgcad.c, which read the pad's
 * interface and endpoints and send its rumble the way this does).
 * Only devices with a layout are ever opened, so the keyboard, mouse and
 * tablet scans are left alone.
 *
 * A worker thread looks for pads every second, opens one for each layout
 * it finds (up to four), keeps an interrupt read going on each and turns
 * its reports into a GameCube pad.
 */

#ifdef HAVE_HIDPADS

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>

#include "libinput_hid.h"
#include "libinput_internal.h"

#define MAX_PADS 4
#define MAX_DEVICES 32
#define MAX_USER 8
#define MAX_KNOWN 48
#define CFG_MAX 512
#define STACKSIZE 16384
#define LOOK_MS 1000      /* a missing pad is looked for this often */
#define GONE_MS 500       /* a removal notice is checked against the list this often */
#define FAILS_MAX 100     /* reads failing this many times in a row end the link */
#define NO_ENDPOINT 0

#define REQTYPE_CLASS_IF_OUT 0x21
#define HID_SET_REPORT 0x09
#define HID_OUTPUT_REPORT 0x02

static int (*log_print)(const char *fmt, ...);
#define LOG(...) do { if (log_print) log_print(__VA_ARGS__); } while (0)

void INPUT_HIDPadSetLog(int (*print)(const char *fmt, ...)) { log_print = print; }

typedef struct {
    volatile bool done, pending;
    volatile s32 res;
} xfer;

typedef struct {
    bool used;
    s32 fd;
    s32 dev_id;
    u16 vid, pid;
    u8 iface, ep_in, ep_out;
    u16 in_bytes;
    const char *name;
    hidpad_mapping map;
    volatile bool unplugged;
    int fails;
    u32 retry_at, gone_check;
    xfer in, out;
    u8 rumble_want, rumble_sent, rumble_flight, rumble_index;
} slot_t;

static slot_t slots[MAX_PADS];
static bool oh0;           /* devices open through /dev/usb/oh0, not by device id */
static bool oh0_known;

/* DMA buffers.  Static: a transfer that never completes may still land in
 * them after the pad is gone. */
static u8 in_buf[MAX_PADS][HIDPAD_REPORT_MAX] ATTRIBUTE_ALIGN(32);
static u8 out_buf[MAX_PADS][HIDPAD_RUMBLE_MAX] ATTRIBUTE_ALIGN(32);
static u8 ctl_buf[CFG_MAX] ATTRIBUTE_ALIGN(32);
static xfer ctl_x;

/* What the main thread reads, under `lock`. */
static mutex_t lock = LWP_MUTEX_NULL;
static hidpad_state published[MAX_PADS];
static bool present[MAX_PADS];
static char names[MAX_PADS][64];
static bool want_rumble[MAX_PADS];

static volatile bool running, exited;
static bool disabled;
static lwp_t thread = LWP_THREAD_NULL;
static u8 stack[STACKSIZE] ATTRIBUTE_ALIGN(32);

/* Layouts the app handed over, and ids with none (or none that can be
 * read), so the scan that repeats every second asks the SD card once. */
static hidpad_mapping user_maps[MAX_USER];
static int user_count;
static u32 unknown[MAX_KNOWN];
static int unknown_count;
static s32 excluded[MAX_DEVICES]; /* v5 list entries that are not the pad's input interface */
static int excluded_count;

static u32 now_ms(void) { return ticks_to_millisecs(gettime()); }

static s32 xfer_cb(s32 result, void *arg)
{
    xfer *x = arg;

    x->res = result;
    x->done = true;
    return 0;
}

static s32 removed_cb(s32 result, void *arg)
{
    (void)result;
    ((slot_t *)arg)->unplugged = true;
    return 0;
}

/* ---- Finding a layout --------------------------------------------------- */

bool INPUT_HIDPadAddConfig(const char *text, size_t length)
{
    hidpad_mapping m;
    char error[96];

    if (!text || user_count >= MAX_USER)
        return false;
    if (!hidpad_parse(text, length, &m, error, sizeof(error))) {
        LOG("USB pad: layout refused: %s\n", error);
        return false;
    }
    user_maps[user_count] = m;
    user_count++;
    /* Pads already turned away may have a layout now. */
    unknown_count = 0;
    return true;
}

static bool from_file(const char *path, u32 id, hidpad_mapping *out)
{
    static char text[HIDPAD_CONFIG_BYTES + 1];
    char error[96];
    FILE *file = fopen(path, "rb");
    size_t size;
    bool bad;

    if (!file)
        return false;
    size = fread(text, 1, sizeof(text), file);
    bad = ferror(file) != 0;
    fclose(file);
    if (bad || !hidpad_parse(text, size, out, error, sizeof(error))) {
        LOG("USB pad %04x:%04x: cannot use %s: %s\n", (unsigned)(id >> 16), (unsigned)(id & 0xFFFF), path,
            bad ? "the file could not be read" : error);
        return false;
    }
    /* sd:/controller.ini is one pad's. */
    return (((u32)out->vid << 16) | out->pid) == id;
}

static bool is_unknown(u32 id)
{
    for (int i = 0; i < unknown_count; i++)
        if (unknown[i] == id)
            return true;
    return false;
}

static void remember_unknown(u32 id)
{
    if (unknown_count < MAX_KNOWN)
        unknown[unknown_count++] = id;
}

/* A user layout first, then the files on the SD card where Nintendont
 * keeps them, then the library's own. */
static bool find_layout(u32 id, hidpad_mapping *out, const char **name)
{
    const hidpad_builtin *builtin;
    char path[64];
    unsigned vid = id >> 16, pid = id & 0xFFFF;

    for (int i = 0; i < user_count; i++) {
        if ((((u32)user_maps[i].vid << 16) | user_maps[i].pid) == id) {
            *out = user_maps[i];
            *name = "the app's layout";
            return true;
        }
    }
    snprintf(path, sizeof(path), "sd:/controllers/%04X_%04X.ini", vid, pid);
    if (from_file(path, id, out)) {
        *name = "a file on the SD card";
        return true;
    }
    snprintf(path, sizeof(path), "sd:/controllers/%04x_%04x.ini", vid, pid);
    if (from_file(path, id, out)) {
        *name = "a file on the SD card";
        return true;
    }
    if (from_file("sd:/controller.ini", id, out)) {
        *name = "sd:/controller.ini";
        return true;
    }
    builtin = hidpad_find_builtin(id);
    if (builtin) {
        char error[96];

        if (!hidpad_parse(builtin->text, strlen(builtin->text), out, error, sizeof(error))) {
            LOG("USB pad %04x:%04x: the built-in layout (%s) cannot be used: %s\n", vid, pid, builtin->file,
                error);
            return false;
        }
        *name = builtin->name;
        return true;
    }
    return false;
}

/* ---- Talking to the device ---------------------------------------------- */

static bool wait_for(xfer *x, int ms, volatile bool *gone)
{
    for (int waited = 0; !x->done; waited++) {
        if ((gone && *gone) || waited >= ms)
            return false;
        usleep(1000);
    }
    x->pending = false;
    return true;
}

/* A control transfer with a deadline (libogc's own have none), staged
 * through ctl_buf. */
static s32 ctrl(s32 fd, u8 type, u8 req, u16 val, u16 idx, u16 len, void *data, int ms)
{
    s32 ret;

    if (len > sizeof(ctl_buf) || ctl_x.pending)
        return -1;
    if (!(type & 0x80) && len)
        memcpy(ctl_buf, data, len);
    else
        memset(ctl_buf, 0, len ? len : 1);
    DCFlushRange(ctl_buf, (len + 31) & ~31u);

    ctl_x.done = false;
    ctl_x.res = -1;
    ctl_x.pending = true;
    if (type & 0x80)
        ret = USB_ReadCtrlMsgAsync(fd, type, req, val, idx, len, ctl_buf, xfer_cb, &ctl_x);
    else
        ret = USB_WriteCtrlMsgAsync(fd, type, req, val, idx, len, len ? ctl_buf : NULL, xfer_cb, &ctl_x);
    if (ret < 0) {
        ctl_x.pending = false;
        return ret;
    }
    if (!wait_for(&ctl_x, ms, NULL))
        return -6; /* stays pending: ctl_buf outlives it */
    if ((type & 0x80) && len) {
        DCInvalidateRange(ctl_buf, (len + 31) & ~31u);
        memcpy(data, ctl_buf, len);
    }
    return ctl_x.res;
}

typedef struct {
    u8 number, subclass;
    u8 ep_in, ep_out;
    u16 packet;
} iface_t;

/* The HID interfaces of a configuration descriptor with their interrupt
 * endpoints. */
static int hid_interfaces(const u8 *cfg, u32 total, iface_t *out, int max)
{
    int n = 0;
    bool in_hid = false;
    u32 off = 0;

    while (off + 2 <= total) {
        const u8 *d = cfg + off;
        u8 len = d[0], type = d[1];

        if (len < 2 || off + len > total)
            break;
        if (type == USB_DT_INTERFACE && len >= 9) {
            in_hid = d[5] == USB_CLASS_HID && d[3] == 0 && n < max;
            if (in_hid) {
                memset(&out[n], 0, sizeof(out[n]));
                out[n].number = d[2];
                out[n].subclass = d[6];
                n++;
            }
        } else if (in_hid && type == USB_DT_ENDPOINT && len >= 7 && (d[3] & 0x03) == 0x03) {
            if ((d[2] & 0x80) && !out[n - 1].ep_in) {
                out[n - 1].ep_in = d[2];
                out[n - 1].packet = (u16)((d[4] | (d[5] << 8)) & 0x7FF);
            } else if (!(d[2] & 0x80) && !out[n - 1].ep_out) {
                out[n - 1].ep_out = d[2];
            }
        }
        off += len;
    }
    return n;
}

/* The interface a USBV5 list entry stands for, kept in its token. */
static u8 entry_iface(const usb_device_entry *e) { return (u8)(e->token >> 8); }

static bool read_interfaces(s32 fd, iface_t *ifaces, int *n)
{
    static u8 cfg[CFG_MAX];
    u16 total;
    s32 ret;

    ret = ctrl(fd, 0x80, USB_REQ_GETDESCRIPTOR, USB_DT_CONFIG << 8, 0, 9, cfg, 300);
    if (ret < 9)
        return false;
    total = (u16)(cfg[2] | (cfg[3] << 8));
    if (total > CFG_MAX)
        total = CFG_MAX;
    ret = ctrl(fd, 0x80, USB_REQ_GETDESCRIPTOR, USB_DT_CONFIG << 8, 0, total, cfg, 500);
    if (ret < 9)
        return false;
    if ((u32)ret < total)
        total = (u16)ret;
    *n = hid_interfaces(cfg, total, ifaces, 4);
    return true;
}

static void publish(int port, const hidpad_state *s, bool on, const char *name)
{
    LWP_MutexLock(lock);
    present[port] = on;
    if (s)
        published[port] = *s;
    else
        memset(&published[port], 0, sizeof(published[port]));
    if (name) {
        strncpy(names[port], name, sizeof(names[port]) - 1);
        names[port][sizeof(names[port]) - 1] = 0;
    }
    LWP_MutexUnlock(lock);
}

static void close_slot(int port)
{
    slot_t *s = &slots[port];

    if (!s->used)
        return;
    /* A read stays outstanding while the pad is idle.  Under oh0 closing
     * is a real IOS_Close, which waits for it, so give it a moment and
     * otherwise leave the handle to the IOS reload that ends the app.
     * USBV5's close never reaches IOS. */
    if (oh0 && ((s->in.pending && !wait_for(&s->in, 50, NULL)) || (s->out.pending && !wait_for(&s->out, 50, NULL))))
        LOG("USB pad %04x:%04x: left open, a transfer is outstanding\n", s->vid, s->pid);
    else
        USB_CloseDevice(&s->fd);
    LOG("USB pad %04x:%04x: unplugged\n", s->vid, s->pid);
    publish(port, NULL, false, NULL);
    LWP_MutexLock(lock);
    want_rumble[port] = false;
    names[port][0] = 0;
    LWP_MutexUnlock(lock);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

static bool claimed(const usb_device_entry *e)
{
    for (int i = 0; i < MAX_PADS; i++) {
        if (!slots[i].used)
            continue;
        if (oh0 ? (slots[i].vid == e->vid && slots[i].pid == e->pid) : slots[i].dev_id == e->device_id)
            return true;
    }
    return false;
}

static bool is_excluded(s32 id)
{
    for (int i = 0; i < excluded_count; i++)
        if (excluded[i] == id)
            return true;
    return false;
}

static void exclude(s32 id)
{
    if (excluded_count < MAX_DEVICES)
        excluded[excluded_count++] = id;
}

static void open_pad(const usb_device_entry *e, const hidpad_mapping *map, const char *where)
{
    iface_t ifaces[4];
    int n = 0, pick = -1, port = -1;
    s32 fd = -1;
    slot_t *s;
    u32 id = ((u32)e->vid << 16) | e->pid;

    for (int i = 0; i < MAX_PADS; i++)
        if (!slots[i].used) {
            port = i;
            break;
        }
    if (port < 0)
        return;

    /* Said first: USBV5 reads an HID device's descriptors as it opens it,
     * with no deadline, so a stall here shows in the log. */
    LOG("USB pad %04x:%04x: opening id %08x (%s)\n", e->vid, e->pid, (unsigned)e->device_id,
        oh0 ? "oh0" : "usbv5");
    if (USB_OpenDevice(oh0 ? USB_OH0_DEVICE_ID : e->device_id, e->vid, e->pid, &fd) < 0)
        return; /* tried again by the next scan */

    ctl_x.pending = false; /* one that timed out earlier is no longer ours to wait for */

    if (!read_interfaces(fd, ifaces, &n)) {
        LOG("USB pad %04x:%04x: its descriptors could not be read\n", e->vid, e->pid);
        USB_CloseDevice(&fd);
        return;
    }
    for (int i = 0; i < n && pick < 0; i++) {
        if (!oh0 && ifaces[i].number != entry_iface(e))
            continue;
        if (ifaces[i].ep_in)
            pick = i;
    }
    if (pick < 0) {
        /* USBV5 lists a device once per interface: the others are not this pad's input. */
        if (!oh0)
            exclude(e->device_id);
        else
            remember_unknown(id);
        LOG("USB pad %04x:%04x: entry %08x has no HID input endpoint, left alone\n", e->vid, e->pid,
            (unsigned)e->device_id);
        USB_CloseDevice(&fd);
        return;
    }
    if (ifaces[pick].packet < map->report_bytes || ifaces[pick].packet > HIDPAD_REPORT_MAX) {
        LOG("USB pad %04x:%04x: its reports are %u bytes, the layout needs %u: left alone\n", e->vid, e->pid,
            ifaces[pick].packet, map->report_bytes);
        remember_unknown(id);
        USB_CloseDevice(&fd);
        return;
    }
    /* A boot interface may still be in boot protocol: ask for reports. */
    if (ifaces[pick].subclass == 1)
        (void)ctrl(fd, REQTYPE_CLASS_IF_OUT, USB_REQ_SETPROTOCOL, 1, ifaces[pick].number, 0, NULL, 100);

    s = &slots[port];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->fd = fd;
    s->dev_id = e->device_id;
    s->vid = e->vid;
    s->pid = e->pid;
    s->iface = ifaces[pick].number;
    s->ep_in = ifaces[pick].ep_in;
    s->ep_out = ifaces[pick].ep_out;
    s->in_bytes = ifaces[pick].packet;
    s->map = *map;
    s->rumble_sent = 0xFF; /* unknown: the first poll settles it */
    USB_DeviceRemovalNotifyAsync(fd, removed_cb, s);
    s->unplugged = false;
    LOG("USB pad %04x:%04x: port %d, interface %u, in %02x out %02x, %u byte reports (%s)\n", e->vid, e->pid,
        port + 1, s->iface, s->ep_in, s->ep_out, s->in_bytes, where);
    publish(port, NULL, true, where);
}

static void scan(void)
{
    static usb_device_entry list[MAX_DEVICES] ATTRIBUTE_ALIGN(32);
    u8 count = 0;

    if (!oh0_known) {
        s32 fd = IOS_Open("/dev/usb/oh0", 0);

        oh0 = fd >= 0;
        if (oh0)
            IOS_Close(fd);
        oh0_known = true;
    }
    memset(list, 0, sizeof(list));
    if (USB_GetDeviceList(list, MAX_DEVICES, USB_CLASS_HID, &count) < 0)
        return;

    /* Entries that left the list can be asked about again. */
    for (int i = 0; i < excluded_count;) {
        bool there = false;

        for (int j = 0; j < count; j++)
            there = there || list[j].device_id == excluded[i];
        if (there)
            i++;
        else
            excluded[i] = excluded[--excluded_count];
    }

    for (int i = 0; i < count; i++) {
        u32 id = ((u32)list[i].vid << 16) | list[i].pid;
        hidpad_mapping map;
        const char *where;

        if ((list[i].vid == 0 && list[i].pid == 0) || id == 0x057E0337u /* the GameCube adapter has its own driver */ ||
            claimed(&list[i]) || is_excluded(list[i].device_id) || is_unknown(id))
            continue;
        if (!find_layout(id, &map, &where)) {
            remember_unknown(id);
            continue;
        }
        if (!map.polltype) {
            LOG("USB pad %04x:%04x: %s polls with GET_REPORT (a PS3 pad), which is not read here\n", list[i].vid,
                list[i].pid, where);
            remember_unknown(id);
            continue;
        }
        open_pad(&list[i], &map, where);
    }
}

/* Whether a pad's entry is still listed: one that is was not unplugged,
 * whatever the removal hook said (libogc's USBV5 close calls every
 * removal hook on a device, and the keyboard driver opens and closes
 * every HID device it rescans). */
static bool still_listed(const slot_t *s)
{
    static usb_device_entry list[MAX_DEVICES] ATTRIBUTE_ALIGN(32);
    u8 count = 0;

    if (oh0)
        return false;
    memset(list, 0, sizeof(list));
    if (USB_GetDeviceList(list, MAX_DEVICES, USB_CLASS_HID, &count) < 0)
        return false;
    for (int i = 0; i < count; i++)
        if (list[i].device_id == s->dev_id)
            return true;
    return false;
}

/* ---- Reports and rumble ------------------------------------------------- */

static void submit_in(int port)
{
    slot_t *s = &slots[port];
    s32 ret;

    memset(in_buf[port], 0, HIDPAD_REPORT_MAX);
    DCFlushRange(in_buf[port], HIDPAD_REPORT_MAX);
    s->in.done = false;
    s->in.pending = true;
    ret = USB_ReadIntrMsgAsync(s->fd, s->ep_in, s->in_bytes, in_buf[port], xfer_cb, &s->in);
    if (ret < 0) {
        s->in.pending = false;
        s->fails++;
        s->retry_at = now_ms() + (s->fails > 20 ? 100 : 5);
    }
}

static void collect_in(int port)
{
    slot_t *s = &slots[port];
    s32 ret = s->in.res;
    hidpad_state state;
    u32 bytes;

    s->in.pending = false;
    if (ret < 0) {
        s->fails++;
        s->retry_at = now_ms() + (s->fails > 20 ? 100 : 5);
        return;
    }
    s->fails = 0;
    DCInvalidateRange(in_buf[port], HIDPAD_REPORT_MAX);
    /* A zero length is a full report on some IOSes. */
    bytes = ret == 0 ? s->in_bytes : (u32)ret;
    if (bytes > s->in_bytes)
        bytes = s->in_bytes;
    /* A report another one of its pads' kinds, or too short, is skipped. */
    if (!hidpad_decode(&s->map, in_buf[port], bytes, &state))
        return;
    if (s->ep_out == NO_ENDPOINT && s->map.rumble_type)
        state.no_rumble = 1;
    publish(port, &state, true, NULL);
}

/* One of the configured rumble transfers: interrupt OUT, or SET_REPORT. */
static void send_rumble(int port)
{
    slot_t *s = &slots[port];
    const hidpad_mapping *m = &s->map;
    const u8 *data = s->rumble_flight ? m->rumble_on : m->rumble_off;
    /* Interrupt rumble advances through the payload; control rumble repeats the first block. */
    u32 offset = m->rumble_type ? (u32)s->rumble_index * m->rumble_transfer : 0;
    s32 ret;

    memcpy(out_buf[port], data + offset, m->rumble_transfer);
    DCFlushRange(out_buf[port], (m->rumble_transfer + 31) & ~31u);
    s->out.done = false;
    s->out.pending = true;
    if (m->rumble_type)
        ret = USB_WriteIntrMsgAsync(s->fd, s->ep_out, m->rumble_transfer, out_buf[port], xfer_cb, &s->out);
    else
        ret = USB_WriteCtrlMsgAsync(s->fd, REQTYPE_CLASS_IF_OUT, HID_SET_REPORT, (HID_OUTPUT_REPORT << 8) | 1,
                                    s->iface, m->rumble_transfer, out_buf[port], xfer_cb, &s->out);
    if (ret < 0) {
        s->out.pending = false;
        s->rumble_sent = s->rumble_flight; /* not retried every pass */
    }
}

static void collect_out(int port)
{
    slot_t *s = &slots[port];

    s->out.pending = false;
    if (s->out.res >= 0 && ++s->rumble_index < s->map.rumble_count) {
        send_rumble(port);
        return;
    }
    s->rumble_sent = s->rumble_flight;
}

/* Between polls, never alongside another transfer. */
static void pump_rumble(int port)
{
    slot_t *s = &slots[port];
    bool want;

    if (!s->map.rumble_bytes || (s->map.rumble_type && s->ep_out == NO_ENDPOINT))
        return;
    LWP_MutexLock(lock);
    want = want_rumble[port];
    LWP_MutexUnlock(lock);
    if ((u8)want == s->rumble_sent)
        return;
    s->rumble_flight = want;
    s->rumble_index = 0;
    send_rumble(port);
}

static void service(int port)
{
    slot_t *s = &slots[port];

    if (!s->used)
        return;
    if (s->unplugged) {
        u32 now = now_ms();

        if (now - s->gone_check < GONE_MS)
            return;
        s->gone_check = now;
        if (!still_listed(s)) {
            close_slot(port);
            return;
        }
        s->unplugged = false;
        USB_DeviceRemovalNotifyAsync(s->fd, removed_cb, s);
    }
    if (s->in.pending && s->in.done)
        collect_in(port);
    if (s->out.pending && s->out.done)
        collect_out(port);
    if (s->fails >= FAILS_MAX && !s->in.pending && !s->out.pending) {
        close_slot(port);
        return;
    }
    if (s->in.pending || s->out.pending)
        return;
    if (now_ms() < s->retry_at && s->fails)
        return;
    pump_rumble(port);
    if (!s->out.pending)
        submit_in(port);
}

static void *worker(void *arg)
{
    u32 last = 0;
    bool first = true;

    (void)arg;
    while (running) {
        bool active = false;
        u32 now = now_ms();

        if (first || now - last >= LOOK_MS) {
            first = false;
            last = now;
            scan();
        }
        for (int i = 0; i < MAX_PADS; i++) {
            service(i);
            active = active || slots[i].used;
        }
        usleep(active ? 1000 : 10000);
    }
    for (int i = 0; i < MAX_PADS; i++)
        close_slot(i);
    exited = true;
    return NULL;
}

/* ---- API ------------------------------------------------------------------ */

void inpw_hidpads_init(bool enabled)
{
    disabled = !enabled;
    if (disabled || running)
        return;
    if (lock == LWP_MUTEX_NULL)
        LWP_MutexInit(&lock, false);
    for (int i = 0; i < MAX_PADS; i++) {
        memset(&slots[i], 0, sizeof(slots[i]));
        slots[i].fd = -1;
        present[i] = false;
        want_rumble[i] = false;
        names[i][0] = 0;
    }
    unknown_count = 0;
    excluded_count = 0;
    oh0_known = false;
    USB_Initialize();
    running = true;
    exited = false;
    if (LWP_CreateThread(&thread, worker, NULL, stack, STACKSIZE, 70) < 0) {
        running = false;
        thread = LWP_THREAD_NULL;
    }
}

void inpw_hidpads_shutdown(void)
{
    if (!running)
        return;
    running = false;
    if (thread == LWP_THREAD_NULL)
        return;
    /* The worker can be inside a libogc call with no deadline (opening a
     * USBV5 HID device reads its descriptors synchronously), and joining it
     * would then hang the caller: half a second, then it is left to the
     * IOS reload that normally follows. */
    for (int ms = 0; ms < 500 && !exited; ms += 10)
        usleep(10 * 1000);
    if (exited)
        LWP_JoinThread(thread, NULL);
    else
        LOG("USB pad: the thread did not stop\n");
    thread = LWP_THREAD_NULL;
}

bool inpw_hidpads_claims(u16 vid, u16 pid)
{
    u32 id = ((u32)vid << 16) | pid;

    if (!running)
        return false;
    for (int i = 0; i < MAX_PADS; i++)
        if (slots[i].used && slots[i].vid == vid && slots[i].pid == pid)
            return true;
    for (int i = 0; i < user_count; i++)
        if ((((u32)user_maps[i].vid << 16) | user_maps[i].pid) == id)
            return true;
    return hidpad_find_builtin(id) != NULL;
}

bool INPUT_HIDPadConnected(void)
{
    bool any = false;

    if (!running)
        return false;
    LWP_MutexLock(lock);
    for (int i = 0; i < MAX_PADS; i++)
        any = any || present[i];
    LWP_MutexUnlock(lock);
    return any;
}

bool INPUT_HIDPadPad(int port, input_gc_pad *out)
{
    hidpad_state s;
    bool on;

    if (!running || port < 0 || port >= MAX_PADS)
        return false;
    LWP_MutexLock(lock);
    on = present[port];
    s = published[port];
    LWP_MutexUnlock(lock);
    if (!on)
        return false;
    if (out) {
        out->buttons = s.buttons;
        out->stick_x = s.stick_x;
        out->stick_y = s.stick_y;
        out->substick_x = s.substick_x;
        out->substick_y = s.substick_y;
        out->trigger_l = s.trigger_l;
        out->trigger_r = s.trigger_r;
        out->wireless = s.no_rumble != 0;
    }
    return true;
}

const char *INPUT_HIDPadName(int port)
{
    static char copy[MAX_PADS][64];

    if (!running || port < 0 || port >= MAX_PADS)
        return NULL;
    LWP_MutexLock(lock);
    memcpy(copy[port], names[port], sizeof(copy[port]));
    LWP_MutexUnlock(lock);
    return copy[port][0] ? copy[port] : NULL;
}

void INPUT_HIDPadRumble(int port, bool on)
{
    if (!running || port < 0 || port >= MAX_PADS)
        return;
    LWP_MutexLock(lock);
    want_rumble[port] = on;
    LWP_MutexUnlock(lock);
}

#endif /* HAVE_HIDPADS */
