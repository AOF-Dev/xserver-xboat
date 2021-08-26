/*
 * Copyright © 2006 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "xboat.h"
#include "exa_priv.h"
#include "fbpict.h"

#define XBOAT_TRACE_DRAW 0

#if XBOAT_TRACE_DRAW
#define TRACE_DRAW() ErrorF("%s\n", __FUNCTION__);
#else
#define TRACE_DRAW() do { } while (0)
#endif

/* Use some oddball alignments, to expose issues in alignment handling in EXA. */
#define XBOAT_OFFSET_ALIGN	24
#define XBOAT_PITCH_ALIGN	24

#define XBOAT_OFFSCREEN_SIZE	(16 * 1024 * 1024)
#define XBOAT_OFFSCREEN_BASE	(1 * 1024 * 1024)

/**
 * Forces a real devPrivate.ptr for hidden pixmaps, so that we can call down to
 * fb functions.
 */
static void
xboatPreparePipelinedAccess(PixmapPtr pPix, int index)
{
    KdScreenPriv(pPix->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    assert(fakexa->saved_ptrs[index] == NULL);
    fakexa->saved_ptrs[index] = pPix->devPrivate.ptr;

    if (pPix->devPrivate.ptr != NULL)
        return;

    pPix->devPrivate.ptr = fakexa->exa->memoryBase + exaGetPixmapOffset(pPix);
}

/**
 * Restores the original devPrivate.ptr of the pixmap from before we messed with
 * it.
 */
static void
xboatFinishPipelinedAccess(PixmapPtr pPix, int index)
{
    KdScreenPriv(pPix->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    pPix->devPrivate.ptr = fakexa->saved_ptrs[index];
    fakexa->saved_ptrs[index] = NULL;
}

/**
 * Sets up a scratch GC for fbFill, and saves other parameters for the
 * xboatSolid implementation.
 */
static Bool
xboatPrepareSolid(PixmapPtr pPix, int alu, Pixel pm, Pixel fg)
{
    ScreenPtr pScreen = pPix->drawable.pScreen;

    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;
    ChangeGCVal tmpval[3];

    xboatPreparePipelinedAccess(pPix, EXA_PREPARE_DEST);

    fakexa->pDst = pPix;
    fakexa->pGC = GetScratchGC(pPix->drawable.depth, pScreen);

    tmpval[0].val = alu;
    tmpval[1].val = pm;
    tmpval[2].val = fg;
    ChangeGC(NullClient, fakexa->pGC, GCFunction | GCPlaneMask | GCForeground,
             tmpval);

    ValidateGC(&pPix->drawable, fakexa->pGC);

    TRACE_DRAW();

    return TRUE;
}

/**
 * Does an fbFill of the rectangle to be drawn.
 */
static void
xboatSolid(PixmapPtr pPix, int x1, int y1, int x2, int y2)
{
    ScreenPtr pScreen = pPix->drawable.pScreen;

    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    fbFill(&fakexa->pDst->drawable, fakexa->pGC, x1, y1, x2 - x1, y2 - y1);
}

/**
 * Cleans up the scratch GC created in xboatPrepareSolid.
 */
static void
xboatDoneSolid(PixmapPtr pPix)
{
    ScreenPtr pScreen = pPix->drawable.pScreen;

    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    FreeScratchGC(fakexa->pGC);

    xboatFinishPipelinedAccess(pPix, EXA_PREPARE_DEST);
}

/**
 * Sets up a scratch GC for fbCopyArea, and saves other parameters for the
 * xboatCopy implementation.
 */
static Bool
xboatPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int dx, int dy, int alu,
                 Pixel pm)
{
    ScreenPtr pScreen = pDst->drawable.pScreen;

    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;
    ChangeGCVal tmpval[2];

    xboatPreparePipelinedAccess(pDst, EXA_PREPARE_DEST);
    xboatPreparePipelinedAccess(pSrc, EXA_PREPARE_SRC);

    fakexa->pSrc = pSrc;
    fakexa->pDst = pDst;
    fakexa->pGC = GetScratchGC(pDst->drawable.depth, pScreen);

    tmpval[0].val = alu;
    tmpval[1].val = pm;
    ChangeGC(NullClient, fakexa->pGC, GCFunction | GCPlaneMask, tmpval);

    ValidateGC(&pDst->drawable, fakexa->pGC);

    TRACE_DRAW();

    return TRUE;
}

/**
 * Does an fbCopyArea to take care of the requested copy.
 */
static void
xboatCopy(PixmapPtr pDst, int srcX, int srcY, int dstX, int dstY, int w, int h)
{
    ScreenPtr pScreen = pDst->drawable.pScreen;

    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    fbCopyArea(&fakexa->pSrc->drawable, &fakexa->pDst->drawable, fakexa->pGC,
               srcX, srcY, w, h, dstX, dstY);
}

/**
 * Cleans up the scratch GC created in xboatPrepareCopy.
 */
static void
xboatDoneCopy(PixmapPtr pDst)
{
    ScreenPtr pScreen = pDst->drawable.pScreen;

    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    FreeScratchGC(fakexa->pGC);

    xboatFinishPipelinedAccess(fakexa->pSrc, EXA_PREPARE_SRC);
    xboatFinishPipelinedAccess(fakexa->pDst, EXA_PREPARE_DEST);
}

/**
 * Reports that we can always accelerate the given operation.  This may not be
 * desirable from an EXA testing standpoint -- testing the fallback paths would
 * be useful, too.
 */
static Bool
xboatCheckComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
                    PicturePtr pDstPicture)
{
    /* Exercise the component alpha helper, so fail on this case like a normal
     * driver
     */
    if (pMaskPicture && pMaskPicture->componentAlpha && op == PictOpOver)
        return FALSE;

    return TRUE;
}

