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

#include "hostboat.h"
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

#include <boat.h>
#ifdef GLAMOR
#include <epoxy/gl.h>
#include "glamor.h"
#include "ephyr_glamor_glx.h"
#endif
#include "xboatlog.h"
#include "xboat.h"

typedef uint32_t pixel32_t;

typedef struct xboat_image_t {
    // Width in pixels, excluding pads etc.
    uint16_t           width;
    // Height in pixels.
    uint16_t           height;
    uint8_t            format;
    // Depth in bits.
    // 1, 4, 8, 16, 24 for z format.
    uint8_t            depth;
    // Storage per pixel in bits.
    // 1, 4, 8, 16, 24, 32 for z format.
    uint8_t            bpp;
    // Component byte order for z-pixmap.
    uint8_t            byte_order;
    // Bytes per image row.
    uint32_t           stride;
    uint8_t *          data;
} xboat_image_t;

static xboat_image_t* xboat_image_create(uint16_t width, uint16_t height) {
    xboat_image_t* image = malloc(sizeof(xboat_image_t));
    image->width = width;
    image->height = height;
    image->format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    image->depth = 32;
    image->bpp = 32;
    image->stride = width * sizeof(pixel32_t);
    return image;
}

static void xboat_image_destroy(xboat_image_t *image) {
    free(image);
}

static void xboat_image_put_pixel(xboat_image_t *image, uint32_t x, uint32_t y, uint32_t pixel) {
    uint8_t* row = image->data + (y * image->stride);
    row[x << 2] = pixel;
    row[(x << 2) + 1] = pixel >> 8;
    row[(x << 2) + 2] = pixel >> 16;
    row[(x << 2) + 3] = pixel >> 24;
}

static void xboat_image_put(xboat_image_t *image, ANativeWindow* window,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t dst_x, uint32_t dst_y,
                            uint32_t width, uint32_t height) {
    ANativeWindow_Buffer buffer;
    ANativeWindow_lock(window, &buffer, NULL);
    uint32_t dst_stride = buffer.stride * sizeof(pixel32_t);
    uint32_t src_stride = image->stride;
    uint8_t* dst_line = (uint8_t*)buffer.bits + dst_y * dst_stride;
    uint8_t* src_line = image->data + src_y * src_stride;
    uint32_t bytes_per_line = width * sizeof(pixel32_t);
    dst_line += dst_x * sizeof(pixel32_t);
    src_line += src_x * sizeof(pixel32_t);
    for (uint32_t i = 0; i < height; i++) {
        memcpy(dst_line, src_line, bytes_per_line);
        dst_line += dst_stride;
        src_line += src_stride;
    }
    ANativeWindow_unlockAndPost(window);
}

static void xboat_fill_rectangle(uint32_t fill_color, ANativeWindow* window,
                                 uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height) {
    ANativeWindow_Buffer buffer;
    ANativeWindow_lock(window, &buffer, NULL);
    pixel32_t* line = (pixel32_t*)buffer.bits + y * buffer.stride;
    line += x;
    for (uint32_t j = 0; j < height; j++) {
        for (uint32_t i = 0; i < width; i++) {
            line[i] = (pixel32_t)fill_color;
        }
        line += buffer.stride;
    }
    ANativeWindow_unlockAndPost(window);
}

typedef struct xboat_visualtype_t {
    uint8_t        bits_per_rgb_value;
    uint16_t       colormap_entries;
    uint32_t       red_mask;
    uint32_t       green_mask;
    uint32_t       blue_mask;
} xboat_visualtype_t;

static xboat_visualtype_t xboat_default_visual = {
    .bits_per_rgb_value = 8,
    .colormap_entries = 256,
    .red_mask = 0x000000ff,
    .green_mask = 0x0000ff00,
    .blue_mask = 0x00ff0000,
};

struct EphyrHostXVars {
    char *server_dpy_name;
    xboat_visualtype_t *visual;
    ANativeWindow* winroot;
    BoatEvent *saved_event;
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
    int index;

    EPHYR_DBG("mark");
#ifdef GLAMOR
    if (ephyr_glamor)
        HostX.conn = ephyr_glamor_connect();
    else
#endif
        {}

    HostX.winroot = boatGetNativeWindow();
    HostX.depth = 32; // only support 32bit-RGBA8888
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
        HostX.visual = &xboat_default_visual;

    for (index = 0; index < HostX.n_screens; index++) {
        KdScreenInfo *screen = HostX.screens[index];
        EphyrScrPriv *scrpriv = screen->driver;

        // Xboat: Boat has only one window (root window)
        //        just use it
        scrpriv->win = HostBoat.winroot;
        scrpriv->server_depth = HostX.depth;
        scrpriv->ximg = NULL;
        scrpriv->win_x = 0;
        scrpriv->win_y = 0;

        {
            hostx_set_win_title(screen,
                                "(touch Grab to grab mouse and keyboard)");

            if (HostX.use_fullscreen) {
                scrpriv->win_width  = ANativeWindow_getWidth(scrpriv->win);
                scrpriv->win_height = ANativeWindow_getHeight(scrpriv->win);
            }
        }
    }

