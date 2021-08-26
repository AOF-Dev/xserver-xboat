/*
 * Xephyr - A kdrive X server thats runs in a host X window.
 *          Authored by Matthew Allum <mallum@o-hand.com>
 *
 * Copyright © 2004 Nokia
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Nokia not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission. Nokia makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * NOKIA DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL NOKIA BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _BOAT_STUFF_H_
#define _BOAT_STUFF_H_

#include <boat_keycodes.h>
#include <boat.h>
#include "xboat.h"

#define XBOAT_WANT_DEBUG 0

#if (XBOAT_WANT_DEBUG)
#define XBOAT_DBG(x, a...) \
 fprintf(stderr, __FILE__ ":%d,%s() " x "\n", __LINE__, __func__, ##a)
#else
#define XBOAT_DBG(x, a...) do {} while (0)
#endif

typedef struct XboatHostXVars XboatHostXVars;

typedef struct {
    int x, y, width, height;
} XboatBox;

typedef struct {
    short x1, y1, x2, y2;
} XboatRect;

int
hostboat_want_screen_geometry(KdScreenInfo *screen, int *width, int *height, int *x, int *y);

void
 hostboat_use_sw_cursor(void);

void
 hostboat_use_fullscreen(void);

int
 hostboat_want_fullscreen(void);

void
 hostboat_handle_signal(int signum);

int
 hostboat_init(void);

void
hostboat_add_screen(KdScreenInfo *screen, int screen_num);

void
 hostboat_set_display_name(char *name);

void
hostboat_set_screen_number(KdScreenInfo *screen, int number);

void
hostboat_set_win_title(KdScreenInfo *screen, const char *extra_text);

int
 hostboat_get_depth(void);

int
hostboat_get_server_depth(KdScreenInfo *screen);

void
hostboat_get_visual_masks(KdScreenInfo *screen,
                       CARD32 *rmsk, CARD32 *gmsk, CARD32 *bmsk);
void

hostboat_set_cmap_entry(ScreenPtr pScreen, unsigned char idx,
                     unsigned char r, unsigned char g, unsigned char b);

void *hostboat_screen_init(KdScreenInfo *screen,
                        int x, int y,
                        int width, int height, int buffer_height,
                        int *bytes_per_line, int *bits_per_pixel);

void
hostboat_paint_rect(KdScreenInfo *screen,
                 int sx, int sy, int dx, int dy, int width, int height);

Bool
hostboat_load_keymap(KeySymsPtr keySyms, CARD8 *modmap, XkbControlsPtr controls);

void
hostboat_size_set_from_configure(Bool);

BoatEvent *
hostboat_get_event(Bool queued_only);

Bool
hostboat_has_queued_event(void);

int hostboat_get_fd(void);

#endif /*_XLIBS_STUFF_H_*/
