# libwiiinput

USB keyboard and mouse input for the Nintendo Wii, built on libogc.

- One call reads every controller and normalises it to `WPAD_BUTTON_*`: Wii Remote,
  Classic Controller, GameCube pad, plus optional Wii U Pro Controller (libwupc),
  Wii U GamePad (libwiidrc), DualShock 3 (libsicksaxis) and the GameCube controller
  adapter for Wii U (no extra library, from RiftWii)
- Background thread keeps the keyboard connected and scanned (hot-plug safe)
- Per-frame polling: held / pressed / released for every USB HID key
- Optional keyboard-to-Wii-Remote mapping (`INPUT_KeyboardPadHeld/Down`)
- Mouse position (clamped to configurable bounds, optional speed multiplier), deltas, wheel and buttons
- Wii Speak microphone (`input_wiispeak.h`): boots the mic and streams 16-bit mono samples
- USB microphones (`input_usbmic.h`): any class-compliant (USB Audio Class 1.0) mic, streamed as 16-bit mono
- USB pen tablets (`input_tablet.h`): Wacom, Huion, XP-Pen and other drawing tablets, with pressure, tilt, eraser, tablet keys and Bamboo finger touch
- The uDraw GameTablet for the Wii, through the same `input_tablet.h` calls, with pressure and both rocker buttons

The microphone and tablet drivers are compile options, off by default (see below).

The keyboard handling was originally part of GlowWii and has been split out
into a reusable library; mouse support is new.

## Build

Requires devkitPro with devkitPPC and libogc.

```sh
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
make            # produces lib/libinput-wii.a
make install    # copies into $DEVKITPRO/portlibs/ppc
```

### Optional controllers

```sh
make controllers      # fetches and builds libwiidrc, libwupc, libsicksaxis into ./portlibs/ppc
make WITH_WIIDRC=1 WITH_WUPC=1 WITH_SICKSAXIS=1
```

Enable the same flags when linking your app, adding `-lwiidrc`, `-lwupc` (with
`-Wl,-wrap,wiiuse_accept`) and `-lsicksaxis` ahead of `-lwiiuse`.

Link with `-linput-wii -lwiikeyboard -logc` (and the usual `-lwiiuse -lbte -lm`).

```sh
make WITH_GCADAPTER=1   # the GameCube controller adapter for Wii U, no extra library needed
make WITH_WPADQUEUE=1   # a fourth Wii Remote and the Balance Board without a crash
```

`WITH_WPADQUEUE=1` has to be linked with `-Wl,--wrap=wiiuse_init_cmd_queue` (see below).

### Optional drivers

```sh
make WITH_TABLET=1    # USB pen tablets and the uDraw GameTablet (input_tablet.h)
make WITH_MIC=1       # Wii Speak and USB microphones (input_wiispeak.h, input_usbmic.h)
```

Each option can be combined with the others; every set of options builds into its own folder under
`build/`, so switching them never mixes objects. Without an option its sources compile to nothing,
and apps get link errors if they call into it. A library built with `WITH_TABLET=1` also has to be
linked with `-Wl,--wrap=classic_ctrl_handshake,--wrap=parse_event,--wrap=USB_GetDeviceList` (see the
pen tablet and uDraw sections).

## Usage

```c
#include <input_wii.h>

INPUT_Init();
INPUT_SetScreenSize(640, 480);

while (running) {
    VIDEO_WaitVSync();
    INPUT_Update();                        /* once per frame */

    u32 down = INPUT_ButtonsDown();        /* WPAD_BUTTON_* from any device */
    if (down & WPAD_BUTTON_A) { }

    float x, y;
    if (INPUT_Pointer(&x, &y)) { }         /* Wii Remote IR */
    INPUT_Stick(INPUT_STICK_LEFT, &x, &y, NULL);

    input_key_event ev;
    while (INPUT_KeyPop(&ev)) { /* ev.ch is typed ASCII */ }

    if (INPUT_MouseButtonsPressed() & INPUT_MOUSE_LEFT) { }
}

INPUT_Shutdown();
```

