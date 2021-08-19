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

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "hostx.h"
#include "input.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>             /* for memset */
#include <errno.h>
#include <time.h>
#include <err.h>

#include <sys/ipc.h>
#include <sys/time.h>
#include <sys/mman.h>

#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_image.h>
#include <xcb/shape.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>
#include <xcb/xkb.h>
#ifdef GLAMOR
#include <epoxy/gl.h>
#include "glamor.h"
#include "ephyr_glamor_glx.h"
#endif
#include "ephyrlog.h"
#include "ephyr.h"

struct EphyrHostXVars {
    char *server_dpy_name;
    xcb_connection_t *conn;
    int screen;
    xcb_visualtype_t *visual;
    Window winroot;
    xcb_generic_event_t *saved_event;
    int depth;
    Bool use_sw_cursor;
    Bool use_fullscreen;

    int n_screens;
    KdScreenInfo **screens;

    long damage_debug_msec;
    Bool size_set_from_configure;
};

/* memset ( missing> ) instead of below  */
/*static EphyrHostXVars HostX = { "?", 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};*/
static EphyrHostXVars HostX;

static int HostXWantDamageDebug = 0;

extern Bool EphyrWantResize;

Bool ephyr_glamor = FALSE;
extern Bool ephyr_glamor_skip_present;

Bool
hostx_has_extension(xcb_extension_t *extension)
{
    const xcb_query_extension_reply_t *rep;

    rep = xcb_get_extension_data(HostX.conn, extension);

    return rep && rep->present;
}

#define host_depth_matches_server(_vars) (HostX.depth == (_vars)->server_depth)

int
hostx_want_screen_geometry(KdScreenInfo *screen, int *width, int *height, int *x, int *y)
{
    EphyrScrPriv *scrpriv = screen->driver;

    if (scrpriv && HostX.use_fullscreen == TRUE) {
        *x = scrpriv->win_x;
        *y = scrpriv->win_y;
        *width = scrpriv->win_width;
        *height = scrpriv->win_height;
        return 1;
    }

    return 0;
}

void
hostx_add_screen(KdScreenInfo *screen, int screen_num)
{
    EphyrScrPriv *scrpriv = screen->driver;
    int index = HostX.n_screens;

    HostX.n_screens += 1;
    HostX.screens = reallocarray(HostX.screens,
                                 HostX.n_screens, sizeof(HostX.screens[0]));
    HostX.screens[index] = screen;

    scrpriv->screen = screen;
}

void
hostx_set_display_name(char *name)
{
    HostX.server_dpy_name = strdup(name);
}

void
hostx_set_screen_number(KdScreenInfo *screen, int number)
{
    EphyrScrPriv *scrpriv = screen->driver;

    if (scrpriv) {
        scrpriv->mynum = number;
        hostx_set_win_title(screen, "");
    }
}

void
hostx_set_win_title(KdScreenInfo *screen, const char *extra_text)
{
    EphyrScrPriv *scrpriv = screen->driver;

    if (!scrpriv)
        return;

    {
#define BUF_LEN 256
        char buf[BUF_LEN + 1];

        memset(buf, 0, BUF_LEN + 1);
        snprintf(buf, BUF_LEN, "Xboat on %s.%d %s",
                 HostX.server_dpy_name ? HostX.server_dpy_name : ":0",
                 scrpriv->mynum, (extra_text != NULL) ? extra_text : "");

        // boatSetTitle(buf);
    }
}

void
hostx_use_sw_cursor(void)
{
    HostX.use_sw_cursor = TRUE;
}

void
hostx_use_fullscreen(void)
{
    HostX.use_fullscreen = TRUE;
}

int
hostx_want_fullscreen(void)
{
    return HostX.use_fullscreen;
}

static void
hostx_toggle_damage_debug(void)
{
    HostXWantDamageDebug ^= 1;
}

void
hostx_handle_signal(int signum)
{
    hostx_toggle_damage_debug();
    EPHYR_DBG("Signal caught. Damage Debug:%i\n", HostXWantDamageDebug);
}

