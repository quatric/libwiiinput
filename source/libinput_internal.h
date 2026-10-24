#ifndef LIBINPUT_INTERNAL_H
#define LIBINPUT_INTERNAL_H

#include "input_wii.h"

extern int inpw_screen_w, inpw_screen_h;

void inpw_set_source(const char *name);

s32  inpw_pads_init(u32 flags);
void inpw_pads_shutdown(void);
void inpw_pads_update(void);
void inpw_pads_set_screen(int w, int h);

s32  inpw_keyboard_init(void);
void inpw_keyboard_shutdown(void);
void inpw_keyboard_update(void);

s32  inpw_mouse_init(void);
void inpw_mouse_shutdown(void);
void inpw_mouse_update(void);
void inpw_mouse_set_bounds(int w, int h);
void inpw_cursor_update(void);

#ifdef HAVE_WPADQUEUE
void inpw_wpadqueue_link(void);
#endif

#ifdef HAVE_GCADAPTER
void inpw_gcadapter_init(bool enabled);
void inpw_gcadapter_shutdown(void);
void inpw_gcadapter_update(void);
#endif

#ifdef HAVE_TABLET
#include "input_tablet.h"
/* the uDraw, for the TB_* calls in libinput_tablet.c */
bool inpw_udraw_pop(tb_sample *out);
bool inpw_udraw_state(tb_sample *out);
bool inpw_udraw_info(tb_info *info);
void inpw_udraw_extent(s32 *w, s32 *h);
#endif

#endif