* Buttons are `WPAD_BUTTON_*` bits plus `INPUT_BUTTON_Z` / `INPUT_BUTTON_C` for the
  Nunchuk. Classic Controller, GameCube, Wii U Pro, GamePad and DualShock 3 are
  folded onto the Wii Remote layout.
* Sticks are -1..1, y up, no deadzone; the strongest one across devices wins.
* `INPUT_SetCursorMode(INPUT_CURSOR_IR | INPUT_CURSOR_MOUSE | INPUT_CURSOR_STICK)` picks
  which devices drive the pointer cursor (`INPUT_CursorActive/Position/Moved`),
  so the Wii pointer is optional per app. Draw your own cursor sprite.
* `INPUT_Source()` names the device used last, handy for on-screen hints.
* `INPUT_KeyboardMapSet/Clear` change the keyboard-as-controller mapping.
* `INPUT_GameCubeMapSet/Clear` do the same for the GameCube pad.
* `INPUT_IgnoreWiimoteButtons` leaves a channel's own Wii Remote buttons to another layer.
* `INPUT_StickRaw` gives a stick before its length is clamped, for apps with their own scaling.
* `INPUT_GCAdapterPad/Rumble/Connected` reach the Wii U GameCube adapter's ports (`WITH_GCADAPTER`).

### GameCube controller adapter for Wii U

With `WITH_GCADAPTER=1`, Nintendo's WUP-028 adapter (and the third-party ones that copy it) works
like the console's own GameCube ports: its four controllers go into `INPUT_ButtonsHeld/Down` and
`INPUT_Stick` through the same `INPUT_GameCubeMapSet` mapping, with the source `"GameCube"`. It is
looked for every two seconds, so it can be plugged in with the app running. `INPUT_GCAdapterPad`
gives a port's raw state (buttons, sticks, analogue triggers, WaveBird or not),
`INPUT_GCAdapterRumble` rumbles one (with the grey power cable plugged in),
`INPUT_GCAdapterConnected` says whether it is reporting, and `INPUT_GCAdapterSetLog(printf)` shows
what the driver does. Sticks centre on where they rest when a controller is plugged in; X+Y+Start
held for three seconds takes the centre again. `INPUT_InitEx(INPUT_INIT_NO_GCADAPTER)` leaves it
alone.

The adapter is a USB HID device read through IOS's `/dev/usb/hid`. Under IOS 58 (the Homebrew
Channel's), libogc's USB already holds that device, so the driver goes through libogc's device list
and transfers; under a d2x cIOS (v4) it opens `/dev/usb/hid` itself. Every request is asynchronous,
and a USB module that does not answer within 1.5 s turns the adapter off instead of hanging the app.
`INPUT_Shutdown` gives the adapter back, so the next program can open it.

The driver (`source/libinput_gcad.c`) and its libogc side (`source/libinput_gcadapter.c`) come from
[RiftWii](https://github.com/KakarottoCake/Riftwii), which built them on wiibrew's IOS documentation,
Dolphin's emulation and adapter code, wup-028-bslug (Alex Chadwick) and Nintendont (FIX94 and
contributors). Those files are GPL-3.0-or-later, which combines with this library's AGPL-3.0. Not
yet tested on real hardware as part of this library; RiftWii's own testers use it on Wiis and Wii Us.

### A fourth Wii Remote and the Balance Board

libogc 3.1 gives each of its six Bluetooth slots a 256-command queue from its kernel heap, which only
has room for three. The fourth Wii Remote, or a Balance Board left on from another game, gets an
empty queue and crashes the program as it connects. `WITH_WPADQUEUE=1` (also from RiftWii) gives the
queues normal heap memory instead; apps link with

```make
LDFLAGS += -Wl,--wrap=wiiuse_init_cmd_queue
```

or the link fails on `__wrap_wiiuse_init_cmd_queue`. The library also sets the data format and
pointer size on the four Wii Remote channels only, never the Balance Board's.

### Wii Speak

```c
#include <input_wiispeak.h>

WS_SetLog(printf);                         /* optional: show the boot steps */
if (WS_Open(firmware, firmware_size) == WS_OK) {
    WS_SetFreq(WS_FREQ_16KHZ);
    WS_SetSignalProcessing(false);
    WS_Start();
    /* each frame: */
    s16 buf[1024];
    u32 n = WS_Read(buf, 1024);            /* host-endian samples */
    WS_Stop();
    WS_Close();
}
```

