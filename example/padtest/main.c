/*
 * padtest: shows everything libwiiinput reads.
 *
 *   - every connected controller on its own line: buttons, sticks,
 *     triggers, battery and its own raw bits
 *   - the folded buttons, sticks, IR pointer, cursor, mouse and keyboard,
 *     the way an app sees them
 *   - A+B held on a controller rumbles that controller
 *   - HOME (or Esc) held for two seconds, or RESET, exits
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include <input_wii.h>

#define COLS 74 /* the console's width at 640 pixels, less its margins */

#define HL  "\x1b[42m\x1b[30m" /* held: black on green */
#define DIM "\x1b[36m"
#define RST "\x1b[40m\x1b[37m"

static volatile bool reset_pressed;
static void on_reset(u32 irq, void *ctx)
{
    (void)irq;
    (void)ctx;
    reset_pressed = true;
}

/* One console line: printed over the last frame's, the rest blanked, so
 * nothing is cleared and nothing flickers. Escape codes don't count
 * toward the width. */
static void line(const char *fmt, ...)
{
    char buf[512];
    int visible = 0;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (const char *p = buf; *p; p++) {
        if (*p == '\x1b') {
            while (*p && *p != 'm')
                p++;
            if (!*p)
                break;
            continue;
        }
        visible++;
    }
    fputs(buf, stdout);
    fputs(RST, stdout);
    for (; visible < COLS; visible++)
        putchar(' ');
    putchar('\n');
}

static const struct { u32 bit; const char *name; } BUTTONS[] = {
    { WPAD_BUTTON_A, "A" },       { WPAD_BUTTON_B, "B" },        { WPAD_BUTTON_1, "1" },
    { WPAD_BUTTON_2, "2" },       { WPAD_BUTTON_MINUS, "-" },    { WPAD_BUTTON_PLUS, "+" },
    { WPAD_BUTTON_HOME, "HOME" }, { WPAD_BUTTON_UP, "UP" },      { WPAD_BUTTON_DOWN, "DOWN" },
    { WPAD_BUTTON_LEFT, "LEFT" }, { WPAD_BUTTON_RIGHT, "RIGHT" }, { INPUT_BUTTON_Z, "Z" },
    { INPUT_BUTTON_C, "C" },
};
#define NBUTTONS (int)(sizeof(BUTTONS) / sizeof(BUTTONS[0]))

/* Every button name, the held ones highlighted. */
static void buttons_row(u32 held)
{
    char buf[400];
    int at = snprintf(buf, sizeof(buf), "Buttons ");

    for (int i = 0; i < NBUTTONS; i++) {
        bool on = (held & BUTTONS[i].bit) != 0;
        at += snprintf(buf + at, sizeof(buf) - at, "%s %s " RST, on ? HL : "", BUTTONS[i].name);
    }
    line("%s", buf);
}

static void held_names(u32 held, char *out, size_t size)
{
    size_t at = 0;

    out[0] = 0;
    for (int i = 0; i < NBUTTONS && at < size; i++)
        if (held & BUTTONS[i].bit)
            at += snprintf(out + at, size - at, "%s ", BUTTONS[i].name);
}

/* What the GameCube adapter and USB pad drivers say, the last few lines. */
static char log_lines[3][80];
static volatile int log_next;

static int log_print(const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[strcspn(buf, "\n")] = 0;
    int i = log_next % 3;
    memcpy(log_lines[i], buf, sizeof(buf));
    log_next = i + 1;
    return n;
}

static char typed[36];