#ifdef __SUNPRO_C
/* prevent "Function has no return statement" error for x_io_error_handler */
#pragma does_not_return(exit)
#endif

int
hostx_init(void)
{
    uint32_t attrs[2];
    uint32_t attr_mask = 0;
    int index;
    xcb_screen_t *xscreen;

    attrs[0] =
        XCB_EVENT_MASK_BUTTON_PRESS
        | XCB_EVENT_MASK_BUTTON_RELEASE
        | XCB_EVENT_MASK_POINTER_MOTION
        | XCB_EVENT_MASK_KEY_PRESS
        | XCB_EVENT_MASK_KEY_RELEASE
        | XCB_EVENT_MASK_EXPOSURE
        | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    attr_mask |= XCB_CW_EVENT_MASK;

    EPHYR_DBG("mark");
#ifdef GLAMOR
    if (ephyr_glamor)
        HostX.conn = ephyr_glamor_connect();
    else
#endif
        HostX.conn = xcb_connect(NULL, &HostX.screen);
    if (!HostX.conn || xcb_connection_has_error(HostX.conn)) {
        fprintf(stderr, "\nXephyr cannot open host display. Is DISPLAY set?\n");
        exit(1);
    }

    xscreen = xcb_aux_get_screen(HostX.conn, HostX.screen);
    HostX.winroot = xscreen->root;
    HostX.depth = xscreen->root_depth;
#ifdef GLAMOR
    if (ephyr_glamor) {
        HostX.visual = ephyr_glamor_get_visual();
        if (HostX.visual->visual_id != xscreen->root_visual) {
            attrs[1] = xcb_generate_id(HostX.conn);
            attr_mask |= XCB_CW_COLORMAP;
            xcb_create_colormap(HostX.conn,
                                XCB_COLORMAP_ALLOC_NONE,
                                attrs[1],
                                HostX.winroot,
                                HostX.visual->visual_id);
        }
    } else
#endif
        HostX.visual = xcb_aux_find_visual_by_id(xscreen,xscreen->root_visual);

    for (index = 0; index < HostX.n_screens; index++) {
        KdScreenInfo *screen = HostX.screens[index];
        EphyrScrPriv *scrpriv = screen->driver;

        scrpriv->win = xcb_generate_id(HostX.conn);
        scrpriv->server_depth = HostX.depth;
        scrpriv->ximg = NULL;
        scrpriv->win_x = 0;
        scrpriv->win_y = 0;

        {
            xcb_create_window(HostX.conn,
                              XCB_COPY_FROM_PARENT,
                              scrpriv->win,
                              HostX.winroot,
                              0,0,100,100, /* will resize */
                              0,
                              XCB_WINDOW_CLASS_COPY_FROM_PARENT,
                              HostX.visual->visual_id,
                              attr_mask,
                              attrs);

            hostx_set_win_title(screen,
                                "(ctrl+shift grabs mouse and keyboard)");

            if (HostX.use_fullscreen) {
                scrpriv->win_width  = xscreen->width_in_pixels;
                scrpriv->win_height = xscreen->height_in_pixels;
            }
        }
    }

    HostX.debug_fill_color = 0x000000ff; // red for RGBA8888

    {
        CursorVisible = TRUE;
        /* Ditch the cursor, we provide our 'own' */
    }

    xcb_flush(HostX.conn);

    /* Setup the pause time between paints when debugging updates */

    HostX.damage_debug_msec = 20000;    /* 1/50 th of a second */

    if (getenv("XEPHYR_PAUSE")) {
        HostX.damage_debug_msec = strtol(getenv("XEPHYR_PAUSE"), NULL, 0);
        EPHYR_DBG("pause is %li\n", HostX.damage_debug_msec);
    }

    return 1;
}

int
hostx_get_depth(void)
{
    return HostX.depth;
}