The Wii Speak (`057e:0308`) is not a USB audio class device. The driver reimplements the RVL SDK's
`PMIC` library over `/dev/usb/oh0`, and was taken from RecordMii. The mic has no firmware of its own:
`WS_Open` uploads the 97,480-byte `PMIC_FIRM_DATA` blob, which is Nintendo's and is **not** included.
Apps have to get it from the user (RecordMii's `tools/extract_pmic_firmware.py` pulls it out of an
RVL SDK `pmic.a`). Dolphin's emulated Wii Speak accepts any non-empty blob and records from the host
microphone. Not yet tested on real hardware.

### USB microphones

```c
#include <input_usbmic.h>

um_info mic;

UM_SetLog(printf);                         /* optional: show the scan */
if (UM_Open(&mic) == UM_OK) {              /* mic.rate, mic.name, ... */
    UM_SetVolume(75);                      /* if mic.has_volume */
    UM_Start();
    /* each frame: */
    s16 buf[1024];
    u32 n = UM_Read(buf, 1024);            /* mono, at mic.rate */
    UM_Stop();
    UM_Close();
}
```

Works with the plug-and-play USB mics sold for PCs, which follow the USB Audio Class 1.0 spec. The
driver reads the format list from the mic itself and picks 16-bit PCM at 48 kHz when offered (then
44.1 kHz, then whatever is closest); stereo and 24-bit streams are folded to 16-bit mono. It unmutes
the mic and exposes its volume control as a percentage. Full-speed only, which is all the Wii's
ports run at; mics that only stream at high speed (USB Audio Class 2.0) are not supported. Needs an
IOS with `/dev/usb/oh0` or `/dev/usb/ven`. Not yet tested on real hardware.

### Pen tablets

```c
#include <input_tablet.h>

INPUT_Init();                              /* the USB mouse works alongside, see below */
TB_Init();                                 /* finds the tablet, now or when plugged in */

/* each frame: */
tb_sample s;
while (TB_Pop(&s)) {                       /* every report, 130-200 a second */
    int x, y;
    TB_ToScreen(&s, 640, 480, &x, &y);
    if (s.buttons & TB_BUTTON_TIP) { /* draw, s.pressure is 0..1 */ }
}

TB_Shutdown();
```

The driver reads the tablet's HID report descriptor and finds the pen's fields there, so it is not
tied to one model. That covers Wacom's current tablets (Intuos CTL-4100/6100, Wacom One, Intuos Pro),
which put the pen on Wacom's vendor usage page `0xFF0D` as a mirror of the standard digitizer page,
and the Huion, XP-Pen and Gaomon tablets that use the standard page itself. Older Wacoms (Bamboo,
CTL-480 era Intuos), whose descriptor describes no pen, are switched into tablet mode with feature
report 2 and read in their fixed format. On a Bamboo Pen & Touch the touch surface is read too:
fingers come out of `TB_Pop` as samples with `finger` set (and from `TB_GetFingers`), and are
ignored while the pen is in range. `TB_GetInfo` gives the coordinate and pressure range,
`TB_GetState` the latest sample, and `TB_GetStats` the raw last report, to help with a tablet that
does not work.

Most tablets also present themselves as a boot-protocol mouse, and libogc's mouse driver opens the
first one it finds: it could take the tablet instead of a real mouse, and steal the pen's reports.
The link wrap `--wrap=USB_GetDeviceList` hides tablets from libogc's device lists (the makers'
vendor IDs, and any device this driver has found to be a tablet), so its mouse and keyboard drivers
only see the rest and a USB mouse works side by side with the tablet. The driver itself still sees
everything, and remembers devices that are certainly not tablets so its scan, which repeats every
second until a tablet turns up, stops opening them. A read stays queued in IOS while the pen is away from the tablet, so apps should reload IOS
before returning to the loader, as `example/tablet` does.

