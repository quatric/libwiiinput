#include <string.h>
#include <unistd.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/usbkeyboard.h>
#include "libinput_internal.h"

#define KEY_BYTES (256 / 8)
#define QUEUE_LEN 64

#define BIT_GET(a, k) ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define BIT_SET(a, k) ((a)[(k) >> 3] |= (1 << ((k) & 7)))
#define BIT_CLR(a, k) ((a)[(k) >> 3] &= ~(1 << ((k) & 7)))

/* Written by the scan thread (via the event callback) under kbd_lock. */
static u8 live[KEY_BYTES];
static u8 pressed_acc[KEY_BYTES];
static u8 released_acc[KEY_BYTES];
static input_key_event queue[QUEUE_LEN];
static int q_head, q_tail;
static bool caps;

/* Snapshot published by inpw_keyboard_update(). */
static u8 held[KEY_BYTES];
static u8 pressed[KEY_BYTES];
static u8 released[KEY_BYTES];

static u32 key_map[256];

static mutex_t kbd_lock = LWP_MUTEX_NULL;
static lwp_t kbd_thread_hndl = LWP_THREAD_NULL;
static volatile bool kbd_thread_should_run;
static bool kbd_initialized;

static const char plain[]   = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

static u8 live_mods(void)
{
    u8 m = 0;
    if (BIT_GET(live, 0xE1) || BIT_GET(live, 0xE5)) m |= INPUT_MOD_SHIFT;
    if (BIT_GET(live, 0xE0) || BIT_GET(live, 0xE4)) m |= INPUT_MOD_CTRL;
    if (BIT_GET(live, 0xE2) || BIT_GET(live, 0xE6)) m |= INPUT_MOD_ALT;
    if (BIT_GET(live, 0xE3) || BIT_GET(live, 0xE7)) m |= INPUT_MOD_GUI;
    return m;
}

static char key_char(u8 code, bool shift)
{
    char c = 0;

    if (code >= 0x04 && code <= 0x38) {
        c = (shift ? shifted : plain)[code - 0x04];
        if (code <= 0x1D && caps)
            c = (c >= 'a' && c <= 'z') ? c - 32 : c + 32;
    } else if (code >= 0x59 && code <= 0x61) {
        c = '1' + (code - 0x59);
    } else {
        switch (code) {
        case 0x54: c = '/'; break;
        case 0x55: c = '*'; break;
        case 0x56: c = '-'; break;
        case 0x57: c = '+'; break;
        case 0x62: c = '0'; break;
        case 0x63: c = '.'; break;
        }
    }
    return (c >= 0x20 && c < 0x7F) ? c : 0;
}

/* Runs on the scan thread, inside USBKeyboard_Scan(). */
static void kb_event_locked(USBKeyboard_event event, bool *caps_changed)
{
    u8 k = event.keyCode;

    if (event.type == USBKEYBOARD_PRESSED) {
        bool fresh = !BIT_GET(live, k);
        BIT_SET(live, k);
        if (!fresh)
            return;
        BIT_SET(pressed_acc, k);
        inpw_set_source("Keyboard");

        if (k == 0x39) { /* Caps Lock */
            caps = !caps;
            *caps_changed = true;
            return;
        }
        if (k >= 0xE0 && k <= 0xE7)
            return;

        int next = (q_head + 1) % QUEUE_LEN;
        if (next != q_tail) {
            u8 mods = live_mods();
            queue[q_head].code = k;
            queue[q_head].mods = mods;
            queue[q_head].ch = (mods & INPUT_MOD_CTRL) ? 0 : key_char(k, mods & INPUT_MOD_SHIFT);
            q_head = next;
        }
    } else if (event.type == USBKEYBOARD_RELEASED) {
        if (BIT_GET(live, k))
            BIT_SET(released_acc, k);
        BIT_CLR(live, k);
    }
}

/* Every held key is released when the keyboard goes away. */
static void release_all_locked(void)
{
    for (int i = 0; i < 256; i++) {
        if (BIT_GET(live, i))
            BIT_SET(released_acc, i);
    }
    memset(live, 0, sizeof(live));
}

static volatile bool kbd_lost;

static void kb_event(USBKeyboard_event event)
{
    bool caps_changed = false;

    /* The disconnect event comes from an IPC callback, where a mutex cannot
     * be taken; the scan thread releases the keys. */
    if (event.type == USBKEYBOARD_DISCONNECTED) {
        kbd_lost = true;
        return;
    }

    LWP_MutexLock(kbd_lock);
    kb_event_locked(event, &caps_changed);
    LWP_MutexUnlock(kbd_lock);

    if (caps_changed)
        USBKeyboard_SetLed(USBKEYBOARD_LEDCAPS, caps);
}

static void *kbd_thread(void *arg)
{
    (void)arg;

    while (kbd_thread_should_run) {
        if (kbd_lost) {
            kbd_lost = false;
            LWP_MutexLock(kbd_lock);
            release_all_locked();
            LWP_MutexUnlock(kbd_lock);
        }

        if (!USBKeyboard_IsConnected()) {
            /* Looking for a keyboard walks the USB device list through IOS, so
             * only do it a few times a second. */
            if (USBKeyboard_Open(kb_event) <= 0) {
                usleep(250000);
                continue;
            }
            caps = false;
            for (int i = 0; i < 3; i++) {
                USBKeyboard_SetLed(i, 1);
                usleep(120000);
                USBKeyboard_SetLed(i, 0);
            }
        }

        /* Scan() blocks in IOS until the keyboard sends its next report, so it
         * must not run under kbd_lock: the frame loop takes that lock in
         * inpw_keyboard_update() and INPUT_KeyPop(). kb_event() locks for each
         * event instead. */
        if (USBKeyboard_Scan() < 0)
            usleep(10000);
        else
            usleep(400);
    }

    return NULL;
}