int
hostx_get_server_depth(KdScreenInfo *screen)
{
    EphyrScrPriv *scrpriv = screen->driver;

    return scrpriv ? scrpriv->server_depth : 0;
}

void
hostx_get_visual_masks(KdScreenInfo *screen,
                       CARD32 *rmsk, CARD32 *gmsk, CARD32 *bmsk)
{
    EphyrScrPriv *scrpriv = screen->driver;

    if (!scrpriv)
        return;

    if (host_depth_matches_server(scrpriv)) {
        *rmsk = HostX.visual->red_mask;
        *gmsk = HostX.visual->green_mask;
        *bmsk = HostX.visual->blue_mask;
    }
    else if (scrpriv->server_depth == 16) {
        /* Assume 16bpp 565 */
        *rmsk = 0xf800;
        *gmsk = 0x07e0;
        *bmsk = 0x001f;
    }
    else {
        *rmsk = 0x0;
        *gmsk = 0x0;
        *bmsk = 0x0;
    }
}

static int
hostx_calculate_color_shift(unsigned long mask)
{
    int shift = 1;

    /* count # of bits in mask */
    while ((mask = (mask >> 1)))
        shift++;
    /* cmap entry is an unsigned char so adjust it by size of that */
    shift = shift - sizeof(unsigned char) * 8;
    if (shift < 0)
        shift = 0;
    return shift;
}

void
hostx_set_cmap_entry(ScreenPtr pScreen, unsigned char idx,
                     unsigned char r, unsigned char g, unsigned char b)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    EphyrScrPriv *scrpriv = screen->driver;
/* need to calculate the shifts for RGB because server could be BGR. */
/* XXX Not sure if this is correct for 8 on 16, but this works for 8 on 24.*/
    static int rshift, bshift, gshift = 0;
    static int first_time = 1;

    if (first_time) {
        first_time = 0;
        rshift = hostx_calculate_color_shift(HostX.visual->red_mask);
        gshift = hostx_calculate_color_shift(HostX.visual->green_mask);
        bshift = hostx_calculate_color_shift(HostX.visual->blue_mask);
    }
    scrpriv->cmap[idx] = ((r << rshift) & HostX.visual->red_mask) |
        ((g << gshift) & HostX.visual->green_mask) |
        ((b << bshift) & HostX.visual->blue_mask);
}

/**
 * hostx_screen_init creates the XImage that will contain the front buffer of
 * the ephyr screen, and possibly offscreen memory.
 *
 * @param width width of the screen
 * @param height height of the screen
 * @param buffer_height  height of the rectangle to be allocated.
 *
 * hostx_screen_init() creates an XImage, using MIT-SHM if it's available.
 * buffer_height can be used to create a larger offscreen buffer, which is used
 * by fakexa for storing offscreen pixmap data.
 */
