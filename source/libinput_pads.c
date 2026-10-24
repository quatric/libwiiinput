#include "libinput_internal.h"
#include <math.h>
#include <string.h>
#include <ogc/conf.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>
#ifdef HAVE_WIIDRC
#include <wiidrc/wiidrc.h>
#endif
#ifdef HAVE_WUPC
#include <wupc/wupc.h>
#endif
#ifdef HAVE_SICKSAXIS
#include <sicksaxis.h>
#endif

#define STICK_ACTIVE 0.18f

typedef struct {
    float x, y, mag;
    float raw_x, raw_y; /* before the length is clamped to 1 */
    const char *src;
} stick_t;

static u32 pads_held, pads_down;
static u32 all_held, all_down, all_up, all_prev;
static stick_t sticks[2];
static bool pointer_valid;
static float pointer_x, pointer_y, gforce_max;

#ifdef HAVE_SICKSAXIS
static struct ss_device ds3;
static bool ds3_initialized;
static u32 ds3_previous;
/* Opening the DS3 walks the USB device list through IOS: retry about every two seconds, not every frame. */
#define DS3_RETRY_FRAMES 120
static int ds3_retry;
#endif

static void stick_feed(int which, const char *src, float x, float y)
{
    float raw_x = x, raw_y = y;
    float m = sqrtf(x * x + y * y);
    if (m > 1.0f) {
        x /= m;
        y /= m;
        m = 1.0f;
    }
    if (m > sticks[which].mag) {
        sticks[which].x = x;
        sticks[which].y = y;
        sticks[which].mag = m;
        sticks[which].raw_x = raw_x;
        sticks[which].raw_y = raw_y;
        sticks[which].src = src;
    }
}

static void joy_feed(int which, const char *src, const joystick_t *j)
{
    if (j->mag < 0.01f)
        return;
    float a = j->ang * 0.017453292f;
    stick_feed(which, src, sinf(a) * j->mag, cosf(a) * j->mag);
}

static u32 map_classic(u32 b)
{
    u32 out = 0;
    if (b & (WPAD_CLASSIC_BUTTON_ZR | WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_PLUS))
        out |= WPAD_BUTTON_PLUS;
    if (b & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_MINUS))
        out |= WPAD_BUTTON_MINUS;
    if (b & WPAD_CLASSIC_BUTTON_A)     out |= WPAD_BUTTON_A;
    if (b & WPAD_CLASSIC_BUTTON_B)     out |= WPAD_BUTTON_B;
    if (b & WPAD_CLASSIC_BUTTON_X)     out |= WPAD_BUTTON_2;
    if (b & WPAD_CLASSIC_BUTTON_Y)     out |= WPAD_BUTTON_1;
    if (b & WPAD_CLASSIC_BUTTON_HOME)  out |= WPAD_BUTTON_HOME;
    if (b & WPAD_CLASSIC_BUTTON_UP)    out |= WPAD_BUTTON_UP;
    if (b & WPAD_CLASSIC_BUTTON_DOWN)  out |= WPAD_BUTTON_DOWN;
    if (b & WPAD_CLASSIC_BUTTON_LEFT)  out |= WPAD_BUTTON_LEFT;
    if (b & WPAD_CLASSIC_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    return out;
}

/* Core Wii Remote buttons left to another layer, per channel. */
static u32 wiimote_ignored[4];

void INPUT_IgnoreWiimoteButtons(int chan, u32 buttons)
{
    if (chan >= 0 && chan < 4)
        wiimote_ignored[chan] = buttons & 0xFFFF;
}

/* GameCube pad bit -> WPAD_BUTTON_* (remappable). */
static u32 gc_map[16];
static bool gc_map_ready;

static void gc_map_set_bits(u32 pad_buttons, u32 button)
{
    for (int i = 0; i < 16; i++)
        if (pad_buttons & (1u << i))
            gc_map[i] = button;
}