The descriptor parser (`source/libinput_hidparse.c`) has no libogc dependency and was checked on a
computer against Wacom descriptors from the Linux kernel's HID selftests (PTH-460/660/860 and a
CTL-style pen). Tested on hardware with a Wacom Intuos S (CTL-4100), pen only as it has no touch
sensor, and with the finger side of a Bamboo Pen & Touch (CTH-460). The Bamboo's pen has not been
tried yet.

Under USBV5 IOSes (what the Homebrew Channel runs) a device is listed once per interface, and an
interrupt read goes to the interface its handle stands for, so the driver opens the pen and touch
interfaces through their own list entries, matched by the interface number in each entry's token.

`example/tablet` is a pressure-sensitive painting program built on it: `make` there makes a
`boot.dol` for the Homebrew Channel, with controls listed at the top of its `main.c`. Drawings save
to `sd:/sketches/` as BMP files.

### uDraw GameTablet

The THQ uDraw GameTablet plugs into the Wii Remote like a Nunchuk. With `WITH_TABLET=1` it shows up
through the pen tablet calls above: `TB_Pop` gives one sample per Wii Remote report with `wiimote`
set to the channel it came from (`-1` for a USB tablet), `TB_Connected` and `TB_GetInfo` see it,
`TB_ToScreen` maps it, and `TB_UDrawConnected(chan)` tells which channels have one. The pen gives
x/y over a 1834 x 1344 area, 9-bit pressure (`TB_BUTTON_TIP` once it passes a small threshold, with
hysteresis), and the two rockers come out as `TB_BUTTON_BARREL2` (upper) and `TB_BUTTON_BARREL`
(lower). The pen area can be adjusted with `TB_UDrawSetArea` for a tablet that does not reach the
corners. `TB_Init` is not needed for the uDraw alone, and up to four can be used at once.

wiiuse knows nothing of the uDraw and would take it for a Classic Controller, so the driver hooks
two of its functions with linker wraps. Apps that use it have to link with

```make
LDFLAGS += -Wl,--wrap=classic_ctrl_handshake,--wrap=parse_event,--wrap=USB_GetDeviceList
```

or the link fails on `__real_parse_event` (the last wrap is the USB tablets', see above). The decoding is written against libogc 3.1's wiiuse
(a warning shows up when building against another version).

libogc's Bluetooth stack can drop an output report while reporting it sent, and wiiuse then waits
forever for its answer, so an extension's handshake never finishes. This happens with every
extension in Dolphin, the Nunchuk too. The uDraw driver resends a command that has had no answer
for 300 ms, which fixes it for every extension while the driver is linked in.

`example/udraw` is a painting program for it: up to four tablets at once, the upper rocker picks the
next colour, the lower one held is the eraser, +/- change the brush, 1 clears, 2 saves a BMP to
`sd:/udraw/`, HOME exits. `make TEST=1` there builds a version that logs to `sd:/udraw/test/` and
powers off after 20 s, for driving it from Dolphin. Dolphin emulates the uDraw (choose it as the
Wii Remote's extension); for its stylus to move while the window is in the background, turn on
background input. Tested on a real uDraw GameTablet and in Dolphin 2609.

The report format comes from WiiBrew and Dolphin's emulation of the tablet, with
rvlpaint by cmyksoda as a working reference.

### Using it from an app

Add it as a git submodule and compile its sources with your own flags, so the
`HAVE_*` options apply to it too:

```sh
git submodule add https://github.com/quatric/libwiiinput.git libwiiinput
```

```make
SOURCES  += libwiiinput/source
CFLAGS   += -Ilibwiiinput/include
CFLAGS   += -DHAVE_TABLET -DHAVE_MIC     # only the drivers you use
CFLAGS   += -DHAVE_GCADAPTER -DHAVE_WPADQUEUE
LDFLAGS  += -Wl,--wrap=classic_ctrl_handshake,--wrap=parse_event,--wrap=USB_GetDeviceList   # with HAVE_TABLET
LDFLAGS  += -Wl,--wrap=wiiuse_init_cmd_queue   # with HAVE_WPADQUEUE
```

See `example/main.c` for a complete program.

## License

AGPL-3.0, see `LICENSE`. The files taken from RiftWii (`libinput_gcad.*`, `libinput_gcadapter.c`,
`libinput_wpadqueue.c`) keep their GPL-3.0-or-later notices.