void *
hostx_screen_init(KdScreenInfo *screen,
                  int x, int y,
                  int width, int height, int buffer_height,
                  int *bytes_per_line, int *bits_per_pixel)
{
    EphyrScrPriv *scrpriv = screen->driver;

    if (!scrpriv) {
        fprintf(stderr, "%s: Error in accessing hostx data\n", __func__);
        exit(1);
    }

    EPHYR_DBG("host_screen=%p x=%d, y=%d, wxh=%dx%d, buffer_height=%d",
              screen, x, y, width, height, buffer_height);

    if (scrpriv->ximg != NULL) {
        /* Free up the image data if previously used
         * i.ie called by server reset
         */

        {
            free(scrpriv->ximg->data);
            scrpriv->ximg->data = NULL;

            xcb_image_destroy(scrpriv->ximg);
        }
    }

    if (!ephyr_glamor) {
        EPHYR_DBG("Creating image %dx%d for screen scrpriv=%p\n",
                  width, buffer_height, scrpriv);
        scrpriv->ximg = xcb_image_create_native(HostX.conn,
                                                    width,
                                                    buffer_height,
                                                    XCB_IMAGE_FORMAT_Z_PIXMAP,
                                                    HostX.depth,
                                                    NULL,
                                                    ~0,
                                                    NULL);

        /* Match server byte order so that the image can be converted to
         * the native byte order by xcb_image_put() before drawing */
        if (host_depth_matches_server(scrpriv))
            scrpriv->ximg->byte_order = IMAGE_BYTE_ORDER;

        scrpriv->ximg->data =
            xallocarray(scrpriv->ximg->stride, buffer_height);
    }

    if (!HostX.size_set_from_configure)
    {
        uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        uint32_t values[2] = {width, height};
        xcb_configure_window(HostX.conn, scrpriv->win, mask, values);
    }

    if (!EphyrWantResize) {
        /* Ask the WM to keep our size static */
        xcb_size_hints_t size_hints = {0};
        size_hints.max_width = size_hints.min_width = width;
        size_hints.max_height = size_hints.min_height = height;
        size_hints.flags = (XCB_ICCCM_SIZE_HINT_P_MIN_SIZE |
                            XCB_ICCCM_SIZE_HINT_P_MAX_SIZE);
        xcb_icccm_set_wm_normal_hints(HostX.conn, scrpriv->win,
                                      &size_hints);
    }

#ifdef GLAMOR
    if (!ephyr_glamor_skip_present)
#endif
        xcb_map_window(HostX.conn, scrpriv->win);


    xcb_aux_sync(HostX.conn);

    scrpriv->win_width = width;
    scrpriv->win_height = height;
    scrpriv->win_x = x;
    scrpriv->win_y = y;

#ifdef GLAMOR
    if (ephyr_glamor) {
        *bytes_per_line = 0;
        ephyr_glamor_set_window_size(scrpriv->glamor,
                                     scrpriv->win_width, scrpriv->win_height);
        return NULL;
    } else
#endif
    if (host_depth_matches_server(scrpriv)) {
        *bytes_per_line = scrpriv->ximg->stride;
        *bits_per_pixel = scrpriv->ximg->bpp;

        EPHYR_DBG("Host matches server");
        return scrpriv->ximg->data;
    }
    else {
        int bytes_per_pixel = scrpriv->server_depth >> 3;
        int stride = (width * bytes_per_pixel + 0x3) & ~0x3;

        *bytes_per_line = stride;
        *bits_per_pixel = scrpriv->server_depth;

        EPHYR_DBG("server bpp %i", bytes_per_pixel);
        scrpriv->fb_data = xallocarray (stride, buffer_height);
        return scrpriv->fb_data;
    }
}

static void hostx_paint_debug_rect(KdScreenInfo *screen,
                                   int x, int y, int width, int height);

