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

#ifdef GLAMOR
#include <epoxy/gl.h>
#include "glamor.h"
#include "xboat_glamor_egl.h"
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
    image->depth = 24;
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
    ARect rect;
    rect.left = dst_x;
    rect.top = dst_y;
    rect.right = dst_x + width;
    rect.bottom = dst_y + height;
    ANativeWindow_lock(window, &buffer, &rect);
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
    ARect rect;
    rect.left = x;
    rect.top = y;
    rect.right = x + width;
    rect.bottom = y + height;
    ANativeWindow_lock(window, &buffer, &rect);
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

struct XboatHostBoatVars {
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

    uint32_t debug_fill_color;
};

/* memset ( missing> ) instead of below  */
/*static XboatHostBoatVars HostBoat = { "?", 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};*/
static struct XboatHostBoatVars HostBoat;

static int HostBoatWantDamageDebug = 0;

extern Bool XboatWantResize;

Bool xboat_glamor = FALSE;
extern Bool xboat_glamor_skip_present;

#define host_depth_matches_server(_vars) (HostBoat.depth == (_vars)->server_depth)

int
hostboat_want_screen_geometry(KdScreenInfo *screen, int *width, int *height, int *x, int *y)
{
    XboatScrPriv *scrpriv = screen->driver;

    if (scrpriv && HostBoat.use_fullscreen == TRUE) {
        *x = scrpriv->win_x;
        *y = scrpriv->win_y;
        *width = scrpriv->win_width;
        *height = scrpriv->win_height;
        return 1;
    }

    return 0;
}

void
hostboat_add_screen(KdScreenInfo *screen, int screen_num)
{
    XboatScrPriv *scrpriv = screen->driver;
    int index = HostBoat.n_screens;

    HostBoat.n_screens += 1;
    HostBoat.screens = reallocarray(HostBoat.screens,
                                 HostBoat.n_screens, sizeof(HostBoat.screens[0]));
    HostBoat.screens[index] = screen;

    scrpriv->screen = screen;
}

void
hostboat_set_display_name(char *name)
{
    HostBoat.server_dpy_name = strdup(name);
}

void
hostboat_set_screen_number(KdScreenInfo *screen, int number)
{
    XboatScrPriv *scrpriv = screen->driver;

    if (scrpriv) {
        scrpriv->mynum = number;
        hostboat_set_win_title(screen, "");
    }
}

void
hostboat_set_win_title(KdScreenInfo *screen, const char *extra_text)
{
    XboatScrPriv *scrpriv = screen->driver;

    if (!scrpriv)
        return;

    {
#define BUF_LEN 256
        char buf[BUF_LEN + 1];

        memset(buf, 0, BUF_LEN + 1);
        snprintf(buf, BUF_LEN, "Xboat on %s.%d %s",
                 HostBoat.server_dpy_name ? HostBoat.server_dpy_name : ":0",
                 scrpriv->mynum, (extra_text != NULL) ? extra_text : "");

        // boatSetTitle(buf);
    }
}

void
hostboat_use_sw_cursor(void)
{
    HostBoat.use_sw_cursor = TRUE;
}

void
hostboat_use_fullscreen(void)
{
    HostBoat.use_fullscreen = TRUE;
}

int
hostboat_want_fullscreen(void)
{
    return HostBoat.use_fullscreen;
}

static void
hostboat_toggle_damage_debug(void)
{
    HostBoatWantDamageDebug ^= 1;
}

void
hostboat_handle_signal(int signum)
{
    hostboat_toggle_damage_debug();
    XBOAT_DBG("Signal caught. Damage Debug:%i\n", HostBoatWantDamageDebug);
}

#ifdef __SUNPRO_C
/* prevent "Function has no return statement" error for x_io_error_handler */
#pragma does_not_return(exit)
#endif