/**
 * Saves off the parameters for xboatComposite.
 */
static Bool
xboatPrepareComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
                      PicturePtr pDstPicture, PixmapPtr pSrc, PixmapPtr pMask,
                      PixmapPtr pDst)
{
    KdScreenPriv(pDst->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    xboatPreparePipelinedAccess(pDst, EXA_PREPARE_DEST);
    if (pSrc != NULL)
        xboatPreparePipelinedAccess(pSrc, EXA_PREPARE_SRC);
    if (pMask != NULL)
        xboatPreparePipelinedAccess(pMask, EXA_PREPARE_MASK);

    fakexa->op = op;
    fakexa->pSrcPicture = pSrcPicture;
    fakexa->pMaskPicture = pMaskPicture;
    fakexa->pDstPicture = pDstPicture;
    fakexa->pSrc = pSrc;
    fakexa->pMask = pMask;
    fakexa->pDst = pDst;

    TRACE_DRAW();

    return TRUE;
}

/**
 * Does an fbComposite to complete the requested drawing operation.
 */
static void
xboatComposite(PixmapPtr pDst, int srcX, int srcY, int maskX, int maskY,
               int dstX, int dstY, int w, int h)
{
    KdScreenPriv(pDst->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    fbComposite(fakexa->op, fakexa->pSrcPicture, fakexa->pMaskPicture,
                fakexa->pDstPicture, srcX, srcY, maskX, maskY, dstX, dstY,
                w, h);
}

static void
xboatDoneComposite(PixmapPtr pDst)
{
    KdScreenPriv(pDst->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    if (fakexa->pMask != NULL)
        xboatFinishPipelinedAccess(fakexa->pMask, EXA_PREPARE_MASK);
    if (fakexa->pSrc != NULL)
        xboatFinishPipelinedAccess(fakexa->pSrc, EXA_PREPARE_SRC);
    xboatFinishPipelinedAccess(fakexa->pDst, EXA_PREPARE_DEST);
}

/**
 * Does fake acceleration of DownloadFromScren using memcpy.
 */
static Bool
xboatDownloadFromScreen(PixmapPtr pSrc, int x, int y, int w, int h, char *dst,
                        int dst_pitch)
{
    KdScreenPriv(pSrc->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;
    unsigned char *src;
    int src_pitch, cpp;

    if (pSrc->drawable.bitsPerPixel < 8)
        return FALSE;

    xboatPreparePipelinedAccess(pSrc, EXA_PREPARE_SRC);

    cpp = pSrc->drawable.bitsPerPixel / 8;
    src_pitch = exaGetPixmapPitch(pSrc);
    src = fakexa->exa->memoryBase + exaGetPixmapOffset(pSrc);
    src += y * src_pitch + x * cpp;

    for (; h > 0; h--) {
        memcpy(dst, src, w * cpp);
        dst += dst_pitch;
        src += src_pitch;
    }

    exaMarkSync(pSrc->drawable.pScreen);

    xboatFinishPipelinedAccess(pSrc, EXA_PREPARE_SRC);

    return TRUE;
}

/**
 * Does fake acceleration of UploadToScreen using memcpy.
 */
static Bool
xboatUploadToScreen(PixmapPtr pDst, int x, int y, int w, int h, char *src,
                    int src_pitch)
{
    KdScreenPriv(pDst->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;
    unsigned char *dst;
    int dst_pitch, cpp;

    if (pDst->drawable.bitsPerPixel < 8)
        return FALSE;

    xboatPreparePipelinedAccess(pDst, EXA_PREPARE_DEST);

    cpp = pDst->drawable.bitsPerPixel / 8;
    dst_pitch = exaGetPixmapPitch(pDst);
    dst = fakexa->exa->memoryBase + exaGetPixmapOffset(pDst);
    dst += y * dst_pitch + x * cpp;

    for (; h > 0; h--) {
        memcpy(dst, src, w * cpp);
        dst += dst_pitch;
        src += src_pitch;
    }

    exaMarkSync(pDst->drawable.pScreen);

    xboatFinishPipelinedAccess(pDst, EXA_PREPARE_DEST);

    return TRUE;
}

static Bool
xboatPrepareAccess(PixmapPtr pPix, int index)
{
    /* Make sure we don't somehow end up with a pointer that is in framebuffer
     * and hasn't been readied for us.
     */
    assert(pPix->devPrivate.ptr != NULL);

    return TRUE;
}

/**
 * In fakexa, we currently only track whether we have synced to the latest
 * "accelerated" drawing that has happened or not.  It's not used for anything
 * yet.
 */
static int
xboatMarkSync(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    fakexa->is_synced = FALSE;

    return 0;
}

/**
 * Assumes that we're waiting on the latest marker.  When EXA gets smarter and
 * starts using markers in a fine-grained way (for example, waiting on drawing
 * to required pixmaps to complete, rather than waiting for all drawing to
 * complete), we'll want to make the xboatMarkSync/xboatWaitMarker
 * implementation fine-grained as well.
 */
static void
xboatWaitMarker(ScreenPtr pScreen, int marker)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatFakexaPriv *fakexa = scrpriv->fakexa;

    fakexa->is_synced = TRUE;
}

/**
 * This function initializes EXA to use the fake acceleration implementation
 * which just falls through to software.  The purpose is to have a reliable,
 * correct driver with which to test changes to the EXA core.
 */
Bool
xboatDrawInit(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    XboatScrPriv *scrpriv = screen->driver;
    XboatPriv *priv = screen->card->driver;
    XboatFakexaPriv *fakexa;
    Bool success;

    fakexa = calloc(1, sizeof(*fakexa));
    if (fakexa == NULL)
        return FALSE;

    fakexa->exa = exaDriverAlloc();
    if (fakexa->exa == NULL) {
        free(fakexa);
        return FALSE;
    }

    fakexa->exa->memoryBase = (CARD8 *) (priv->base);
    fakexa->exa->memorySize = priv->bytes_per_line * xboatBufferHeight(screen);
    fakexa->exa->offScreenBase = priv->bytes_per_line * screen->height;

    /* Since we statically link against EXA, we shouldn't have to be smart about
     * versioning.
     */
    fakexa->exa->exa_major = 2;
    fakexa->exa->exa_minor = 0;

    fakexa->exa->PrepareSolid = xboatPrepareSolid;
    fakexa->exa->Solid = xboatSolid;
    fakexa->exa->DoneSolid = xboatDoneSolid;

    fakexa->exa->PrepareCopy = xboatPrepareCopy;
    fakexa->exa->Copy = xboatCopy;
    fakexa->exa->DoneCopy = xboatDoneCopy;

    fakexa->exa->CheckComposite = xboatCheckComposite;
    fakexa->exa->PrepareComposite = xboatPrepareComposite;
    fakexa->exa->Composite = xboatComposite;
    fakexa->exa->DoneComposite = xboatDoneComposite;

    fakexa->exa->DownloadFromScreen = xboatDownloadFromScreen;
    fakexa->exa->UploadToScreen = xboatUploadToScreen;

    fakexa->exa->MarkSync = xboatMarkSync;
    fakexa->exa->WaitMarker = xboatWaitMarker;

    fakexa->exa->PrepareAccess = xboatPrepareAccess;

    fakexa->exa->pixmapOffsetAlign = XBOAT_OFFSET_ALIGN;
    fakexa->exa->pixmapPitchAlign = XBOAT_PITCH_ALIGN;

    fakexa->exa->maxX = 1023;
    fakexa->exa->maxY = 1023;

    fakexa->exa->flags = EXA_OFFSCREEN_PIXMAPS;

    success = exaDriverInit(pScreen, fakexa->exa);
    if (success) {
        ErrorF("Initialized fake EXA acceleration\n");
        scrpriv->fakexa = fakexa;
    }
    else {
        ErrorF("Failed to initialize EXA\n");
        free(fakexa->exa);
        free(fakexa);
    }

    return success;
}

void
xboatDrawEnable(ScreenPtr pScreen)
{
}

void
xboatDrawDisable(ScreenPtr pScreen)
{
}

void
xboatDrawFini(ScreenPtr pScreen)
{
}

/**
 * exaDDXDriverInit is required by the top-level EXA module, and is used by
 * the xorg DDX to hook in its EnableDisableFB wrapper.  We don't need it, since
 * we won't be enabling/disabling the FB.
 */
void
exaDDXDriverInit(ScreenPtr pScreen)
{
    ExaScreenPriv(pScreen);

    pExaScr->migration = ExaMigrationSmart;
    pExaScr->checkDirtyCorrectness = TRUE;
}