void INPUT_KeyboardMapClear(void)
{
    memset(key_map, 0, sizeof(key_map));
}

void INPUT_KeyboardMapSet(u8 keycode, u32 button)
{
    key_map[keycode] = button;
}

static void map_defaults(void)
{
    INPUT_KeyboardMapClear();
    INPUT_KeyboardMapSet(0x52, WPAD_BUTTON_UP);
    INPUT_KeyboardMapSet(0x51, WPAD_BUTTON_DOWN);
    INPUT_KeyboardMapSet(0x50, WPAD_BUTTON_LEFT);
    INPUT_KeyboardMapSet(0x4F, WPAD_BUTTON_RIGHT);
    INPUT_KeyboardMapSet(0x28, WPAD_BUTTON_A);     /* Enter */
    INPUT_KeyboardMapSet(0x58, WPAD_BUTTON_A);     /* Enter (numpad) */
    INPUT_KeyboardMapSet(0x2A, WPAD_BUTTON_B);     /* Backspace */
    INPUT_KeyboardMapSet(0x1B, WPAD_BUTTON_1);     /* X */
    INPUT_KeyboardMapSet(0x1C, WPAD_BUTTON_2);     /* Y */
    INPUT_KeyboardMapSet(0x4C, WPAD_BUTTON_MINUS); /* Delete */
    INPUT_KeyboardMapSet(0x29, WPAD_BUTTON_HOME);  /* Esc */
    INPUT_KeyboardMapSet(0x4A, WPAD_BUTTON_HOME);  /* Home */
}

s32 inpw_keyboard_init(void)
{
    if (kbd_initialized)
        return 0;

    s32 ret = USBKeyboard_Initialize();
    if (ret < 0)
        return ret;

    if (LWP_MutexInit(&kbd_lock, false) < 0) {
        USBKeyboard_Deinitialize();
        return -1;
    }

    memset(live, 0, sizeof(live));
    memset(pressed_acc, 0, sizeof(pressed_acc));
    memset(released_acc, 0, sizeof(released_acc));
    memset(held, 0, sizeof(held));
    memset(pressed, 0, sizeof(pressed));
    memset(released, 0, sizeof(released));
    q_head = q_tail = 0;
    caps = false;
    map_defaults();

    kbd_thread_should_run = true;
    ret = LWP_CreateThread(&kbd_thread_hndl, kbd_thread, NULL, NULL, 0x4000, 0x20);
    if (ret < 0) {
        kbd_thread_should_run = false;
        LWP_MutexDestroy(kbd_lock);
        kbd_lock = LWP_MUTEX_NULL;
        USBKeyboard_Deinitialize();
        return ret;
    }

    kbd_initialized = true;
    return 0;
}

void inpw_keyboard_shutdown(void)
{
    if (!kbd_initialized)
        return;

    kbd_thread_should_run = false;
    /* Scan() blocks in IOS until the next key report, so closing the device
     * first is what wakes the thread; joining first hangs on HOME until a key
     * is pressed. */
    USBKeyboard_Close();
    if (kbd_thread_hndl != LWP_THREAD_NULL)
        LWP_JoinThread(kbd_thread_hndl, NULL);
    kbd_thread_hndl = LWP_THREAD_NULL;

    USBKeyboard_Deinitialize();
    LWP_MutexDestroy(kbd_lock);
    kbd_lock = LWP_MUTEX_NULL;
    kbd_initialized = false;
}

void inpw_keyboard_update(void)
{
    if (!kbd_initialized)
        return;

    LWP_MutexLock(kbd_lock);
    memcpy(held, live, sizeof(held));
    memcpy(pressed, pressed_acc, sizeof(pressed));
    memcpy(released, released_acc, sizeof(released));
    memset(pressed_acc, 0, sizeof(pressed_acc));
    memset(released_acc, 0, sizeof(released_acc));
    LWP_MutexUnlock(kbd_lock);
}

bool INPUT_KeyboardConnected(void)
{
    return kbd_initialized && USBKeyboard_IsConnected();
}

bool INPUT_KeyHeld(u8 keycode)     { return BIT_GET(live, keycode) != 0; }
bool INPUT_KeyPressed(u8 keycode)  { return BIT_GET(pressed, keycode) != 0; }
bool INPUT_KeyReleased(u8 keycode) { return BIT_GET(released, keycode) != 0; }
u8   INPUT_KeyModifiers(void)      { return live_mods(); }

bool INPUT_KeyPop(input_key_event *ev)
{
    bool ok = false;

    if (!kbd_initialized)
        return false;

    LWP_MutexLock(kbd_lock);
    if (q_tail != q_head) {
        if (ev)
            *ev = queue[q_tail];
        q_tail = (q_tail + 1) % QUEUE_LEN;
        ok = true;
    }
    LWP_MutexUnlock(kbd_lock);
    return ok;
}

s32 INPUT_KeyboardSetLed(input_led led, bool on)
{
    if (!kbd_initialized)
        return -1;
    return USBKeyboard_SetLed((USBKeyboard_led)led, on);
}

static u32 pad_map(const u8 *keys)
{
    u32 out = 0;

    for (int i = 0; i < 256; i++) {
        if (key_map[i] && BIT_GET(keys, i))
            out |= key_map[i];
    }
    return out;
}

u32 INPUT_KeyboardPadHeld(void) { return pad_map(held); }
u32 INPUT_KeyboardPadDown(void) { return pad_map(pressed); }
