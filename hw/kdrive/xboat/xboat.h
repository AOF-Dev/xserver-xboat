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

#ifndef _XBOAT_H_
#define _XBOAT_H_
#include <stdio.h>
#include <unistd.h>
#include <libgen.h>

#include "os.h"                 /* for OsSignal() */
#include "kdrive.h"
#include "hostboat.h"
#include "exa.h"

#ifdef RANDR
#include "randrstr.h"
#endif

#include "damage.h"

typedef struct _xboatPriv {
    CARD8 *base;
    int bytes_per_line;
} XboatPriv;

typedef struct _xboatFakexaPriv {
    ExaDriverPtr exa;
    Bool is_synced;

    /* The following are arguments and other information from Prepare* calls
     * which are stored for use in the inner calls.
     */
    int op;
    PicturePtr pSrcPicture, pMaskPicture, pDstPicture;
    void *saved_ptrs[3];
    PixmapPtr pDst, pSrc, pMask;
    GCPtr pGC;
} XboatFakexaPriv;

typedef struct _xboatScrPriv {
    /* xboat server info */
    Rotation randr;
    Bool shadow;
    DamagePtr pDamage;
    XboatFakexaPriv *fakexa;

    /* Host X window info */
    ANativeWindow* win;
    ANativeWindow* peer_win;            /* Used for GL; should be at most one */
    struct xboat_image_t *ximg;
    int win_x, win_y;
    int win_width, win_height;
    int server_depth;
    unsigned char *fb_data;     /* only used when host bpp != server bpp */

    KdScreenInfo *screen;
    int mynum;                  /* Screen number */
    unsigned long cmap[256];

    ScreenBlockHandlerProcPtr   BlockHandler;

    /**
     * Per-screen EGL-using state for glamor (private to
     * xboat_glamor_egl.c)
     */
    struct xboat_glamor *glamor;
} XboatScrPriv;

extern KdCardFuncs xboatFuncs;
extern KdKeyboardInfo *xboatKbd;
extern KdPointerInfo *xboatMouse;

extern miPointerScreenFuncRec xboatPointerScreenFuncs;

Bool
 xboatInitialize(KdCardInfo * card, XboatPriv * priv);

Bool
 xboatCardInit(KdCardInfo * card);

Bool
xboatScreenInitialize(KdScreenInfo *screen);

Bool
 xboatInitScreen(ScreenPtr pScreen);

Bool
 xboatFinishInitScreen(ScreenPtr pScreen);

Bool
 xboatCreateResources(ScreenPtr pScreen);

void
 xboatPreserve(KdCardInfo * card);

Bool
 xboatEnable(ScreenPtr pScreen);

Bool
 xboatDPMS(ScreenPtr pScreen, int mode);

void
 xboatDisable(ScreenPtr pScreen);

void
 xboatRestore(KdCardInfo * card);

void
 xboatScreenFini(KdScreenInfo * screen);

void
xboatCloseScreen(ScreenPtr pScreen);

void
 xboatCardFini(KdCardInfo * card);

void
 xboatGetColors(ScreenPtr pScreen, int n, xColorItem * pdefs);

void
 xboatPutColors(ScreenPtr pScreen, int n, xColorItem * pdefs);

Bool
 xboatMapFramebuffer(KdScreenInfo * screen);

void *xboatWindowLinear(ScreenPtr pScreen,
                        CARD32 row,
                        CARD32 offset, int mode, CARD32 *size, void *closure);

void
 xboatSetScreenSizes(ScreenPtr pScreen);

Bool
 xboatUnmapFramebuffer(KdScreenInfo * screen);

void
 xboatUnsetInternalDamage(ScreenPtr pScreen);

Bool
 xboatSetInternalDamage(ScreenPtr pScreen);

Bool
 xboatCreateColormap(ColormapPtr pmap);

#ifdef RANDR
Bool
 xboatRandRGetInfo(ScreenPtr pScreen, Rotation * rotations);

Bool

xboatRandRSetConfig(ScreenPtr pScreen,
                    Rotation randr, int rate, RRScreenSizePtr pSize);
Bool
 xboatRandRInit(ScreenPtr pScreen);

void
 xboatShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf);

#endif

void
 xboatUpdateModifierState(unsigned int state);

extern KdPointerDriver XboatMouseDriver;

extern KdKeyboardDriver XboatKeyboardDriver;

extern int xboatBufferHeight(KdScreenInfo * screen);

/* xboat_draw.c */

Bool
 xboatDrawInit(ScreenPtr pScreen);

void
 xboatDrawEnable(ScreenPtr pScreen);

void
 xboatDrawDisable(ScreenPtr pScreen);

void
 xboatDrawFini(ScreenPtr pScreen);

/* hostboat.c glamor support */
Bool xboat_glamor_init(ScreenPtr pScreen);
Bool xboat_glamor_create_screen_resources(ScreenPtr pScreen);
void xboat_glamor_enable(ScreenPtr pScreen);
void xboat_glamor_disable(ScreenPtr pScreen);
void xboat_glamor_fini(ScreenPtr pScreen);
void xboat_glamor_host_paint_rect(ScreenPtr pScreen);

/*ephyvideo.c*/

Bool xboatInitVideo(ScreenPtr pScreen);

/* xboat_glamor_xv.c */
#ifdef GLAMOR
void xboat_glamor_xv_init(ScreenPtr screen);
#else /* !GLAMOR */
static inline void
xboat_glamor_xv_init(ScreenPtr screen)
{
}
#endif /* !GLAMOR */

#endif