int
hostboat_init(void)
{
    int index;

    XBOAT_DBG("mark");
#ifdef GLAMOR
    if (xboat_glamor)
        xboat_glamor_connect();
#endif

    HostBoat.winroot = boatGetNativeWindow();
    HostBoat.depth = 24; // only support 32bit-RGBA8888
#ifdef GLAMOR
    if (xboat_glamor) {
        xboat_glamor_get_visual();
    }
#endif
    HostBoat.visual = &xboat_default_visual;

    for (index = 0; index < HostBoat.n_screens; index++) {
        KdScreenInfo *screen = HostBoat.screens[index];
        XboatScrPriv *scrpriv = screen->driver;

        // Xboat: Boat has only one window (root window)
        //        just use it
        scrpriv->win = HostBoat.winroot;
        scrpriv->server_depth = HostBoat.depth;
        scrpriv->ximg = NULL;
        scrpriv->win_x = 0;
        scrpriv->win_y = 0;

        {
            hostboat_set_win_title(screen,
                                   "(touch Grab to grab mouse and keyboard)");

            if (HostBoat.use_fullscreen) {
                scrpriv->win_width  = ANativeWindow_getWidth(scrpriv->win);
                scrpriv->win_height = ANativeWindow_getHeight(scrpriv->win);
            }
        }
    }

    HostBoat.debug_fill_color = 0x000000ff; // red for RGBA8888

    {
        CursorVisible = TRUE;
        /* Ditch the cursor, we provide our 'own' */
    }

    /* Setup the pause time between paints when debugging updates */

    HostBoat.damage_debug_msec = 20000;    /* 1/50 th of a second */

    if (getenv("XBOAT_PAUSE")) {
        HostBoat.damage_debug_msec = strtol(getenv("XBOAT_PAUSE"), NULL, 0);
        XBOAT_DBG("pause is %li\n", HostBoat.damage_debug_msec);
    }

    return 1;
}

int
hostboat_get_depth(void)
{
    return HostBoat.depth;
}

int
hostboat_get_server_depth(KdScreenInfo *screen)
{
    XboatScrPriv *scrpriv = screen->driver;

    return scrpriv ? scrpriv->server_depth : 0;
}

