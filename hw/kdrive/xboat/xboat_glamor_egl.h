/*
 * Copyright © 2013 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * xboat_glamor_egl.h
 *
 * Prototypes exposed by xboat_glamor_egl.c, without including any
 * server headers.
 */

#include <boat.h>
#include "dix-config.h"

struct xboat_glamor;
struct pixman_region16;

void
xboat_glamor_set_texture(struct xboat_glamor *xboat_glamor, uint32_t tex);

void
xboat_glamor_get_visual(void);

struct xboat_glamor *
xboat_glamor_egl_screen_init(ANativeWindow* win);

void
xboat_glamor_egl_screen_fini(struct xboat_glamor *glamor);

#ifdef GLAMOR
void
xboat_glamor_set_window_size(struct xboat_glamor *glamor,
                             unsigned width, unsigned height);

void
xboat_glamor_damage_redisplay(struct xboat_glamor *glamor,
                              struct pixman_region16 *damage);

void
xboat_glamor_process_event(BoatEvent *xev);

#else /* !GLAMOR */

static inline void
xboat_glamor_set_window_size(struct xboat_glamor *glamor,
                             unsigned width, unsigned height)
{
}

static inline void
xboat_glamor_damage_redisplay(struct xboat_glamor *glamor,
                              struct pixman_region16 *damage)
{
}

static inline void
xboat_glamor_process_event(BoatEvent *xev)
{
}

#endif /* !GLAMOR */