static void gc_map_defaults(void)
{
    gc_map_ready = true;
    memset(gc_map, 0, sizeof(gc_map));
    gc_map_set_bits(PAD_TRIGGER_R | PAD_TRIGGER_Z, WPAD_BUTTON_PLUS);
    gc_map_set_bits(PAD_TRIGGER_L, WPAD_BUTTON_MINUS);
    gc_map_set_bits(PAD_BUTTON_A, WPAD_BUTTON_A);
    gc_map_set_bits(PAD_BUTTON_B, WPAD_BUTTON_B);
    gc_map_set_bits(PAD_BUTTON_X, WPAD_BUTTON_2);
    gc_map_set_bits(PAD_BUTTON_Y, WPAD_BUTTON_1);
    gc_map_set_bits(PAD_BUTTON_MENU, WPAD_BUTTON_HOME);
    gc_map_set_bits(PAD_BUTTON_UP, WPAD_BUTTON_UP);
    gc_map_set_bits(PAD_BUTTON_DOWN, WPAD_BUTTON_DOWN);
    gc_map_set_bits(PAD_BUTTON_LEFT, WPAD_BUTTON_LEFT);
    gc_map_set_bits(PAD_BUTTON_RIGHT, WPAD_BUTTON_RIGHT);
}

void INPUT_GameCubeMapClear(void)
{
    gc_map_ready = true;
    memset(gc_map, 0, sizeof(gc_map));
}

void INPUT_GameCubeMapSet(u16 pad_buttons, u32 button)
{
    if (!gc_map_ready)
        gc_map_defaults();
    gc_map_set_bits(pad_buttons, button);
}

static u32 map_gc(u32 b)
{
    u32 out = 0;

    if (!gc_map_ready)
        gc_map_defaults();
    for (int i = 0; i < 16; i++)
        if (b & (1u << i))
            out |= gc_map[i];
    return out;
}