    HostX.debug_fill_color = 0x000000ff; // red for RGBA8888

    {
        CursorVisible = TRUE;
        /* Ditch the cursor, we provide our 'own' */
    }

    /* Setup the pause time between paints when debugging updates */

    HostX.damage_debug_msec = 20000;    /* 1/50 th of a second */

    if (getenv("XBOAT_PAUSE")) {
        HostX.damage_debug_msec = strtol(getenv("XBOAT_PAUSE"), NULL, 0);
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
    else if (scrpriv->server_depth == 32) {
        /* Assume 32bpp 8888 */
        *rmsk = 0x000000ff;
        *gmsk = 0x0000ff00;
        *bmsk = 0x00ff0000;
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

            xboat_image_destroy(scrpriv->ximg);
        }
    }

    if (!ephyr_glamor) {
        EPHYR_DBG("Creating image %dx%d for screen scrpriv=%p\n",
                  width, buffer_height, scrpriv);
        scrpriv->ximg = xboat_image_create(width,
                                           buffer_height,
                                           HostX.depth);

        /* Match server byte order so that the image can be converted to
         * the native byte order by xcb_image_put() before drawing */
        if (host_depth_matches_server(scrpriv))
            scrpriv->ximg->byte_order = IMAGE_BYTE_ORDER;

        scrpriv->ximg->data =
            xallocarray(scrpriv->ximg->stride, buffer_height);
    }

    if (!HostX.size_set_from_configure)
    {
        // Xboat: Boat does not support resize window from program
        EPHYR_DBG("WARNING: Window resize request ignored: %s:%d %s\n",
                  __FILE__, __LINE__, __func__);
    }

    if (!EphyrWantResize) {
        /* Ask the WM to keep our size static */
        // Maybe we should disable MultiWindow mode on Android?
    }

#ifdef GLAMOR
    if (!ephyr_glamor_skip_present)
#endif
        {}

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

                    xboat_image_put_pixel(scrpriv->ximg, x, y, host_pixel);
                    break;
                }
                case 8:
                {
                    unsigned char pixel =
                        *(unsigned char *) (scrpriv->fb_data + idx);
                    xboat_image_put_pixel(scrpriv->ximg, x, y,
                                        scrpriv->cmap[pixel]);
                    break;
                }
                default:
                    break;
                }
            }
    }

    xboat_image_put(scrpriv->ximg, scrpriv->win, sx, sy, dx, dy, width, height);
}

static void
hostx_paint_debug_rect(KdScreenInfo *screen,
                       int x, int y, int width, int height)
{
    EphyrScrPriv *scrpriv = screen->driver;
    struct timespec tspec;

    tspec.tv_sec = HostX.damage_debug_msec / (1000000);
    tspec.tv_nsec = (HostX.damage_debug_msec % 1000000) * 1000;

    EPHYR_DBG("msec: %li tv_sec %li, tv_msec %li",
              HostX.damage_debug_msec, tspec.tv_sec, tspec.tv_nsec);

    /* fprintf(stderr, "Xephyr updating: %i+%i %ix%i\n", x, y, width, height); */

    xboat_fill_rectangle(debug_fill_color, scrpriv->win, x, y, width, height);

    /* nanosleep seems to work better than usleep for me... */
    nanosleep(&tspec, NULL);
}

Bool
hostx_load_keymap(KeySymsPtr keySyms, CARD8 *modmap, XkbControlsPtr controls)
{
    int min_keycode, max_keycode;

    /* First of all, collect host Boat's
     * min_keycode and max_keycode. */
    min_keycode = BOAT_MIN_SCANCODE;
    max_keycode = BOAT_MAX_SCANCODE;

    EPHYR_DBG("min: %d, max: %d", min_keycode, max_keycode);

    keySyms->minKeyCode = min_keycode;
    keySyms->maxKeyCode = max_keycode;

    // TODO: maybe we need to build a standard keymap for Boat
    return FALSE;
}

void
hostx_size_set_from_configure(Bool ss)
{
    HostX.size_set_from_configure = ss;
}

BoatEvent *
hostx_get_event(Bool queued_only)
{
    BoatEvent *xev;

    if (HostX.saved_event) {
        xev = HostX.saved_event;
        HostX.saved_event = NULL;
    } else {
        if (queued_only) {
            xev = NULL; // xcb_poll_for_queued_event(HostX.conn);
        } else {
            xev = malloc(sizeof(BoatEvent));
            if (!boatPollEvent(xev)) {
                free(xev);
                xev = NULL;
            }
        }
    }
    return xev;
}

Bool
hostx_has_queued_event(void)
{
    if (!HostX.saved_event)
        HostX.saved_event = NULL; // xcb_poll_for_queued_event(HostX.conn);
    return HostX.saved_event != NULL;
}

int
hostx_get_fd(void)
{
    return boatGetEventFd();
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
