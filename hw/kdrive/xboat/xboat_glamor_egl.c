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

/** @file xboat_glamor_egl.c
 *
 * Separate file for hiding Boat and EGL-using parts of xboat from
 * the rest of the server-struct-aware build.
 */

#include <stdlib.h>
#include <boat.h>
#include <pixman.h>
#include <epoxy/egl.h>
#include "xboat_glamor_egl.h"
#include "os.h"

/* until we need geometry shaders GL3.1 should suffice. */
/* Xboat has it's own copy of this for build reasons */
#define GLAMOR_GL_CORE_VER_MAJOR 3
#define GLAMOR_GL_CORE_VER_MINOR 1
/** @{
 *
 * global state for Xboat with glamor.
 *
 * Xboat can render with only one windows.
 */
static EGLDisplay dpy;
static EGLConfig egl_config;
Bool xboat_glamor_gles2;
Bool xboat_glamor_skip_present;
/** @} */

/**
 * Per-screen state for Xboat with glamor.
 */
struct xboat_glamor {
    EGLContext ctx;
    ANativeWindow* win;
    EGLSurface* egl_surf;

    GLuint tex;

    GLuint texture_shader;
    GLuint texture_shader_position_loc;
    GLuint texture_shader_texcoord_loc;

    /* Size of the window that we're rendering to. */
    unsigned width, height;

    GLuint vao, vbo;
};

static GLint
xboat_glamor_compile_glsl_prog(GLenum type, const char *source)
{
    GLint ok;
    GLint prog;

    prog = glCreateShader(type);
    glShaderSource(prog, 1, (const GLchar **) &source, NULL);
    glCompileShader(prog);
    glGetShaderiv(prog, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar *info;
        GLint size;

        glGetShaderiv(prog, GL_INFO_LOG_LENGTH, &size);
        info = malloc(size);
        if (info) {
            glGetShaderInfoLog(prog, size, NULL, info);
            ErrorF("Failed to compile %s: %s\n",
                   type == GL_FRAGMENT_SHADER ? "FS" : "VS", info);
            ErrorF("Program source:\n%s", source);
            free(info);
        }
        else
            ErrorF("Failed to get shader compilation info.\n");
        FatalError("GLSL compile failure\n");
    }

    return prog;
}

static GLuint
xboat_glamor_build_glsl_prog(GLuint vs, GLuint fs)
{
    GLint ok;
    GLuint prog;

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);

    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar *info;
        GLint size;

        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
        info = malloc(size);

        glGetProgramInfoLog(prog, size, NULL, info);
        ErrorF("Failed to link: %s\n", info);
        FatalError("GLSL link failure\n");
    }

    return prog;
}

static void
xboat_glamor_setup_texturing_shader(struct xboat_glamor *glamor)
{
    const char *vs_source =
        "attribute vec2 texcoord;\n"
        "attribute vec2 position;\n"
        "varying vec2 t;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    t = texcoord;\n"
        "    gl_Position = vec4(position, 0, 1);\n"
        "}\n";

    const char *fs_source =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "\n"
        "varying vec2 t;\n"
        "uniform sampler2D s; /* initially 0 */\n"
        "\n"
        "void main()\n"
        "{\n"
        "    gl_FragColor = texture2D(s, t);\n"
        "}\n";

    GLuint fs, vs, prog;

    vs = xboat_glamor_compile_glsl_prog(GL_VERTEX_SHADER, vs_source);
    fs = xboat_glamor_compile_glsl_prog(GL_FRAGMENT_SHADER, fs_source);
    prog = xboat_glamor_build_glsl_prog(vs, fs);

    glamor->texture_shader = prog;
    glamor->texture_shader_position_loc = glGetAttribLocation(prog, "position");
    assert(glamor->texture_shader_position_loc != -1);
    glamor->texture_shader_texcoord_loc = glGetAttribLocation(prog, "texcoord");
    assert(glamor->texture_shader_texcoord_loc != -1);
}

void
xboat_glamor_connect(void)
{
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);
}

void
xboat_glamor_set_texture(struct xboat_glamor *glamor, uint32_t tex)
{
    glamor->tex = tex;
}