#ifdef HAVE_WIIDRC
static u32 map_drc(u32 b)
{
    u32 out = 0;
    if (b & (WIIDRC_BUTTON_R | WIIDRC_BUTTON_ZR | WIIDRC_BUTTON_PLUS))  out |= WPAD_BUTTON_PLUS;
    if (b & (WIIDRC_BUTTON_L | WIIDRC_BUTTON_ZL | WIIDRC_BUTTON_MINUS)) out |= WPAD_BUTTON_MINUS;
    if (b & WIIDRC_BUTTON_A)     out |= WPAD_BUTTON_A;
    if (b & WIIDRC_BUTTON_B)     out |= WPAD_BUTTON_B;
    if (b & WIIDRC_BUTTON_X)     out |= WPAD_BUTTON_2;
    if (b & WIIDRC_BUTTON_Y)     out |= WPAD_BUTTON_1;
    if (b & WIIDRC_BUTTON_HOME)  out |= WPAD_BUTTON_HOME;
    if (b & WIIDRC_BUTTON_UP)    out |= WPAD_BUTTON_UP;
    if (b & WIIDRC_BUTTON_DOWN)  out |= WPAD_BUTTON_DOWN;
    if (b & WIIDRC_BUTTON_LEFT)  out |= WPAD_BUTTON_LEFT;
    if (b & WIIDRC_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    return out;
}
#endif

/* Fold one device's held/down masks into the totals and track recency. */
static void feed(const char *src, u32 held, u32 down)
{
    pads_held |= held;
    pads_down |= down;
    if (down)
        inpw_set_source(src);
}

#ifdef HAVE_SICKSAXIS
static void poll_ds3(void)
{
    if (!ds3_initialized)
        return;
    if (!ss_is_connected(&ds3)) {
        ds3_previous = 0;
        if (++ds3_retry < DS3_RETRY_FRAMES)
            return;
        ds3_retry = 0;
        if (ss_open(&ds3) > 0)
            ss_start_reading(&ds3);
        else
            return;
    }

    const struct SS_BUTTONS *b = &ds3.pad.buttons;
    u32 now = 0;
    if (b->start || b->R1 || b->R2)  now |= WPAD_BUTTON_PLUS;
    if (b->select || b->L1 || b->L2) now |= WPAD_BUTTON_MINUS;
    if (b->cross)    now |= WPAD_BUTTON_A;
    if (b->circle)   now |= WPAD_BUTTON_B;
    if (b->triangle) now |= WPAD_BUTTON_2;
    if (b->square)   now |= WPAD_BUTTON_1;
    if (b->PS)       now |= WPAD_BUTTON_HOME;
    if (b->up)       now |= WPAD_BUTTON_UP;
    if (b->down)     now |= WPAD_BUTTON_DOWN;
    if (b->left)     now |= WPAD_BUTTON_LEFT;
    if (b->right)    now |= WPAD_BUTTON_RIGHT;

    feed("DualShock 3", now, now & ~ds3_previous);
    ds3_previous = now;

    /* Analog values are 0..255 with 128 centred and y growing downward. */
    stick_feed(INPUT_STICK_LEFT, "DualShock 3",
               (ds3.pad.left_analog.x - 128) / 110.0f,
               -(ds3.pad.left_analog.y - 128) / 110.0f);
    stick_feed(INPUT_STICK_RIGHT, "DualShock 3",
               (ds3.pad.right_analog.x - 128) / 110.0f,
               -(ds3.pad.right_analog.y - 128) / 110.0f);
}
#endif

#ifdef HAVE_GCADAPTER
/* The adapter's ports read like the console's own GameCube ports, through
 * the same mapping; the driver gives the sticks relative to their origin. */
static u16 gcad_previous[4];

static void poll_gcadapter(void)
{
    inpw_gcadapter_update();
    for (int port = 0; port < 4; port++) {
        input_gc_pad p;
        if (!INPUT_GCAdapterPad(port, &p)) {
            gcad_previous[port] = 0;
            continue;
        }
        feed("GameCube", map_gc(p.buttons), map_gc(p.buttons & ~gcad_previous[port]));
        gcad_previous[port] = p.buttons;
        stick_feed(INPUT_STICK_LEFT, "GameCube", p.stick_x / 70.0f, p.stick_y / 70.0f);
        stick_feed(INPUT_STICK_RIGHT, "GameCube", p.substick_x / 60.0f, p.substick_y / 60.0f);
    }
}
#endif

/* libogc's wiiuse maps the camera's 1024x768 view to the screen through a box sized for a 4:3
 * screen (560x420) unless told otherwise, and libogc never tells it the console's 16:9 setting.
 * On a widescreen TV the screen spans more of the camera's view, so the pointer ran ahead of the
 * remote, further toward the edges. With the console set to 16:9, the pointer is taken from the
 * smoothed position through wiiuse's own 16:9 box (660x370) and its sensor bar offset (the box's
 * half height less 70, up for a bar above the screen, down for one below). */
static bool widescreen;

static bool point_16_9(const ir_t *ir, float *x, float *y)
{
    const float xs = 660.0f, ys = 370.0f;
    const float offset_y = ir->pos == WIIUSE_IR_ABOVE ? ys / 2 - 70 : -ys / 2 + 70;
    const float x0 = (1024 - xs) / 2, y0 = (768 - ys) / 2 + offset_y;
    if (!ir->smooth_valid || ir->sx < x0 || ir->sx > x0 + xs || ir->sy < y0 || ir->sy > y0 + ys)
        return false;
    *x = (ir->sx - x0) / xs * inpw_screen_w;
    *y = (ir->sy - y0) / ys * inpw_screen_h;
    return true;
}

s32 inpw_pads_init(u32 flags)
{
    PAD_Init();
#ifdef HAVE_WUPC
    WUPC_Init();
#endif
#ifdef HAVE_WPADQUEUE
    inpw_wpadqueue_link();
#endif
    WPAD_Init();
    /* The four Wii Remote channels only: the Balance Board's is never read. */
    for (int ch = WPAD_CHAN_0; ch <= WPAD_CHAN_3; ch++) {
        WPAD_SetDataFormat(ch, (flags & INPUT_INIT_NO_POINTER) ? WPAD_FMT_BTNS : WPAD_FMT_BTNS_ACC_IR);
        WPAD_SetVRes(ch, inpw_screen_w, inpw_screen_h);
    }
    widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
#ifdef HAVE_WIIDRC
    WiiDRC_Init();
#endif
#ifdef HAVE_SICKSAXIS
    if (ss_init() >= 0 && ss_initialize(&ds3) >= 0) {
        ds3_initialized = true;
        if (ss_open(&ds3) > 0)
            ss_start_reading(&ds3);
    }
#endif
#ifdef HAVE_GCADAPTER
    inpw_gcadapter_init(!(flags & INPUT_INIT_NO_GCADAPTER));
#endif
    return 0;
}

void inpw_pads_shutdown(void)
{
#ifdef HAVE_GCADAPTER
    inpw_gcadapter_shutdown();
#endif
#ifdef HAVE_SICKSAXIS
    if (ds3_initialized) {
        if (ss_is_connected(&ds3))
            ss_stop_reading(&ds3);
        ss_close(&ds3);
        ds3_initialized = false;
    }
#endif
#ifdef HAVE_WUPC
    WUPC_Shutdown();
#endif
    WPAD_Shutdown();
}

void inpw_pads_set_screen(int w, int h)
{
    for (int ch = WPAD_CHAN_0; ch <= WPAD_CHAN_3; ch++)
        WPAD_SetVRes(ch, w, h);
}

void inpw_pads_update(void)
{
    pads_held = pads_down = 0;
    sticks[0] = sticks[1] = (stick_t){ 0, 0, 0, 0, 0, "" };
    pointer_valid = false;
    gforce_max = 0;

#ifdef HAVE_WUPC
    WUPC_UpdateButtonStats();
    for (int ch = 0; ch < 4; ch++) {
        feed("Wii U Pro", map_classic(WUPC_ButtonsHeld(ch)), map_classic(WUPC_ButtonsDown(ch)));
        if (WUPC_Data(ch)) {
            stick_feed(INPUT_STICK_LEFT, "Wii U Pro", WUPC_lStickX(ch) / 900.0f, WUPC_lStickY(ch) / 900.0f);
            stick_feed(INPUT_STICK_RIGHT, "Wii U Pro", WUPC_rStickX(ch) / 900.0f, WUPC_rStickY(ch) / 900.0f);
        }
    }
#endif

    WPAD_ScanPads();
    for (int ch = 0; ch < 4; ch++) {
        u32 type;
        if (WPAD_Probe(ch, &type) != WPAD_ERR_NONE)
            continue;
        WPADData *wd = WPAD_Data(ch);
        if (!wd || wd->err != WPAD_ERR_NONE)
            continue;

        u32 h = WPAD_ButtonsHeld(ch), d = WPAD_ButtonsDown(ch);
        u32 core_h = h & 0xFFFF & ~wiimote_ignored[ch];
        u32 core_d = d & 0xFFFF & ~wiimote_ignored[ch];
        u32 ext_h = h & 0xFFFF0000u, ext_d = d & 0xFFFF0000u;
        u32 xh = 0, xd = 0;
        const char *xsrc = "Wiimote";

        /* The upper 16 bits mean different things per expansion; reading
         * them without checking the type would mistake Nunchuk Z for
         * Classic Up. */
        if (wd->exp.type == WPAD_EXP_CLASSIC) {
            xh = map_classic(ext_h);
            xd = map_classic(ext_d);
            xsrc = "Classic";
            joy_feed(INPUT_STICK_LEFT, "Classic", &wd->exp.classic.ljs);
            joy_feed(INPUT_STICK_RIGHT, "Classic", &wd->exp.classic.rjs);
        } else if (wd->exp.type == WPAD_EXP_NUNCHUK) {
            if (ext_h & WPAD_NUNCHUK_BUTTON_Z) xh |= INPUT_BUTTON_Z;
            if (ext_h & WPAD_NUNCHUK_BUTTON_C) xh |= INPUT_BUTTON_C;
            if (ext_d & WPAD_NUNCHUK_BUTTON_Z) xd |= INPUT_BUTTON_Z;
            if (ext_d & WPAD_NUNCHUK_BUTTON_C) xd |= INPUT_BUTTON_C;
            xsrc = "Nunchuk";
            joy_feed(INPUT_STICK_LEFT, "Nunchuk", &wd->exp.nunchuk.js);
        }

        feed("Wiimote", core_h, core_d);
        feed(xsrc, xh, xd);

        if (!pointer_valid) {
            float x, y;
            if (widescreen ? point_16_9(&wd->ir, &x, &y) : wd->ir.valid) {
                pointer_valid = true;
                pointer_x = widescreen ? x : wd->ir.x;
                pointer_y = widescreen ? y : wd->ir.y;
            }
        }

        float g = sqrtf(wd->gforce.x * wd->gforce.x + wd->gforce.y * wd->gforce.y +
                        wd->gforce.z * wd->gforce.z);
        if (g > gforce_max)
            gforce_max = g;
    }

#ifdef HAVE_WIIDRC
    if (WiiDRC_Inited() && WiiDRC_Connected() && WiiDRC_ScanPads()) {
        feed("GamePad", map_drc(WiiDRC_ButtonsHeld()), map_drc(WiiDRC_ButtonsDown()));
        stick_feed(INPUT_STICK_LEFT, "GamePad", WiiDRC_lStickX() / 900.0f, WiiDRC_lStickY() / 900.0f);
        stick_feed(INPUT_STICK_RIGHT, "GamePad", WiiDRC_rStickX() / 900.0f, WiiDRC_rStickY() / 900.0f);
    }
#endif

#ifdef HAVE_SICKSAXIS
    poll_ds3();
#endif

    PAD_ScanPads();
    for (int ch = 0; ch < 4; ch++) {
        feed("GameCube", map_gc(PAD_ButtonsHeld(ch)), map_gc(PAD_ButtonsDown(ch)));
        stick_feed(INPUT_STICK_LEFT, "GameCube", PAD_StickX(ch) / 70.0f, PAD_StickY(ch) / 70.0f);
        stick_feed(INPUT_STICK_RIGHT, "GameCube", PAD_SubStickX(ch) / 60.0f, PAD_SubStickY(ch) / 60.0f);
    }

#ifdef HAVE_GCADAPTER
    poll_gcadapter();
#endif

    for (int i = 0; i < 2; i++) {
        if (sticks[i].mag > STICK_ACTIVE)
            inpw_set_source(sticks[i].src);
    }
    if (pointer_valid)
        inpw_set_source("Wiimote");

    u32 kheld = INPUT_KeyboardPadHeld();
    u32 kdown = INPUT_KeyboardPadDown();
    all_held = pads_held | kheld;
    all_down = pads_down | kdown;
    all_up = all_prev & ~all_held;
    all_prev = all_held;
}

u32 INPUT_PadsHeld(void)    { return pads_held; }
u32 INPUT_PadsDown(void)    { return pads_down; }
u32 INPUT_ButtonsHeld(void) { return all_held; }
u32 INPUT_ButtonsDown(void) { return all_down; }
u32 INPUT_ButtonsUp(void)   { return all_up; }

float INPUT_Stick(int which, float *x, float *y, const char **src)
{
    if (which < 0 || which > 1)
        which = 0;
    if (x) *x = sticks[which].x;
    if (y) *y = sticks[which].y;
    if (src) *src = sticks[which].src ? sticks[which].src : "";
    return sticks[which].mag;
}

float INPUT_StickRaw(int which, float *x, float *y, const char **src)
{
    if (which < 0 || which > 1)
        which = 0;
    if (x) *x = sticks[which].raw_x;
    if (y) *y = sticks[which].raw_y;
    if (src) *src = sticks[which].src ? sticks[which].src : "";
    return sticks[which].mag;
}

bool INPUT_Pointer(float *x, float *y)
{
    if (x) *x = pointer_x;
    if (y) *y = pointer_y;
    return pointer_valid;
}

float INPUT_Gforce(void) { return gforce_max; }