void
hostboat_get_visual_masks(KdScreenInfo *screen,
                       CARD32 *rmsk, CARD32 *gmsk, CARD32 *bmsk)
{
    XboatScrPriv *scrpriv = screen->driver;

    if (!scrpriv)
        return;

    if (host_depth_matches_server(scrpriv)) {
        *rmsk = HostBoat.visual->red_mask;
        *gmsk = HostBoat.visual->green_mask;
        *bmsk = HostBoat.visual->blue_mask;
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
hostboat_calculate_color_shift(unsigned long mask)
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
hostboat_set_cmap_entry(ScreenPtr pScreen, unsigned char idx,
                     unsigned char r, unsigned char g, unsigned char b)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
/* need to calculate the shifts for RGB because server could be BGR. */
/* XXX Not sure if this is correct for 8 on 16, but this works for 8 on 24.*/
    static int rshift, bshift, gshift = 0;
    static int first_time = 1;

    if (first_time) {
        first_time = 0;
        rshift = hostboat_calculate_color_shift(HostBoat.visual->red_mask);
        gshift = hostboat_calculate_color_shift(HostBoat.visual->green_mask);
        bshift = hostboat_calculate_color_shift(HostBoat.visual->blue_mask);
    }
    scrpriv->cmap[idx] = ((r << rshift) & HostBoat.visual->red_mask) |
        ((g << gshift) & HostBoat.visual->green_mask) |
        ((b << bshift) & HostBoat.visual->blue_mask);
}

/**
 * hostboat_screen_init creates the xboat_image that will contain the front buffer of
 * the xboat screen, and possibly offscreen memory.
 *
 * @param width width of the screen
 * @param height height of the screen
 * @param buffer_height  height of the rectangle to be allocated.
 *
 * hostboat_screen_init() creates an xboat_image.
 * buffer_height can be used to create a larger offscreen buffer, which is used
 * by fakexa for storing offscreen pixmap data.
 */
void *
hostboat_screen_init(KdScreenInfo *screen,
                  int x, int y,
                  int width, int height, int buffer_height,
                  int *bytes_per_line, int *bits_per_pixel)
{
    XboatScrPriv *scrpriv = screen->driver;

    if (!scrpriv) {
        fprintf(stderr, "%s: Error in accessing hostboat data\n", __func__);
        exit(1);
    }

    XBOAT_DBG("host_screen=%p x=%d, y=%d, wxh=%dx%d, buffer_height=%d",
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

    if (!xboat_glamor) {
        XBOAT_DBG("Creating image %dx%d for screen scrpriv=%p\n",
                  width, buffer_height, scrpriv);
        scrpriv->ximg = xboat_image_create(width,
                                           buffer_height);

        /* Match server byte order so that the image can be converted to
         * the native byte order by xcb_image_put() before drawing */
        if (host_depth_matches_server(scrpriv))
            scrpriv->ximg->byte_order = IMAGE_BYTE_ORDER;

        scrpriv->ximg->data =
            xallocarray(scrpriv->ximg->stride, buffer_height);
    }

    if (!HostBoat.size_set_from_configure)
    {
        // Xboat: Boat does not support resize window from program
        XBOAT_DBG("WARNING: Window resize request ignored: %s:%d %s\n",
                  __FILE__, __LINE__, __func__);
    }

    if (!XboatWantResize) {
        /* Ask the WM to keep our size static */
        // Maybe we should disable MultiWindow mode on Android?
    }

    scrpriv->win_width = width;
    scrpriv->win_height = height;
    scrpriv->win_x = x;
    scrpriv->win_y = y;

#ifdef GLAMOR
    if (xboat_glamor) {
        *bytes_per_line = 0;
        xboat_glamor_set_window_size(scrpriv->glamor,
                                     scrpriv->win_width, scrpriv->win_height);
        return NULL;
    } else
#endif
    if (host_depth_matches_server(scrpriv)) {
        *bytes_per_line = scrpriv->ximg->stride;
        *bits_per_pixel = scrpriv->ximg->bpp;

        XBOAT_DBG("Host matches server");
        return scrpriv->ximg->data;
    }
    else {
        int bytes_per_pixel = scrpriv->server_depth >> 3;
        int stride = (width * bytes_per_pixel + 0x3) & ~0x3;

        *bytes_per_line = stride;
        *bits_per_pixel = scrpriv->server_depth;

        XBOAT_DBG("server bpp %i", bytes_per_pixel);
        scrpriv->fb_data = xallocarray (stride, buffer_height);
        return scrpriv->fb_data;
    }
}

static void hostboat_paint_debug_rect(KdScreenInfo *screen,
                                   int x, int y, int width, int height);

void
hostboat_paint_rect(KdScreenInfo *screen,
                 int sx, int sy, int dx, int dy, int width, int height)
{
    XboatScrPriv *scrpriv = screen->driver;

    XBOAT_DBG("painting in screen %d\n", scrpriv->mynum);

#ifdef GLAMOR
    if (xboat_glamor) {
        BoxRec box;
        RegionRec region;

        box.x1 = dx;
        box.y1 = dy;
        box.x2 = dx + width;
        box.y2 = dy + height;

        RegionInit(&region, &box, 1);
        xboat_glamor_damage_redisplay(scrpriv->glamor, &region);
        RegionUninit(&region);
        return;
    }
#endif

    /*
     *  Copy the image data updated by the shadow layer
     *  on to the window
     */

    if (HostBoatWantDamageDebug) {
        hostboat_paint_debug_rect(screen, dx, dy, width, height);
    }

    /*
     * If the depth of the xboat server is less than that of the host,
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

        XBOAT_DBG("Unmatched host depth scrpriv=%p\n", scrpriv);
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
hostboat_paint_debug_rect(KdScreenInfo *screen,
                       int x, int y, int width, int height)
{
    XboatScrPriv *scrpriv = screen->driver;
    struct timespec tspec;

    tspec.tv_sec = HostBoat.damage_debug_msec / (1000000);
    tspec.tv_nsec = (HostBoat.damage_debug_msec % 1000000) * 1000;

    XBOAT_DBG("msec: %li tv_sec %li, tv_msec %li",
              HostBoat.damage_debug_msec, tspec.tv_sec, tspec.tv_nsec);

    /* fprintf(stderr, "Xboat updating: %i+%i %ix%i\n", x, y, width, height); */

    xboat_fill_rectangle(HostBoat.debug_fill_color, scrpriv->win, x, y, width, height);

    /* nanosleep seems to work better than usleep for me... */
    nanosleep(&tspec, NULL);
}

Bool
hostboat_load_keymap(KeySymsPtr keySyms, CARD8 *modmap, XkbControlsPtr controls)
{
    int min_keycode, max_keycode;

    /* First of all, collect host Boat's
     * min_keycode and max_keycode. */
    min_keycode = BOAT_MIN_SCANCODE;
    max_keycode = BOAT_MAX_SCANCODE;

    XBOAT_DBG("min: %d, max: %d", min_keycode, max_keycode);

    keySyms->minKeyCode = min_keycode;
    keySyms->maxKeyCode = max_keycode;

    // TODO: maybe we need to build a standard keymap for Boat
    return FALSE;
}

void
hostboat_size_set_from_configure(Bool ss)
{
    HostBoat.size_set_from_configure = ss;
}

BoatEvent *
hostboat_get_event(Bool queued_only)
{
    BoatEvent *xev = NULL;

    if (HostBoat.saved_event) {
        xev = HostBoat.saved_event;
        HostBoat.saved_event = NULL;
    } else {
        if (queued_only) {
            xev = NULL; // xcb_poll_for_queued_event(HostBoat.conn);
        } else {
            xev = malloc(sizeof(BoatEvent));
            if (!boatWaitForEvent(0) || !boatPollEvent(xev)) {
                free(xev);
                xev = NULL;
            }
        }
    }
    return xev;
}

Bool
hostboat_has_queued_event(void)
{
    if (!HostBoat.saved_event)
        HostBoat.saved_event = NULL; // xcb_poll_for_queued_event(HostBoat.conn);
    return HostBoat.saved_event != NULL;
}

int
hostboat_get_fd(void)
{
    return boatGetEventFd();
}

#ifdef GLAMOR
Bool
xboat_glamor_init(ScreenPtr screen)
{
    KdScreenPriv(screen);
    KdScreenInfo *kd_screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = kd_screen->driver;

    scrpriv->glamor = xboat_glamor_egl_screen_init(scrpriv->win);
    xboat_glamor_set_window_size(scrpriv->glamor,
                                 scrpriv->win_width, scrpriv->win_height);

    if (!glamor_init(screen, GLAMOR_USE_EGL_SCREEN)) {
        FatalError("Failed to initialize glamor\n");
        return FALSE;
    }

    return TRUE;
}

static int
xboatSetPixmapVisitWindow(WindowPtr window, void *data)
{
    ScreenPtr screen = window->drawable.pScreen;

    if (screen->GetWindowPixmap(window) == data) {
        screen->SetWindowPixmap(window, screen->GetScreenPixmap(screen));
        return WT_WALKCHILDREN;
    }
    return WT_DONTWALKCHILDREN;
}

Bool
xboat_glamor_create_screen_resources(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *kd_screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = kd_screen->driver;
    PixmapPtr old_screen_pixmap, screen_pixmap;
    uint32_t tex;

    if (!xboat_glamor)
        return TRUE;

    /* kdrive's fbSetupScreen() told mi to have
     * miCreateScreenResources() (which is called before this) make a
     * scratch pixmap wrapping xboat-glamor's NULL
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
        TraverseTree(pScreen->root, xboatSetPixmapVisitWindow, old_screen_pixmap);

    /* Tell the GLX code what to GL texture to read from. */
    tex = glamor_get_pixmap_texture(screen_pixmap);
    if (!tex)
        return FALSE;

    xboat_glamor_set_texture(scrpriv->glamor, tex);

    return TRUE;
}

void
xboat_glamor_enable(ScreenPtr screen)
{
}

void
xboat_glamor_disable(ScreenPtr screen)
{
}

void
xboat_glamor_fini(ScreenPtr screen)
{
    KdScreenPriv(screen);
    KdScreenInfo *kd_screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = kd_screen->driver;

    glamor_fini(screen);
    xboat_glamor_egl_screen_fini(scrpriv->glamor);
    scrpriv->glamor = NULL;
}
#endif