static void
xboat_glamor_set_vertices(struct xboat_glamor *glamor)
{
    glVertexAttribPointer(glamor->texture_shader_position_loc,
                          2, GL_FLOAT, FALSE, 0, (void *) 0);
    glVertexAttribPointer(glamor->texture_shader_texcoord_loc,
                          2, GL_FLOAT, FALSE, 0, (void *) (sizeof (float) * 8));

    glEnableVertexAttribArray(glamor->texture_shader_position_loc);
    glEnableVertexAttribArray(glamor->texture_shader_texcoord_loc);
}

void
xboat_glamor_damage_redisplay(struct xboat_glamor *glamor,
                              struct pixman_region16 *damage)
{
    GLint old_vao;

    /* Skip presenting the output in this mode.  Presentation is
     * expensive, and if we're just running the X Test suite headless,
     * nobody's watching.
     */
    if (xboat_glamor_skip_present)
        return;

    eglMakeCurrent(dpy, glamor->egl_surf, glamor->egl_surf, glamor->ctx);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glBindVertexArray(glamor->vao);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(glamor->texture_shader);
    glViewport(0, 0, glamor->width, glamor->height);
    if (!xboat_glamor_gles2)
        glDisable(GL_COLOR_LOGIC_OP);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glamor->tex);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glBindVertexArray(old_vao);

    eglSwapBuffers(dpy, glamor->egl_surf);
}

struct xboat_glamor *
xboat_glamor_egl_screen_init(ANativeWindow* win)
{
    static const float position[] = {
        -1, -1,
         1, -1,
         1,  1,
        -1,  1,
        0, 1,
        1, 1,
        1, 0,
        0, 0,
    };
    GLint old_vao;

    EGLContext ctx;
    struct xboat_glamor *glamor;
    EGLSurface egl_surf;

    glamor = calloc(1, sizeof(struct xboat_glamor));
    if (!glamor) {
        FatalError("malloc");
        return NULL;
    }

    egl_surf = eglCreateWindowSurface(dpy, egl_config, win, NULL);

    if (xboat_glamor_gles2) {
        eglBindAPI(EGL_OPENGL_ES_API);
        static const int context_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE,
        };
        ctx = eglCreateContext(dpy, egl_config, NULL, context_attribs);
    } else {
        eglBindAPI(EGL_OPENGL_API);
        if (epoxy_has_egl_extension(dpy, "EGL_KHR_create_context")) {
            static const int context_attribs[] = {
                EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                EGL_CONTEXT_MAJOR_VERSION_KHR,
                GLAMOR_GL_CORE_VER_MAJOR,
                EGL_CONTEXT_MINOR_VERSION_KHR,
                GLAMOR_GL_CORE_VER_MINOR,
                EGL_NONE,
            };
            ctx = eglCreateContext(dpy, egl_config, NULL, context_attribs);
        } else {
            ctx = NULL;
        }

        if (!ctx)
            ctx = eglCreateContext(dpy, egl_config, NULL, NULL);
    }
    if (ctx == NULL)
        FatalError("eglCreateContext failed\n");

    if (!eglMakeCurrent(dpy, egl_surf, egl_surf, ctx))
        FatalError("eglMakeCurrent failed\n");

    glamor->ctx = ctx;
    glamor->win = win;
    glamor->egl_surf = egl_surf;
    xboat_glamor_setup_texturing_shader(glamor);

    glGenVertexArrays(1, &glamor->vao);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glBindVertexArray(glamor->vao);

    glGenBuffers(1, &glamor->vbo);

    glBindBuffer(GL_ARRAY_BUFFER, glamor->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof (position), position, GL_STATIC_DRAW);

    xboat_glamor_set_vertices(glamor);
    glBindVertexArray(old_vao);

    return glamor;
}

void
xboat_glamor_egl_screen_fini(struct xboat_glamor *glamor)
{
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, glamor->ctx);
    eglDestroySurface(dpy, glamor->egl_surf);

    free(glamor);
}

void
xboat_glamor_get_visual(void)
{
    int attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    int num_configs;
    EGLConfig *egl_configs;

    eglChooseConfig(dpy, attribs, NULL, 0, &num_configs);
    egl_configs = calloc(num_configs, sizeof(EGLConfig));
    eglChooseConfig(dpy, attribs, egl_configs, num_configs, &num_configs);
    if (!num_configs)
        FatalError("Couldn't choose an EGLConfig\n");
    egl_config = egl_configs[0];
    free(egl_configs);
}

void
xboat_glamor_set_window_size(struct xboat_glamor *glamor,
                             unsigned width, unsigned height)
{
    if (!glamor)
        return;

    glamor->width = width;
    glamor->height = height;
}
