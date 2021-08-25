/*
 * Xephyr - A kdrive X server thats runs in a host X window.
 *          Authored by Matthew Allum <mallum@openedhand.com>
 *
 * Copyright © 2007 OpenedHand Ltd
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of OpenedHand Ltd not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission. OpenedHand Ltd makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * OpenedHand Ltd DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL OpenedHand Ltd BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *    Dodji Seketeli <dodji@openedhand.com>
 */
#ifndef __XBOATLOG_H__
#define __XBOATLOG_H__

#include <assert.h>
#include "os.h"

#ifndef DEBUG
/*we are not in debug mode*/
#define XBOAT_LOG(...)
#define XBOAT_LOG_ERROR(...)
#endif                          /*!DEBUG */

#define ERROR_LOG_LEVEL 3
#define INFO_LOG_LEVEL 4

#ifndef XBOAT_LOG
#define XBOAT_LOG(...) \
LogMessageVerb(X_NOTICE, INFO_LOG_LEVEL, "in %s:%d:%s: ",\
                      __FILE__, __LINE__, __func__) ; \
LogMessageVerb(X_NOTICE, INFO_LOG_LEVEL, __VA_ARGS__)
#endif                          /*nomadik_log */

#ifndef XBOAT_LOG_ERROR
#define XBOAT_LOG_ERROR(...) \
LogMessageVerb(X_NOTICE, ERROR_LOG_LEVEL, "Error:in %s:%d:%s: ",\
                      __FILE__, __LINE__, __func__) ; \
LogMessageVerb(X_NOTICE, ERROR_LOG_LEVEL, __VA_ARGS__)
#endif                          /*XBOAT_LOG_ERROR */

#ifndef XBOAT_RETURN_IF_FAIL
#define XBOAT_RETURN_IF_FAIL(cond) \
if (!(cond)) {XBOAT_LOG_ERROR("condition %s failed\n", #cond);return;}
#endif                          /*nomadik_return_if_fail */

#ifndef XBOAT_RETURN_VAL_IF_FAIL
#define XBOAT_RETURN_VAL_IF_FAIL(cond,val) \
if (!(cond)) {XBOAT_LOG_ERROR("condition %s failed\n", #cond);return val;}
#endif                          /*nomadik_return_val_if_fail */

#endif /*__XBOATLOG_H__*/