static void draw(void)
{
    float lx, ly, rx, ry, px, py;
    const char *lsrc, *rsrc;
    int cx, cy, mx, my, dx, dy, wheel, rows = 0;
    u32 held = INPUT_ButtonsHeld();
    u8 mods = INPUT_KeyModifiers(), mb = INPUT_MouseButtonsHeld();

    printf("\x1b[1;0H");
    line("libwiiinput controller test");
    line(DIM "A+B on a controller: rumble it. HOME or Esc held 2 s, or RESET: exit.");
    line("");
    buttons_row(held);

    INPUT_Stick(INPUT_STICK_LEFT, &lx, &ly, &lsrc);
    INPUT_Stick(INPUT_STICK_RIGHT, &rx, &ry, &rsrc);
    line("Source  %-11s  Gforce %.2f", INPUT_Source(), INPUT_Gforce());
    line("Sticks  L %+.2f %+.2f %-11s R %+.2f %+.2f %s", lx, ly, lsrc, rx, ry, rsrc);

    bool ir = INPUT_Pointer(&px, &py), cur = INPUT_CursorActive();
    INPUT_CursorPosition(&cx, &cy);
    char irs[24] = "-", cs[24] = "-";
    if (ir) snprintf(irs, sizeof(irs), "%d,%d", (int)px, (int)py);
    if (cur) snprintf(cs, sizeof(cs), "%d,%d", cx, cy);
    line("Pointer IR %-10s Cursor %s", irs, cs);

    INPUT_MousePosition(&mx, &my);
    INPUT_MouseDelta(&dx, &dy, &wheel);
    line("Mouse   %-3s %3d,%-3d  buttons %c%c%c  wheel %+d", INPUT_MouseConnected() ? "yes" : "no", mx, my,
         (mb & INPUT_MOUSE_LEFT) ? 'L' : '-', (mb & INPUT_MOUSE_MIDDLE) ? 'M' : '-',
         (mb & INPUT_MOUSE_RIGHT) ? 'R' : '-', wheel);
    line("Keys    %-3s %c%c%c%c  typed: %s_", INPUT_KeyboardConnected() ? "yes" : "no",
         (mods & INPUT_MOD_SHIFT) ? 'S' : '-', (mods & INPUT_MOD_CTRL) ? 'C' : '-', (mods & INPUT_MOD_ALT) ? 'A' : '-',
         (mods & INPUT_MOD_GUI) ? 'G' : '-', typed);
#if defined(HAVE_GCADAPTER) || defined(HAVE_HIDPADS)
    line("Adapter %-3s  USB pads %s",
#ifdef HAVE_GCADAPTER
         INPUT_GCAdapterConnected() ? "yes" : "no",
#else
         "off",
#endif
#ifdef HAVE_HIDPADS
         INPUT_HIDPadConnected() ? "yes" : "no"
#else
         "off"
#endif
    );
#endif
    line("");

    int n = INPUT_DeviceCount();
    line("Controllers: %d", n);
    for (int i = 0; i < n && rows < 12; i++, rows++) {
        input_device d;
        char names[48], bat[16] = "  - ";
        INPUT_DeviceGet(i, &d);
        held_names(d.held, names, sizeof(names));
        if (d.battery >= 0)
            snprintf(bat, sizeof(bat), "%3d%%", d.battery);
        line("%s%d" RST " %-11s %d %s %08x%s%04x L%+.1f%+.1f R%+.1f%+.1f T%.1f/%.1f", d.held ? HL : "", i + 1,
             d.name, d.port + 1, bat, (unsigned)d.raw, d.raw_ext ? "+" : " ",
             (unsigned)(d.raw_ext >> 16), d.lx, d.ly, d.rx, d.ry, d.lt, d.rt);
        if (names[0] || d.pointer) {
            char ptr[24] = "";
            if (d.pointer)
                snprintf(ptr, sizeof(ptr), "IR %d,%d ", (int)d.px, (int)d.py);
            line("   %s%s", ptr, names);
            rows++;
        }
    }
    for (; rows < 12; rows++)
        line("");
    for (int i = 0; i < 3; i++) {
        const char *l = log_lines[(log_next + i) % 3];
        line(DIM "%s%.70s", l[0] ? "log: " : "", l);
    }
}

int main(void)
{
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    printf("\x1b[2J");

    SYS_SetResetCallback(on_reset);
    fatInitDefault(); /* for sd:/controllers/ layouts */

#ifdef HAVE_GCADAPTER
    INPUT_GCAdapterSetLog(log_print);
#endif
#ifdef HAVE_HIDPADS
    INPUT_HIDPadSetLog(log_print);
#endif
    INPUT_Init();
    INPUT_SetScreenSize(rmode->fbWidth, rmode->xfbHeight);

    u64 home_since = 0;
    bool leaving = false;

    while (!leaving && !reset_pressed) {
        VIDEO_WaitVSync();
        INPUT_Update();

        input_key_event ev;
        while (INPUT_KeyPop(&ev)) {
            size_t len = strlen(typed);
            if (ev.code == 0x2A && len) /* Backspace */
                typed[len - 1] = 0;
            else if (ev.ch) {
                if (len >= sizeof(typed) - 1) {
                    memmove(typed, typed + 1, len);
                    len--;
                }
                typed[len] = ev.ch;
                typed[len + 1] = 0;
            }
        }

        for (int i = 0; i < INPUT_DeviceCount(); i++) {
            input_device d;
            INPUT_DeviceGet(i, &d);
            INPUT_DeviceRumble(i, (d.held & (WPAD_BUTTON_A | WPAD_BUTTON_B)) == (WPAD_BUTTON_A | WPAD_BUTTON_B));
        }

        if (INPUT_ButtonsHeld() & WPAD_BUTTON_HOME) {
            if (!home_since)
                home_since = gettime();
            else if (ticks_to_millisecs(gettime() - home_since) >= 2000)
                leaving = true;
        } else {
            home_since = 0;
        }

        draw();
    }

    INPUT_Shutdown();
    return 0;
}