void
hostx_paint_rect(KdScreenInfo *screen,
                 int sx, int sy, int dx, int dy, int width, int height)
{
    EphyrScrPriv *scrpriv = screen->driver;

    EPHYR_DBG("painting in screen %d\n", scrpriv->mynum);

#ifdef GLAMOR
    if (ephyr_glamor) {
        BoxRec box;
        RegionRec region;

        box.x1 = dx;
        box.y1 = dy;
        box.x2 = dx + width;
        box.y2 = dy + height;

        RegionInit(&region, &box, 1);
        ephyr_glamor_damage_redisplay(scrpriv->glamor, &region);
        RegionUninit(&region);
        return;
    }
#endif

    /*
     *  Copy the image data updated by the shadow layer
     *  on to the window
     */

    if (HostXWantDamageDebug) {
        hostx_paint_debug_rect(screen, dx, dy, width, height);
    }

    /*
     * If the depth of the ephyr server is less than that of the host,
     * the kdrive fb does not point to the ximage data but to a buffer
     * ( fb_data ), we shift the various bits from this onto the XImage
     * so they match the host.
     *
     * Note, This code is pretty new ( and simple ) so may break on
     *       endian issues, 32 bpp host etc.
     *       Not sure if 8bpp case is right either.
     *       ... and it will be slower than the matching depth case.
     */

    if (!host_depth_matches_server(scrpriv)) {
        int x, y, idx, bytes_per_pixel = (scrpriv->server_depth >> 3);
        int stride = (scrpriv->win_width * bytes_per_pixel + 0x3) & ~0x3;
        unsigned char r, g, b;
        unsigned long host_pixel;

        EPHYR_DBG("Unmatched host depth scrpriv=%p\n", scrpriv);
        for (y = sy; y < sy + height; y++)
            for (x = sx; x < sx + width; x++) {
                idx = y * stride + x * bytes_per_pixel;

                switch (scrpriv->server_depth) {
                case 16:
                {
                    unsigned short pixel =
                        *(unsigned short *) (scrpriv->fb_data + idx);

                    r = ((pixel & 0xf800) >> 8);
                    g = ((pixel & 0x07e0) >> 3);
                    b = ((pixel & 0x001f) << 3);

                    host_pixel = (r << 16) | (g << 8) | (b);

                    xcb_image_put_pixel(scrpriv->ximg, x, y, host_pixel);
                    break;
                }
                case 8:
                {
                    unsigned char pixel =
                        *(unsigned char *) (scrpriv->fb_data + idx);
                    xcb_image_put_pixel(scrpriv->ximg, x, y,
                                        scrpriv->cmap[pixel]);
                    break;
                }
                default:
                    break;
                }
            }
    }

    {
        xcb_image_t *subimg = xcb_image_subimage(scrpriv->ximg, sx, sy,
                                                 width, height, 0, 0, 0);
        xcb_image_t *img = xcb_image_native(HostX.conn, subimg, 1);
        // put subimg to win at dx, dy
        if (subimg != img)
            xcb_image_destroy(img);
        xcb_image_destroy(subimg);
    }

    xcb_aux_sync(HostX.conn);
}

static void
hostx_paint_debug_rect(KdScreenInfo *screen,
                       int x, int y, int width, int height)
{
    EphyrScrPriv *scrpriv = screen->driver;
    struct timespec tspec;
    xcb_rectangle_t rect = { .x = x, .y = y, .width = width, .height = height };

    tspec.tv_sec = HostX.damage_debug_msec / (1000000);
    tspec.tv_nsec = (HostX.damage_debug_msec % 1000000) * 1000;

    EPHYR_DBG("msec: %li tv_sec %li, tv_msec %li",
              HostX.damage_debug_msec, tspec.tv_sec, tspec.tv_nsec);

    /* fprintf(stderr, "Xephyr updating: %i+%i %ix%i\n", x, y, width, height); */

    // fill rect with debug_fill_color

    /* nanosleep seems to work better than usleep for me... */
    nanosleep(&tspec, NULL);
}

