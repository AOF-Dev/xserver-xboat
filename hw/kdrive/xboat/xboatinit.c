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
#include "xboat.h"
#include "xboatlog.h"
#include "glx_extinit.h"

extern Bool XboatWantGrayScale;
extern Bool XboatWantResize;
extern Bool XboatWantNoHostGrab;
extern Bool kdHasPointer;
extern Bool kdHasKbd;
extern Bool xboat_glamor, xboat_glamor_gles2, xboat_glamor_skip_present;

extern Bool xboatNoXV;

void processScreenArg(const char *screen_size);

int
main(int argc, char *argv[], char *envp[])
{
    return dix_main(argc, argv, envp);
}

void
InitCard(char *name)
{
    XBOAT_DBG("mark");
    KdCardInfoAdd(&xboatFuncs, 0);
}

void
InitOutput(ScreenInfo * pScreenInfo, int argc, char **argv)
{
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    KdKeyboardInfo *ki;
    KdPointerInfo *pi;

    if (!SeatId) {
        KdAddKeyboardDriver(&XboatKeyboardDriver);
        KdAddPointerDriver(&XboatMouseDriver);

        if (!kdHasKbd) {
            ki = KdNewKeyboard();
            if (!ki)
                FatalError("Couldn't create Xboat keyboard\n");
            ki->driver = &XboatKeyboardDriver;
            KdAddKeyboard(ki);
        }

        if (!kdHasPointer) {
            pi = KdNewPointer();
            if (!pi)
                FatalError("Couldn't create Xboat pointer\n");
            pi->driver = &XboatMouseDriver;
            KdAddPointer(pi);
        }
    }

    KdInitInput();
}

void
CloseInput(void)
{
    KdCloseInput();
}

#if INPUTTHREAD
/** This function is called in Xserver/os/inputthread.c when starting
    the input thread. */
void
ddxInputThreadInit(void)
{
}
#endif

#ifdef DDXBEFORERESET
void
ddxBeforeReset(void)
{
}
#endif

void
ddxUseMsg(void)
{
    KdUseMsg();

    ErrorF("\nXboat Option Usage:\n");
    ErrorF("-sw-cursor           Render cursors in software in Xboat\n");
    ErrorF("-fullscreen          Attempt to run Xboat fullscreen\n");
    ErrorF("-grayscale           Simulate 8bit grayscale\n");
    ErrorF("-resizeable          Make Xboat windows resizeable\n");
#ifdef GLAMOR
    ErrorF("-glamor              Enable 2D acceleration using glamor\n");
    ErrorF("-glamor_gles2        Enable 2D acceleration using glamor (with GLES2 only)\n");
    ErrorF("-glamor-skip-present Skip presenting the output when using glamor (for internal testing optimization)\n");
#endif
    ErrorF
        ("-fakexa              Simulate acceleration using software rendering\n");
    ErrorF("-verbosity <level>   Set log verbosity level\n");
    ErrorF("-noxv                do not use XV\n");
    ErrorF("-no-host-grab        Disable grabbing the keyboard and mouse.\n");
    ErrorF("\n");
}