Bool
hostx_load_keymap(KeySymsPtr keySyms, CARD8 *modmap, XkbControlsPtr controls)
{
    int min_keycode, max_keycode;
    int map_width;
    size_t i, j;
    int keymap_len;
    xcb_keysym_t *keymap;
    xcb_keycode_t *modifier_map;
    xcb_get_keyboard_mapping_cookie_t mapping_c;
    xcb_get_keyboard_mapping_reply_t *mapping_r;
    xcb_get_modifier_mapping_cookie_t modifier_c;
    xcb_get_modifier_mapping_reply_t *modifier_r;
    xcb_xkb_use_extension_cookie_t use_c;
    xcb_xkb_use_extension_reply_t *use_r;
    xcb_xkb_get_controls_cookie_t controls_c;
    xcb_xkb_get_controls_reply_t *controls_r;

    /* First of all, collect host X server's
     * min_keycode and max_keycode, which are
     * independent from XKB support. */
    min_keycode = xcb_get_setup(HostX.conn)->min_keycode;
    max_keycode = xcb_get_setup(HostX.conn)->max_keycode;

    EPHYR_DBG("min: %d, max: %d", min_keycode, max_keycode);

    keySyms->minKeyCode = min_keycode;
    keySyms->maxKeyCode = max_keycode;

    /* Check for XKB availability in host X server */
    if (!hostx_has_extension(&xcb_xkb_id)) {
        EPHYR_LOG_ERROR("XKB extension is not supported in host X server.");
        return FALSE;
    }

    use_c = xcb_xkb_use_extension(HostX.conn,
                                  XCB_XKB_MAJOR_VERSION,
                                  XCB_XKB_MINOR_VERSION);
    use_r = xcb_xkb_use_extension_reply(HostX.conn, use_c, NULL);

    if (!use_r) {
        EPHYR_LOG_ERROR("Couldn't use XKB extension.");
        return FALSE;
    } else if (!use_r->supported) {
        EPHYR_LOG_ERROR("XKB extension is not supported in host X server.");
        free(use_r);
        return FALSE;
    }

    free(use_r);

    /* Send all needed XCB requests at once,
     * and process the replies as needed. */
    mapping_c = xcb_get_keyboard_mapping(HostX.conn,
                                         min_keycode,
                                         max_keycode - min_keycode + 1);
    modifier_c = xcb_get_modifier_mapping(HostX.conn);
    controls_c = xcb_xkb_get_controls(HostX.conn,
                                      XCB_XKB_ID_USE_CORE_KBD);

    mapping_r = xcb_get_keyboard_mapping_reply(HostX.conn,
                                               mapping_c,
                                               NULL);

    if (!mapping_r) {
        EPHYR_LOG_ERROR("xcb_get_keyboard_mapping_reply() failed.");
        return FALSE;
    }

    map_width = mapping_r->keysyms_per_keycode;
    keymap = xcb_get_keyboard_mapping_keysyms(mapping_r);
    keymap_len = xcb_get_keyboard_mapping_keysyms_length(mapping_r);

    keySyms->mapWidth = map_width;
    keySyms->map = calloc(keymap_len, sizeof(KeySym));

    if (!keySyms->map) {
        EPHYR_LOG_ERROR("Failed to allocate KeySym map.");
        free(mapping_r);
        return FALSE;
    }

    for (i = 0; i < keymap_len; i++) {
        keySyms->map[i] = keymap[i];
    }

    free(mapping_r);

    modifier_r = xcb_get_modifier_mapping_reply(HostX.conn,
                                                modifier_c,
                                                NULL);

    if (!modifier_r) {
        EPHYR_LOG_ERROR("xcb_get_modifier_mapping_reply() failed.");
        return FALSE;
    }

    modifier_map = xcb_get_modifier_mapping_keycodes(modifier_r);
    memset(modmap, 0, sizeof(CARD8) * MAP_LENGTH);

    for (j = 0; j < 8; j++) {
        for (i = 0; i < modifier_r->keycodes_per_modifier; i++) {
            CARD8 keycode;

            if ((keycode = modifier_map[j * modifier_r->keycodes_per_modifier + i])) {
                modmap[keycode] |= 1 << j;
            }
        }
    }

    free(modifier_r);

    controls_r = xcb_xkb_get_controls_reply(HostX.conn,
                                            controls_c,
                                            NULL);

    if (!controls_r) {
        EPHYR_LOG_ERROR("xcb_xkb_get_controls_reply() failed.");
        return FALSE;
    }

    controls->enabled_ctrls = controls_r->enabledControls;

    for (i = 0; i < XkbPerKeyBitArraySize; i++) {
        controls->per_key_repeat[i] = controls_r->perKeyRepeat[i];
    }

    free(controls_r);

    return TRUE;
}

void
hostx_size_set_from_configure(Bool ss)
{
    HostX.size_set_from_configure = ss;
}

xcb_connection_t *
hostx_get_xcbconn(void)
{
    return HostX.conn;
}