void
processScreenArg(const char *screen_size)
{
    KdCardInfo *card;

    InitCard(0);                /*Put each screen on a separate card */
    card = KdCardInfoLast();

    if (card) {
        KdScreenInfo *screen;

        screen = KdScreenInfoAdd(card);
        KdParseScreen(screen, screen_size);
        screen->driver = calloc(1, sizeof(XboatScrPriv));
        if (!screen->driver)
            FatalError("Couldn't alloc screen private\n");

        XBOAT_DBG("screen number:%d\n", screen->mynum);
        hostboat_add_screen(screen, screen->mynum);
    }
    else {
        ErrorF("No matching card found!\n");
    }
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
    XBOAT_DBG("mark argv[%d]='%s'", i, argv[i]);

    if (!strcmp(argv[i], "-screen")) {
        if ((i + 1) < argc) {
            processScreenArg(argv[i + 1]);
            return 2;
        }

        UseMsg();
        exit(1);
    }
    else if (!strcmp(argv[i], "-sw-cursor")) {
        hostboat_use_sw_cursor();
        return 1;
    }
    else if (!strcmp(argv[i], "-fullscreen")) {
        hostboat_use_fullscreen();
        return 1;
    }
    else if (!strcmp(argv[i], "-grayscale")) {
        XboatWantGrayScale = 1;
        return 1;
    }
    else if (!strcmp(argv[i], "-resizeable")) {
        XboatWantResize = 1;
        return 1;
    }
#ifdef GLAMOR
    else if (!strcmp (argv[i], "-glamor")) {
        xboat_glamor = TRUE;
        xboatFuncs.initAccel = xboat_glamor_init;
        xboatFuncs.enableAccel = xboat_glamor_enable;
        xboatFuncs.disableAccel = xboat_glamor_disable;
        xboatFuncs.finiAccel = xboat_glamor_fini;
        return 1;
    }
    else if (!strcmp (argv[i], "-glamor_gles2")) {
        xboat_glamor = TRUE;
        xboat_glamor_gles2 = TRUE;
        xboatFuncs.initAccel = xboat_glamor_init;
        xboatFuncs.enableAccel = xboat_glamor_enable;
        xboatFuncs.disableAccel = xboat_glamor_disable;
        xboatFuncs.finiAccel = xboat_glamor_fini;
        return 1;
    }
    else if (!strcmp (argv[i], "-glamor-skip-present")) {
        xboat_glamor_skip_present = TRUE;
        return 1;
    }
#endif
    else if (!strcmp(argv[i], "-fakexa")) {
        xboatFuncs.initAccel = xboatDrawInit;
        xboatFuncs.enableAccel = xboatDrawEnable;
        xboatFuncs.disableAccel = xboatDrawDisable;
        xboatFuncs.finiAccel = xboatDrawFini;
        return 1;
    }
    else if (!strcmp(argv[i], "-verbosity")) {
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            int verbosity = atoi(argv[i + 1]);

            LogSetParameter(XLOG_VERBOSITY, verbosity);
            XBOAT_LOG("set verbosiry to %d\n", verbosity);
            return 2;
        }
        else {
            UseMsg();
            exit(1);
        }
    }
    else if (!strcmp(argv[i], "-noxv")) {
        xboatNoXV = TRUE;
        XBOAT_LOG("no XVideo enabled\n");
        return 1;
    }
    else if (argv[i][0] == ':') {
        hostboat_set_display_name(argv[i]);
    }
    else if (!strcmp(argv[i], "-no-host-grab")) {
        XboatWantNoHostGrab = 1;
        return 1;
    }
    else if (!strcmp(argv[i], "-sharevts") ||
             !strcmp(argv[i], "-novtswitch")) {
        return 1;
    }
    else if (!strcmp(argv[i], "-layout")) {
        return 2;
    }

    return KdProcessArgument(argc, argv, i);
}

void
OsVendorInit(void)
{
    XBOAT_DBG("mark");

    if (SeatId)
        hostboat_use_sw_cursor();

    if (serverGeneration == 1) {
        if (!KdCardInfoLast()) {
            processScreenArg("640x480", NULL);
        }
        hostboat_init();
    }
}

KdCardFuncs xboatFuncs = {
    xboatCardInit,              /* cardinit */
    xboatScreenInitialize,      /* scrinit */
    xboatInitScreen,            /* initScreen */
    xboatFinishInitScreen,      /* finishInitScreen */
    xboatCreateResources,       /* createRes */
    xboatScreenFini,            /* scrfini */
    xboatCardFini,              /* cardfini */

    0,                          /* initCursor */

    0,                          /* initAccel */
    0,                          /* enableAccel */
    0,                          /* disableAccel */
    0,                          /* finiAccel */

    xboatGetColors,             /* getColors */
    xboatPutColors,             /* putColors */

    xboatCloseScreen,           /* closeScreen */
};