xcb_generic_event_t *
hostx_get_event(Bool queued_only)
{
    xcb_generic_event_t *xev;

    if (HostX.saved_event) {
        xev = HostX.saved_event;
        HostX.saved_event = NULL;
    } else {
        if (queued_only)
            xev = xcb_poll_for_queued_event(HostX.conn);
        else
            xev = xcb_poll_for_event(HostX.conn);
    }
    return xev;
}

Bool
hostx_has_queued_event(void)
{
    if (!HostX.saved_event)
        HostX.saved_event = xcb_poll_for_queued_event(HostX.conn);
    return HostX.saved_event != NULL;
}

int
hostx_get_screen(void)
{
    return HostX.screen;
}

int
hostx_get_fd(void)
{
    return xcb_get_file_descriptor(HostX.conn);
}

#ifdef GLAMOR
Bool
ephyr_glamor_init(ScreenPtr screen)
{
    KdScreenPriv(screen);
    KdScreenInfo *kd_screen = pScreenPriv->screen;
    EphyrScrPriv *scrpriv = kd_screen->driver;

    scrpriv->glamor = ephyr_glamor_glx_screen_init(scrpriv->win);
    ephyr_glamor_set_window_size(scrpriv->glamor,
                                 scrpriv->win_width, scrpriv->win_height);

    if (!glamor_init(screen, 0)) {
        FatalError("Failed to initialize glamor\n");
        return FALSE;
    }

    return TRUE;
}

static int
ephyrSetPixmapVisitWindow(WindowPtr window, void *data)
{
    ScreenPtr screen = window->drawable.pScreen;

    if (screen->GetWindowPixmap(window) == data) {
        screen->SetWindowPixmap(window, screen->GetScreenPixmap(screen));
        return WT_WALKCHILDREN;
    }
    return WT_DONTWALKCHILDREN;
}

Bool
ephyr_glamor_create_screen_resources(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *kd_screen = pScreenPriv->screen;
    EphyrScrPriv *scrpriv = kd_screen->driver;
    PixmapPtr old_screen_pixmap, screen_pixmap;
    uint32_t tex;

    if (!ephyr_glamor)
        return TRUE;

    /* kdrive's fbSetupScreen() told mi to have
     * miCreateScreenResources() (which is called before this) make a
     * scratch pixmap wrapping ephyr-glamor's NULL
     * KdScreenInfo->fb.framebuffer.
     *
     * We want a real (texture-based) screen pixmap at this point.
     * This is what glamor will render into, and we'll then texture
     * out of that into the host's window to present the results.
     *
     * Thus, delete the current screen pixmap, and put a fresh one in.
     */
    old_screen_pixmap = pScreen->GetScreenPixmap(pScreen);
    pScreen->DestroyPixmap(old_screen_pixmap);

    screen_pixmap = pScreen->CreatePixmap(pScreen,
                                          pScreen->width,
                                          pScreen->height,
                                          pScreen->rootDepth,
                                          GLAMOR_CREATE_NO_LARGE);
    if (!screen_pixmap)
        return FALSE;

    pScreen->SetScreenPixmap(screen_pixmap);
    if (pScreen->root && pScreen->SetWindowPixmap)
        TraverseTree(pScreen->root, ephyrSetPixmapVisitWindow, old_screen_pixmap);

    /* Tell the GLX code what to GL texture to read from. */
    tex = glamor_get_pixmap_texture(screen_pixmap);
    if (!tex)
        return FALSE;

    ephyr_glamor_set_texture(scrpriv->glamor, tex);

    return TRUE;
}

void
ephyr_glamor_enable(ScreenPtr screen)
{
}

void
ephyr_glamor_disable(ScreenPtr screen)
{
}

void
ephyr_glamor_fini(ScreenPtr screen)
{
    KdScreenPriv(screen);
    KdScreenInfo *kd_screen = pScreenPriv->screen;
    EphyrScrPriv *scrpriv = kd_screen->driver;

    glamor_fini(screen);
    ephyr_glamor_glx_screen_fini(scrpriv->glamor);
    scrpriv->glamor = NULL;
}
#endif
