/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_OGL && !SDL_RENDER_DISABLED

#include "SDL_hints.h"
#include "SDL_log.h"
#include "SDL_assert.h"
#include "SDL_opengl.h"
#include "../SDL_sysrender.h"
#include "SDL_shaders_gl.h"

#ifdef __MACOSX__
#include <OpenGL/OpenGL.h>
#endif

/* To prevent unnecessary window recreation, 
 * these should match the defaults selected in SDL_GL_ResetAttributes 
 */

#define RENDERER_CONTEXT_MAJOR 2
#define RENDERER_CONTEXT_MINOR 1

/* OpenGL renderer implementation */

/* Details on optimizing the texture path on Mac OS X:
   http://developer.apple.com/library/mac/#documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_texturedata/opengl_texturedata.html
*/

/* Used to re-create the window with OpenGL capability */
extern int SDL_RecreateWindow(SDL_Window * window, Uint32 flags);

static const float inv255f = 1.0f / 255.0f;

/* !!! FIXME: delete these predeclarations and just move the functions before their use. */
static SDL_Renderer *GL_CreateRenderer(SDL_Window * window, Uint32 flags);
static int GL_GetOutputSize(SDL_Renderer * renderer, int *w, int *h);
static SDL_bool GL_SupportsBlendMode(SDL_Renderer * renderer, SDL_BlendMode blendMode);
static int GL_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture);
static int GL_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                            const SDL_Rect * rect, const void *pixels,
                            int pitch);
static int GL_UpdateTextureYUV(SDL_Renderer * renderer, SDL_Texture * texture,
                               const SDL_Rect * rect,
                               const Uint8 *Yplane, int Ypitch,
                               const Uint8 *Uplane, int Upitch,
                               const Uint8 *Vplane, int Vpitch);
static int GL_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                          const SDL_Rect * rect, void **pixels, int *pitch);
static void GL_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture);
static int GL_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture);
static int GL_QueueSetViewport(SDL_Renderer * renderer, SDL_RenderCommand *cmd);
static int GL_QueueDrawPoints(SDL_Renderer * renderer, SDL_RenderCommand *cmd,
                              const SDL_FPoint * points, int count);
static int GL_QueueFillRects(SDL_Renderer * renderer, SDL_RenderCommand *cmd,
                             const SDL_FRect * rects, int count);
static int GL_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                        const SDL_Rect *srcrect, const SDL_FRect *dstrect);
static int GL_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                          const SDL_Rect *srcquad, const SDL_FRect *dstrect,
                          const double angle, const SDL_FPoint *center,
                          const SDL_RendererFlip flip);
static int GL_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize);
static int GL_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                               Uint32 pixel_format, void * pixels, int pitch);
static void GL_RenderPresent(SDL_Renderer * renderer);
static void GL_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture);
static void GL_DestroyRenderer(SDL_Renderer * renderer);
static int GL_BindTexture (SDL_Renderer * renderer, SDL_Texture *texture, float *texw, float *texh);
static int GL_UnbindTexture (SDL_Renderer * renderer, SDL_Texture *texture);

SDL_RenderDriver GL_RenderDriver = {
    GL_CreateRenderer,
    {
     "opengl",
     (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE),
     1,
     {SDL_PIXELFORMAT_ARGB8888},
     0,
     0}
};

typedef struct GL_FBOList GL_FBOList;

struct GL_FBOList
{
    Uint32 w, h;
    GLuint FBO;
    GL_FBOList *next;
};

typedef struct
{
    SDL_GLContext context;

    SDL_bool debug_enabled;
    SDL_bool GL_ARB_debug_output_supported;
    int errors;
    char **error_messages;
    GLDEBUGPROCARB next_error_callback;
    GLvoid *next_error_userparam;

    GLenum textype;

    SDL_bool GL_ARB_texture_non_power_of_two_supported;
    SDL_bool GL_ARB_texture_rectangle_supported;
    SDL_bool GL_EXT_framebuffer_object_supported;
    GL_FBOList *framebuffers;

    /* OpenGL functions */
#define SDL_PROC(ret,func,params) ret (APIENTRY *func) params;
#include "SDL_glfuncs.h"
#undef SDL_PROC

    /* Multitexture support */
    SDL_bool GL_ARB_multitexture_supported;
    PFNGLACTIVETEXTUREARBPROC glActiveTextureARB;
    GLint num_texture_units;

    PFNGLGENFRAMEBUFFERSEXTPROC glGenFramebuffersEXT;
    PFNGLDELETEFRAMEBUFFERSEXTPROC glDeleteFramebuffersEXT;
    PFNGLFRAMEBUFFERTEXTURE2DEXTPROC glFramebufferTexture2DEXT;
    PFNGLBINDFRAMEBUFFEREXTPROC glBindFramebufferEXT;
    PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC glCheckFramebufferStatusEXT;

    /* Shader support */
    GL_ShaderContext *shaders;

} GL_RenderData;

typedef struct
{
    GLuint texture;
    GLfloat texw;
    GLfloat texh;
    GLenum format;
    GLenum formattype;
    void *pixels;
    int pitch;
    SDL_Rect locked_rect;

    /* YUV texture support */
    SDL_bool yuv;
    SDL_bool nv12;
    GLuint utexture;
    GLuint vtexture;

    GL_FBOList *fbo;
} GL_TextureData;

SDL_FORCE_INLINE const char*
GL_TranslateError (GLenum error)
{
#define GL_ERROR_TRANSLATE(e) case e: return #e;
    switch (error) {
    GL_ERROR_TRANSLATE(GL_INVALID_ENUM)
    GL_ERROR_TRANSLATE(GL_INVALID_VALUE)
    GL_ERROR_TRANSLATE(GL_INVALID_OPERATION)
    GL_ERROR_TRANSLATE(GL_OUT_OF_MEMORY)
    GL_ERROR_TRANSLATE(GL_NO_ERROR)
    GL_ERROR_TRANSLATE(GL_STACK_OVERFLOW)
    GL_ERROR_TRANSLATE(GL_STACK_UNDERFLOW)
    GL_ERROR_TRANSLATE(GL_TABLE_TOO_LARGE)
    default:
        return "UNKNOWN";
}
#undef GL_ERROR_TRANSLATE
}

SDL_FORCE_INLINE void
GL_ClearErrors(SDL_Renderer *renderer)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;

    if (!data->debug_enabled)
    {
        return;
    }
    if (data->GL_ARB_debug_output_supported) {
        if (data->errors) {
            int i;
            for (i = 0; i < data->errors; ++i) {
                SDL_free(data->error_messages[i]);
            }
            SDL_free(data->error_messages);

            data->errors = 0;
            data->error_messages = NULL;
        }
    } else {
        while (data->glGetError() != GL_NO_ERROR) {
            continue;
        }
    }
}

SDL_FORCE_INLINE int
GL_CheckAllErrors (const char *prefix, SDL_Renderer *renderer, const char *file, int line, const char *function)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;
    int ret = 0;

    if (!data->debug_enabled)
    {
        return 0;
    }
    if (data->GL_ARB_debug_output_supported) {
        if (data->errors) {
            int i;
            for (i = 0; i < data->errors; ++i) {
                SDL_SetError("%s: %s (%d): %s %s", prefix, file, line, function, data->error_messages[i]);
                ret = -1;
            }
            GL_ClearErrors(renderer);
        }
    } else {
        /* check gl errors (can return multiple errors) */
        for (;;) {
            GLenum error = data->glGetError();
            if (error != GL_NO_ERROR) {
                if (prefix == NULL || prefix[0] == '\0') {
                    prefix = "generic";
                }
                SDL_SetError("%s: %s (%d): %s %s (0x%X)", prefix, file, line, function, GL_TranslateError(error), error);
                ret = -1;
            } else {
                break;
            }
        }
    }
    return ret;
}

#if 0
#define GL_CheckError(prefix, renderer)
#else
#define GL_CheckError(prefix, renderer) GL_CheckAllErrors(prefix, renderer, SDL_FILE, SDL_LINE, SDL_FUNCTION)
#endif

static int
GL_LoadFunctions(GL_RenderData * data)
{
#ifdef __SDL_NOGETPROCADDR__
#define SDL_PROC(ret,func,params) data->func=func;
#else
#define SDL_PROC(ret,func,params) \
    do { \
        data->func = SDL_GL_GetProcAddress(#func); \
        if ( ! data->func ) { \
            return SDL_SetError("Couldn't load GL function %s: %s", #func, SDL_GetError()); \
        } \
    } while ( 0 );
#endif /* __SDL_NOGETPROCADDR__ */

#include "SDL_glfuncs.h"
#undef SDL_PROC
    return 0;
}

static int
GL_ActivateRenderer(SDL_Renderer * renderer)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;

    if (SDL_GL_GetCurrentContext() != data->context) {
        if (SDL_GL_MakeCurrent(renderer->window, data->context) < 0) {
            return -1;
        }
    }

    GL_ClearErrors(renderer);

    return 0;
}

static void APIENTRY
GL_HandleDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *userParam)
{
    SDL_Renderer *renderer = (SDL_Renderer *) userParam;
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;

    if (type == GL_DEBUG_TYPE_ERROR_ARB) {
        /* Record this error */
        int errors = data->errors + 1;
        char **error_messages = SDL_realloc(data->error_messages, errors * sizeof(*data->error_messages));
        if (error_messages) {
            data->errors = errors;
            data->error_messages = error_messages;
            data->error_messages[data->errors-1] = SDL_strdup(message);
        }
    }

    /* If there's another error callback, pass it along, otherwise log it */
    if (data->next_error_callback) {
        data->next_error_callback(source, type, id, severity, length, message, data->next_error_userparam);
    } else {
        if (type == GL_DEBUG_TYPE_ERROR_ARB) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "%s", message);
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_RENDER, "%s", message);
        }
    }
}

static GL_FBOList *
GL_GetFBO(GL_RenderData *data, Uint32 w, Uint32 h)
{
    GL_FBOList *result = data->framebuffers;

    while (result && ((result->w != w) || (result->h != h))) {
        result = result->next;
    }

    if (!result) {
        result = SDL_malloc(sizeof(GL_FBOList));
        if (result) {
            result->w = w;
            result->h = h;
            data->glGenFramebuffersEXT(1, &result->FBO);
            result->next = data->framebuffers;
            data->framebuffers = result;
        }
    }
    return result;
}

SDL_Renderer *
GL_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    SDL_Renderer *renderer;
    GL_RenderData *data;
    GLint value;
    Uint32 window_flags;
    int profile_mask = 0, major = 0, minor = 0;
    SDL_bool changed_window = SDL_FALSE;

    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile_mask);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
    
    window_flags = SDL_GetWindowFlags(window);
    if (!(window_flags & SDL_WINDOW_OPENGL) ||
        profile_mask == SDL_GL_CONTEXT_PROFILE_ES || major != RENDERER_CONTEXT_MAJOR || minor != RENDERER_CONTEXT_MINOR) {

        changed_window = SDL_TRUE;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, RENDERER_CONTEXT_MAJOR);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, RENDERER_CONTEXT_MINOR);

        if (SDL_RecreateWindow(window, window_flags | SDL_WINDOW_OPENGL) < 0) {
            goto error;
        }
    }

    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        goto error;
    }

    data = (GL_RenderData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        GL_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        goto error;
    }

    renderer->GetOutputSize = GL_GetOutputSize;
    renderer->SupportsBlendMode = GL_SupportsBlendMode;
    renderer->CreateTexture = GL_CreateTexture;
    renderer->UpdateTexture = GL_UpdateTexture;
    renderer->UpdateTextureYUV = GL_UpdateTextureYUV;
    renderer->LockTexture = GL_LockTexture;
    renderer->UnlockTexture = GL_UnlockTexture;
    renderer->SetRenderTarget = GL_SetRenderTarget;
    renderer->QueueSetViewport = GL_QueueSetViewport;
    renderer->QueueSetDrawColor = GL_QueueSetViewport;  /* SetViewport and SetDrawColor are (currently) no-ops. */
    renderer->QueueDrawPoints = GL_QueueDrawPoints;
    renderer->QueueDrawLines = GL_QueueDrawPoints;  /* lines and points queue vertices the same way. */
    renderer->QueueFillRects = GL_QueueFillRects;
    renderer->QueueCopy = GL_QueueCopy;
    renderer->QueueCopyEx = GL_QueueCopyEx;
    renderer->RunCommandQueue = GL_RunCommandQueue;
    renderer->RenderReadPixels = GL_RenderReadPixels;
    renderer->RenderPresent = GL_RenderPresent;
    renderer->DestroyTexture = GL_DestroyTexture;
    renderer->DestroyRenderer = GL_DestroyRenderer;
    renderer->GL_BindTexture = GL_BindTexture;
    renderer->GL_UnbindTexture = GL_UnbindTexture;
    renderer->info = GL_RenderDriver.info;
    renderer->info.flags = SDL_RENDERER_ACCELERATED;
    renderer->driverdata = data;
    renderer->window = window;

    data->context = SDL_GL_CreateContext(window);
    if (!data->context) {
        GL_DestroyRenderer(renderer);
        goto error;
    }
    if (SDL_GL_MakeCurrent(window, data->context) < 0) {
        GL_DestroyRenderer(renderer);
        goto error;
    }

    if (GL_LoadFunctions(data) < 0) {
        GL_DestroyRenderer(renderer);
        goto error;
    }

#ifdef __MACOSX__
    /* Enable multi-threaded rendering */
    /* Disabled until Ryan finishes his VBO/PBO code...
       CGLEnable(CGLGetCurrentContext(), kCGLCEMPEngine);
     */
#endif

    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
    if (SDL_GL_GetSwapInterval() > 0) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }

    /* Check for debug output support */
    if (SDL_GL_GetAttribute(SDL_GL_CONTEXT_FLAGS, &value) == 0 &&
        (value & SDL_GL_CONTEXT_DEBUG_FLAG)) {
        data->debug_enabled = SDL_TRUE;
    }
    if (data->debug_enabled && SDL_GL_ExtensionSupported("GL_ARB_debug_output")) {
        PFNGLDEBUGMESSAGECALLBACKARBPROC glDebugMessageCallbackARBFunc = (PFNGLDEBUGMESSAGECALLBACKARBPROC) SDL_GL_GetProcAddress("glDebugMessageCallbackARB");

        data->GL_ARB_debug_output_supported = SDL_TRUE;
        data->glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION_ARB, (GLvoid **)(char *)&data->next_error_callback);
        data->glGetPointerv(GL_DEBUG_CALLBACK_USER_PARAM_ARB, &data->next_error_userparam);
        glDebugMessageCallbackARBFunc(GL_HandleDebugMessage, renderer);

        /* Make sure our callback is called when errors actually happen */
        data->glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
    }

    data->textype = GL_TEXTURE_2D;
    if (SDL_GL_ExtensionSupported("GL_ARB_texture_non_power_of_two")) {
        data->GL_ARB_texture_non_power_of_two_supported = SDL_TRUE;
    } else if (SDL_GL_ExtensionSupported("GL_ARB_texture_rectangle") ||
               SDL_GL_ExtensionSupported("GL_EXT_texture_rectangle")) {
        data->GL_ARB_texture_rectangle_supported = SDL_TRUE;
        data->textype = GL_TEXTURE_RECTANGLE_ARB;
    }
    if (data->GL_ARB_texture_rectangle_supported) {
        data->glGetIntegerv(GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB, &value);
        renderer->info.max_texture_width = value;
        renderer->info.max_texture_height = value;
    } else {
        data->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
        renderer->info.max_texture_width = value;
        renderer->info.max_texture_height = value;
    }

    /* Check for multitexture support */
    if (SDL_GL_ExtensionSupported("GL_ARB_multitexture")) {
        data->glActiveTextureARB = (PFNGLACTIVETEXTUREARBPROC) SDL_GL_GetProcAddress("glActiveTextureARB");
        if (data->glActiveTextureARB) {
            data->GL_ARB_multitexture_supported = SDL_TRUE;
            data->glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &data->num_texture_units);
        }
    }

    /* Check for shader support */
    if (SDL_GetHintBoolean(SDL_HINT_RENDER_OPENGL_SHADERS, SDL_TRUE)) {
        data->shaders = GL_CreateShaderContext();
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER, "OpenGL shaders: %s",
                data->shaders ? "ENABLED" : "DISABLED");

    /* We support YV12 textures using 3 textures and a shader */
    if (data->shaders && data->num_texture_units >= 3) {
        renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_YV12;
        renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_IYUV;
        renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_NV12;
        renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_NV21;
    }

#ifdef __MACOSX__
    renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_UYVY;
#endif

    if (SDL_GL_ExtensionSupported("GL_EXT_framebuffer_object")) {
        data->GL_EXT_framebuffer_object_supported = SDL_TRUE;
        data->glGenFramebuffersEXT = (PFNGLGENFRAMEBUFFERSEXTPROC)
            SDL_GL_GetProcAddress("glGenFramebuffersEXT");
        data->glDeleteFramebuffersEXT = (PFNGLDELETEFRAMEBUFFERSEXTPROC)
            SDL_GL_GetProcAddress("glDeleteFramebuffersEXT");
        data->glFramebufferTexture2DEXT = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)
            SDL_GL_GetProcAddress("glFramebufferTexture2DEXT");
        data->glBindFramebufferEXT = (PFNGLBINDFRAMEBUFFEREXTPROC)
            SDL_GL_GetProcAddress("glBindFramebufferEXT");
        data->glCheckFramebufferStatusEXT = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)
            SDL_GL_GetProcAddress("glCheckFramebufferStatusEXT");
        renderer->info.flags |= SDL_RENDERER_TARGETTEXTURE;
    }
    data->framebuffers = NULL;

    /* Set up parameters for rendering */
    data->glDisable(GL_DEPTH_TEST);
    data->glDisable(GL_CULL_FACE);
    /* This ended up causing video discrepancies between OpenGL and Direct3D */
    /* data->glEnable(GL_LINE_SMOOTH); */

    return renderer;

error:
    if (changed_window) {
        /* Uh oh, better try to put it back... */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile_mask);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        SDL_RecreateWindow(window, window_flags);
    }
    return NULL;
}

static int
GL_GetOutputSize(SDL_Renderer * renderer, int *w, int *h)
{
    SDL_GL_GetDrawableSize(renderer->window, w, h);
    return 0;
}

static GLenum GetBlendFunc(SDL_BlendFactor factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:
        return GL_ZERO;
    case SDL_BLENDFACTOR_ONE:
        return GL_ONE;
    case SDL_BLENDFACTOR_SRC_COLOR:
        return GL_SRC_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        return GL_ONE_MINUS_SRC_COLOR;
    case SDL_BLENDFACTOR_SRC_ALPHA:
        return GL_SRC_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case SDL_BLENDFACTOR_DST_COLOR:
        return GL_DST_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        return GL_ONE_MINUS_DST_COLOR;
    case SDL_BLENDFACTOR_DST_ALPHA:
        return GL_DST_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    default:
        return GL_INVALID_ENUM;
    }
}

static GLenum GetBlendEquation(SDL_BlendOperation operation)
{
    switch (operation) {
    case SDL_BLENDOPERATION_ADD:
        return GL_FUNC_ADD;
    case SDL_BLENDOPERATION_SUBTRACT:
        return GL_FUNC_SUBTRACT;
    case SDL_BLENDOPERATION_REV_SUBTRACT:
        return GL_FUNC_REVERSE_SUBTRACT;
    default:
        return GL_INVALID_ENUM;
    }
}

static SDL_bool
GL_SupportsBlendMode(SDL_Renderer * renderer, SDL_BlendMode blendMode)
{
    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    if (GetBlendFunc(srcColorFactor) == GL_INVALID_ENUM ||
        GetBlendFunc(srcAlphaFactor) == GL_INVALID_ENUM ||
        GetBlendEquation(colorOperation) == GL_INVALID_ENUM ||
        GetBlendFunc(dstColorFactor) == GL_INVALID_ENUM ||
        GetBlendFunc(dstAlphaFactor) == GL_INVALID_ENUM ||
        GetBlendEquation(alphaOperation) == GL_INVALID_ENUM) {
        return SDL_FALSE;
    }
    if (colorOperation != alphaOperation) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

SDL_FORCE_INLINE int
power_of_2(int input)
{
    int value = 1;

    while (value < input) {
        value <<= 1;
    }
    return value;
}

SDL_FORCE_INLINE SDL_bool
convert_format(GL_RenderData *renderdata, Uint32 pixel_format,
               GLint* internalFormat, GLenum* format, GLenum* type)
{
    switch (pixel_format) {
    case SDL_PIXELFORMAT_ARGB8888:
        *internalFormat = GL_RGBA8;
        *format = GL_BGRA;
        *type = GL_UNSIGNED_INT_8_8_8_8_REV;
        break;
    case SDL_PIXELFORMAT_YV12:
    case SDL_PIXELFORMAT_IYUV:
    case SDL_PIXELFORMAT_NV12:
    case SDL_PIXELFORMAT_NV21:
        *internalFormat = GL_LUMINANCE;
        *format = GL_LUMINANCE;
        *type = GL_UNSIGNED_BYTE;
        break;
#ifdef __MACOSX__
    case SDL_PIXELFORMAT_UYVY:
        *internalFormat = GL_RGB8;
        *format = GL_YCBCR_422_APPLE;
        *type = GL_UNSIGNED_SHORT_8_8_APPLE;
        break;
#endif
    default:
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

static int
GL_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    GL_RenderData *renderdata = (GL_RenderData *) renderer->driverdata;
    const GLenum textype = renderdata->textype;
    GL_TextureData *data;
    GLint internalFormat;
    GLenum format, type;
    int texture_w, texture_h;
    GLenum scaleMode;

    GL_ActivateRenderer(renderer);

    if (texture->access == SDL_TEXTUREACCESS_TARGET &&
        !renderdata->GL_EXT_framebuffer_object_supported) {
        return SDL_SetError("Render targets not supported by OpenGL");
    }

    if (!convert_format(renderdata, texture->format, &internalFormat,
                        &format, &type)) {
        return SDL_SetError("Texture format %s not supported by OpenGL",
                            SDL_GetPixelFormatName(texture->format));
    }

    data = (GL_TextureData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }

    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        size_t size;
        data->pitch = texture->w * SDL_BYTESPERPIXEL(texture->format);
        size = texture->h * data->pitch;
        if (texture->format == SDL_PIXELFORMAT_YV12 ||
            texture->format == SDL_PIXELFORMAT_IYUV) {
            /* Need to add size for the U and V planes */
            size += 2 * ((texture->h + 1) / 2) * ((data->pitch + 1) / 2);
        }
        if (texture->format == SDL_PIXELFORMAT_NV12 ||
            texture->format == SDL_PIXELFORMAT_NV21) {
            /* Need to add size for the U/V plane */
            size += 2 * ((texture->h + 1) / 2) * ((data->pitch + 1) / 2);
        }
        data->pixels = SDL_calloc(1, size);
        if (!data->pixels) {
            SDL_free(data);
            return SDL_OutOfMemory();
        }
    }

    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        data->fbo = GL_GetFBO(renderdata, texture->w, texture->h);
    } else {
        data->fbo = NULL;
    }

    GL_CheckError("", renderer);
    renderdata->glGenTextures(1, &data->texture);
    if (GL_CheckError("glGenTextures()", renderer) < 0) {
        if (data->pixels) {
            SDL_free(data->pixels);
        }
        SDL_free(data);
        return -1;
    }
    texture->driverdata = data;

    if (renderdata->GL_ARB_texture_non_power_of_two_supported) {
        texture_w = texture->w;
        texture_h = texture->h;
        data->texw = 1.0f;
        data->texh = 1.0f;
    } else if (renderdata->GL_ARB_texture_rectangle_supported) {
        texture_w = texture->w;
        texture_h = texture->h;
        data->texw = (GLfloat) texture_w;
        data->texh = (GLfloat) texture_h;
    } else {
        texture_w = power_of_2(texture->w);
        texture_h = power_of_2(texture->h);
        data->texw = (GLfloat) (texture->w) / texture_w;
        data->texh = (GLfloat) texture->h / texture_h;
    }

    data->format = format;
    data->formattype = type;
    scaleMode = (texture->scaleMode == SDL_ScaleModeNearest) ? GL_NEAREST : GL_LINEAR;
    renderdata->glEnable(textype);
    renderdata->glBindTexture(textype, data->texture);
    renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, scaleMode);
    renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, scaleMode);
    /* According to the spec, CLAMP_TO_EDGE is the default for TEXTURE_RECTANGLE
       and setting it causes an INVALID_ENUM error in the latest NVidia drivers.
    */
    if (textype != GL_TEXTURE_RECTANGLE_ARB) {
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_S,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_T,
                                    GL_CLAMP_TO_EDGE);
    }
#ifdef __MACOSX__
#ifndef GL_TEXTURE_STORAGE_HINT_APPLE
#define GL_TEXTURE_STORAGE_HINT_APPLE       0x85BC
#endif
#ifndef STORAGE_CACHED_APPLE
#define STORAGE_CACHED_APPLE                0x85BE
#endif
#ifndef STORAGE_SHARED_APPLE
#define STORAGE_SHARED_APPLE                0x85BF
#endif
    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        renderdata->glTexParameteri(textype, GL_TEXTURE_STORAGE_HINT_APPLE,
                                    GL_STORAGE_SHARED_APPLE);
    } else {
        renderdata->glTexParameteri(textype, GL_TEXTURE_STORAGE_HINT_APPLE,
                                    GL_STORAGE_CACHED_APPLE);
    }
    if (texture->access == SDL_TEXTUREACCESS_STREAMING
        && texture->format == SDL_PIXELFORMAT_ARGB8888
        && (texture->w % 8) == 0) {
        renderdata->glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        renderdata->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH,
                          (data->pitch / SDL_BYTESPERPIXEL(texture->format)));
        renderdata->glTexImage2D(textype, 0, internalFormat, texture_w,
                                 texture_h, 0, format, type, data->pixels);
        renderdata->glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
    }
    else
#endif
    {
        renderdata->glTexImage2D(textype, 0, internalFormat, texture_w,
                                 texture_h, 0, format, type, NULL);
    }
    renderdata->glDisable(textype);
    if (GL_CheckError("glTexImage2D()", renderer) < 0) {
        return -1;
    }

    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        data->yuv = SDL_TRUE;

        renderdata->glGenTextures(1, &data->utexture);
        renderdata->glGenTextures(1, &data->vtexture);
        renderdata->glEnable(textype);

        renderdata->glBindTexture(textype, data->utexture);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_S,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_T,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexImage2D(textype, 0, internalFormat, (texture_w+1)/2,
                                 (texture_h+1)/2, 0, format, type, NULL);

        renderdata->glBindTexture(textype, data->vtexture);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_S,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_T,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexImage2D(textype, 0, internalFormat, (texture_w+1)/2,
                                 (texture_h+1)/2, 0, format, type, NULL);

        renderdata->glDisable(textype);
    }

    if (texture->format == SDL_PIXELFORMAT_NV12 ||
        texture->format == SDL_PIXELFORMAT_NV21) {
        data->nv12 = SDL_TRUE;

        renderdata->glGenTextures(1, &data->utexture);
        renderdata->glEnable(textype);

        renderdata->glBindTexture(textype, data->utexture);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_S,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexParameteri(textype, GL_TEXTURE_WRAP_T,
                                    GL_CLAMP_TO_EDGE);
        renderdata->glTexImage2D(textype, 0, GL_LUMINANCE_ALPHA, (texture_w+1)/2,
                                 (texture_h+1)/2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
        renderdata->glDisable(textype);
    }

    return GL_CheckError("", renderer);
}

static int
GL_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                 const SDL_Rect * rect, const void *pixels, int pitch)
{
    GL_RenderData *renderdata = (GL_RenderData *) renderer->driverdata;
    const GLenum textype = renderdata->textype;
    GL_TextureData *data = (GL_TextureData *) texture->driverdata;
    const int texturebpp = SDL_BYTESPERPIXEL(texture->format);

    SDL_assert(texturebpp != 0);  /* otherwise, division by zero later. */

    GL_ActivateRenderer(renderer);

    renderdata->glEnable(textype);
    renderdata->glBindTexture(textype, data->texture);
    renderdata->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, (pitch / texturebpp));
    renderdata->glTexSubImage2D(textype, 0, rect->x, rect->y, rect->w,
                                rect->h, data->format, data->formattype,
                                pixels);
    if (data->yuv) {
        renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, ((pitch + 1) / 2));

        /* Skip to the correct offset into the next texture */
        pixels = (const void*)((const Uint8*)pixels + rect->h * pitch);
        if (texture->format == SDL_PIXELFORMAT_YV12) {
            renderdata->glBindTexture(textype, data->vtexture);
        } else {
            renderdata->glBindTexture(textype, data->utexture);
        }
        renderdata->glTexSubImage2D(textype, 0, rect->x/2, rect->y/2,
                                    (rect->w+1)/2, (rect->h+1)/2,
                                    data->format, data->formattype, pixels);

        /* Skip to the correct offset into the next texture */
        pixels = (const void*)((const Uint8*)pixels + ((rect->h + 1) / 2) * ((pitch + 1) / 2));
        if (texture->format == SDL_PIXELFORMAT_YV12) {
            renderdata->glBindTexture(textype, data->utexture);
        } else {
            renderdata->glBindTexture(textype, data->vtexture);
        }
        renderdata->glTexSubImage2D(textype, 0, rect->x/2, rect->y/2,
                                    (rect->w+1)/2, (rect->h+1)/2,
                                    data->format, data->formattype, pixels);
    }

    if (data->nv12) {
        renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, ((pitch + 1) / 2));

        /* Skip to the correct offset into the next texture */
        pixels = (const void*)((const Uint8*)pixels + rect->h * pitch);
        renderdata->glBindTexture(textype, data->utexture);
        renderdata->glTexSubImage2D(textype, 0, rect->x/2, rect->y/2,
                                    (rect->w + 1)/2, (rect->h + 1)/2,
                                    GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, pixels);
    }
    renderdata->glDisable(textype);

    return GL_CheckError("glTexSubImage2D()", renderer);
}

static int
GL_UpdateTextureYUV(SDL_Renderer * renderer, SDL_Texture * texture,
                    const SDL_Rect * rect,
                    const Uint8 *Yplane, int Ypitch,
                    const Uint8 *Uplane, int Upitch,
                    const Uint8 *Vplane, int Vpitch)
{
    GL_RenderData *renderdata = (GL_RenderData *) renderer->driverdata;
    const GLenum textype = renderdata->textype;
    GL_TextureData *data = (GL_TextureData *) texture->driverdata;

    GL_ActivateRenderer(renderer);

    renderdata->glEnable(textype);
    renderdata->glBindTexture(textype, data->texture);
    renderdata->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Ypitch);
    renderdata->glTexSubImage2D(textype, 0, rect->x, rect->y, rect->w,
                                rect->h, data->format, data->formattype,
                                Yplane);

    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Upitch);
    renderdata->glBindTexture(textype, data->utexture);
    renderdata->glTexSubImage2D(textype, 0, rect->x/2, rect->y/2,
                                (rect->w + 1)/2, (rect->h + 1)/2,
                                data->format, data->formattype, Uplane);

    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Vpitch);
    renderdata->glBindTexture(textype, data->vtexture);
    renderdata->glTexSubImage2D(textype, 0, rect->x/2, rect->y/2,
                                (rect->w + 1)/2, (rect->h + 1)/2,
                                data->format, data->formattype, Vplane);
    renderdata->glDisable(textype);

    return GL_CheckError("glTexSubImage2D()", renderer);
}

static int
GL_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
               const SDL_Rect * rect, void **pixels, int *pitch)
{
    GL_TextureData *data = (GL_TextureData *) texture->driverdata;

    data->locked_rect = *rect;
    *pixels =
        (void *) ((Uint8 *) data->pixels + rect->y * data->pitch +
                  rect->x * SDL_BYTESPERPIXEL(texture->format));
    *pitch = data->pitch;
    return 0;
}

static void
GL_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    GL_TextureData *data = (GL_TextureData *) texture->driverdata;
    const SDL_Rect *rect;
    void *pixels;

    rect = &data->locked_rect;
    pixels =
        (void *) ((Uint8 *) data->pixels + rect->y * data->pitch +
                  rect->x * SDL_BYTESPERPIXEL(texture->format));
    GL_UpdateTexture(renderer, texture, rect, pixels, data->pitch);
}

static int
GL_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;
    GL_TextureData *texturedata;
    GLenum status;

    GL_ActivateRenderer(renderer);

    if (!data->GL_EXT_framebuffer_object_supported) {
        return SDL_SetError("Render targets not supported by OpenGL");
    }

    if (texture == NULL) {
        data->glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        return 0;
    }

    texturedata = (GL_TextureData *) texture->driverdata;
    data->glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, texturedata->fbo->FBO);
    /* TODO: check if texture pixel format allows this operation */
    data->glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, data->textype, texturedata->texture, 0);
    /* Check FBO status */
    status = data->glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) {
        return SDL_SetError("glFramebufferTexture2DEXT() failed");
    }
    return 0;
}

/* !!! FIXME: all these Queue* calls set up the vertex buffer the way the immediate mode
   !!! FIXME:  renderer wants it, but this might want to operate differently if we move to
   !!! FIXME:  VBOs at some point. */
static int
GL_QueueSetViewport(SDL_Renderer * renderer, SDL_RenderCommand *cmd)
{
    return 0;  /* nothing to do in this backend. */
}

static int
GL_QueueDrawPoints(SDL_Renderer * renderer, SDL_RenderCommand *cmd, const SDL_FPoint * points, int count)
{
    GLfloat *verts = (GLfloat *) SDL_AllocateRenderVertices(renderer, count * 2 * sizeof (GLfloat), 0, &cmd->data.draw.first);
    size_t i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    for (i = 0; i < count; i++) {
        *(verts++) = 0.5f + points[i].x;
        *(verts++) = 0.5f + points[i].y;
    }

    return 0;
}

static int
GL_QueueFillRects(SDL_Renderer * renderer, SDL_RenderCommand *cmd, const SDL_FRect * rects, int count)
{
    GLfloat *verts = (GLfloat *) SDL_AllocateRenderVertices(renderer, count * 4 * sizeof (GLfloat), 0, &cmd->data.draw.first);
    size_t i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    for (i = 0; i < count; i++) {
        const SDL_FRect *rect = &rects[i];
        *(verts++) = rect->x;
        *(verts++) = rect->y;
        *(verts++) = rect->x + rect->w;
        *(verts++) = rect->y + rect->h;
    }

    return 0;
}

static int
GL_QueueCopy(SDL_Renderer * renderer, SDL_RenderCommand *cmd, SDL_Texture * texture,
             const SDL_Rect * srcrect, const SDL_FRect * dstrect)
{
    GL_TextureData *texturedata = (GL_TextureData *) texture->driverdata;
    GLfloat minx, miny, maxx, maxy;
    GLfloat minu, maxu, minv, maxv;
    GLfloat *verts = (GLfloat *) SDL_AllocateRenderVertices(renderer, 8 * sizeof (GLfloat), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = 1;

    minx = dstrect->x;
    miny = dstrect->y;
    maxx = dstrect->x + dstrect->w;
    maxy = dstrect->y + dstrect->h;

    minu = (GLfloat) srcrect->x / texture->w;
    minu *= texturedata->texw;
    maxu = (GLfloat) (srcrect->x + srcrect->w) / texture->w;
    maxu *= texturedata->texw;
    minv = (GLfloat) srcrect->y / texture->h;
    minv *= texturedata->texh;
    maxv = (GLfloat) (srcrect->y + srcrect->h) / texture->h;
    maxv *= texturedata->texh;

    cmd->data.draw.count = 1;
    *(verts++) = minx;
    *(verts++) = miny;
    *(verts++) = maxx;
    *(verts++) = maxy;
    *(verts++) = minu;
    *(verts++) = maxu;
    *(verts++) = minv;
    *(verts++) = maxv;
    return 0;
}

static int
GL_QueueCopyEx(SDL_Renderer * renderer, SDL_RenderCommand *cmd, SDL_Texture * texture,
               const SDL_Rect * srcrect, const SDL_FRect * dstrect,
               const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip)
{
    GL_TextureData *texturedata = (GL_TextureData *) texture->driverdata;
    GLfloat minx, miny, maxx, maxy;
    GLfloat centerx, centery;
    GLfloat minu, maxu, minv, maxv;
    GLfloat *verts = (GLfloat *) SDL_AllocateRenderVertices(renderer, 11 * sizeof (GLfloat), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    centerx = center->x;
    centery = center->y;

    if (flip & SDL_FLIP_HORIZONTAL) {
        minx =  dstrect->w - centerx;
        maxx = -centerx;
    }
    else {
        minx = -centerx;
        maxx =  dstrect->w - centerx;
    }

    if (flip & SDL_FLIP_VERTICAL) {
        miny =  dstrect->h - centery;
        maxy = -centery;
    }
    else {
        miny = -centery;
        maxy =  dstrect->h - centery;
    }

    minu = (GLfloat) srcrect->x / texture->w;
    minu *= texturedata->texw;
    maxu = (GLfloat) (srcrect->x + srcrect->w) / texture->w;
    maxu *= texturedata->texw;
    minv = (GLfloat) srcrect->y / texture->h;
    minv *= texturedata->texh;
    maxv = (GLfloat) (srcrect->y + srcrect->h) / texture->h;
    maxv *= texturedata->texh;

    cmd->data.draw.count = 1;
    *(verts++) = minx;
    *(verts++) = miny;
    *(verts++) = maxx;
    *(verts++) = maxy;
    *(verts++) = minu;
    *(verts++) = maxu;
    *(verts++) = minv;
    *(verts++) = maxv;
    *(verts++) = (GLfloat) dstrect->x + centerx;
    *(verts++) = (GLfloat) dstrect->y + centery;
    *(verts++) = (GLfloat) angle;
    return 0;
}

static void
SetDrawState(const GL_RenderData *data, const SDL_RenderCommand *cmd, const GL_Shader shader,
             Uint32 *current_color, SDL_BlendMode *current_blend, GL_Shader *current_shader,
             SDL_bool *current_texturing)
{
    const Uint8 r = cmd->data.draw.r;
    const Uint8 g = cmd->data.draw.g;
    const Uint8 b = cmd->data.draw.b;
    const Uint8 a = cmd->data.draw.a;
    const Uint32 color = ((a << 24) | (r << 16) | (g << 8) | b);
    const SDL_BlendMode blend = cmd->data.draw.blend;

    if (color != *current_color) {
        data->glColor4f((GLfloat) r * inv255f, (GLfloat) g * inv255f,
                        (GLfloat) b * inv255f, (GLfloat) a * inv255f);
        *current_color = color;
    }

    if (blend != *current_blend) {
        if (blend == SDL_BLENDMODE_NONE) {
            data->glDisable(GL_BLEND);
        } else {
            data->glEnable(GL_BLEND);
            data->glBlendFuncSeparate(GetBlendFunc(SDL_GetBlendModeSrcColorFactor(blend)),
                                      GetBlendFunc(SDL_GetBlendModeDstColorFactor(blend)),
                                      GetBlendFunc(SDL_GetBlendModeSrcAlphaFactor(blend)),
                                      GetBlendFunc(SDL_GetBlendModeDstAlphaFactor(blend)));
            data->glBlendEquation(GetBlendEquation(SDL_GetBlendModeColorOperation(blend)));
        }
        *current_blend = blend;
    }

    if (data->shaders && (shader != *current_shader)) {
        GL_SelectShader(data->shaders, shader);
        *current_shader = shader;
    }

    if ((cmd->data.draw.texture != NULL) != *current_texturing) {
        if (cmd->data.draw.texture == NULL) {
            data->glDisable(data->textype);
            *current_texturing = SDL_FALSE;
        } else {
            data->glEnable(data->textype);
            *current_texturing = SDL_FALSE;
        }
    }
}

static void
SetCopyState(const GL_RenderData *data, const SDL_RenderCommand *cmd,
             Uint32 *current_color, SDL_BlendMode *current_blend, GL_Shader *current_shader,
             SDL_bool *current_texturing, SDL_Texture **current_texture)
{
    SDL_Texture *texture = cmd->data.draw.texture;
    const GL_TextureData *texturedata = (GL_TextureData *) texture->driverdata;
    GL_Shader shader = SHADER_RGB;

    if (data->shaders) {
        if (texturedata->yuv || texturedata->nv12) {
            switch (SDL_GetYUVConversionModeForResolution(texture->w, texture->h)) {
                case SDL_YUV_CONVERSION_JPEG:
                    if (texturedata->yuv) {
                        shader = SHADER_YUV_JPEG;
                    } else if (texture->format == SDL_PIXELFORMAT_NV12) {
                        shader = SHADER_NV12_JPEG;
                    } else {
                        shader = SHADER_NV21_JPEG;
                    }
                    break;
                case SDL_YUV_CONVERSION_BT601:
                    if (texturedata->yuv) {
                        shader = SHADER_YUV_BT601;
                    } else if (texture->format == SDL_PIXELFORMAT_NV12) {
                        shader = SHADER_NV12_BT601;
                    } else {
                        shader = SHADER_NV21_BT601;
                    }
                    break;
                case SDL_YUV_CONVERSION_BT709:
                    if (texturedata->yuv) {
                        shader = SHADER_YUV_BT709;
                    } else if (texture->format == SDL_PIXELFORMAT_NV12) {
                        shader = SHADER_NV12_BT709;
                    } else {
                        shader = SHADER_NV21_BT709;
                    }
                    break;
                default:
                    SDL_assert(!"unsupported YUV conversion mode");
                    break;
            }
        }
    }

    SetDrawState(data, cmd, shader, current_color, current_blend, current_shader, current_texturing);

    if (texture != *current_texture) {
        const GLenum textype = data->textype;
        if (texturedata->yuv) {
            data->glActiveTextureARB(GL_TEXTURE2_ARB);
            data->glBindTexture(textype, texturedata->vtexture);

            data->glActiveTextureARB(GL_TEXTURE1_ARB);
            data->glBindTexture(textype, texturedata->utexture);
        }
        if (texturedata->nv12) {
            data->glActiveTextureARB(GL_TEXTURE1_ARB);
            data->glBindTexture(textype, texturedata->utexture);
        }
        data->glActiveTextureARB(GL_TEXTURE0_ARB);
        data->glBindTexture(textype, texturedata->texture);

        *current_texture = texture;
    }
}

static int
GL_RunCommandQueue(SDL_Renderer * renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    /* !!! FIXME: it'd be nice to use a vertex buffer instead of immediate mode... */
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;
    SDL_bool clipping_enabled = renderer->clipping_enabled;
    SDL_Rect viewport;
    SDL_Texture *bound_texture = NULL;
    SDL_BlendMode blend = SDL_BLENDMODE_INVALID;
    GL_Shader shader = SHADER_INVALID;
    int drawablew = 0, drawableh = 0;
    SDL_bool cliprect_enabled = SDL_FALSE;
    SDL_Rect cliprect;
    const SDL_bool istarget = renderer->target != NULL;
    Uint32 clear_color = ((renderer->a << 24) | (renderer->r << 16) | (renderer->g << 8) | renderer->b);
    Uint32 draw_color = clear_color;
    SDL_bool texturing = SDL_FALSE;
    size_t i;

    /* !!! FIXME: if we could guarantee the app isn't messing with the GL, too, we wouldn't
       !!! FIXME:  have to default a bunch of this state at the start. */

    if (GL_ActivateRenderer(renderer) < 0) {
        return -1;
    }

    if (!istarget) {
        SDL_GL_GetDrawableSize(renderer->window, &drawablew, &drawableh);
    }

    data->glClearColor((GLfloat) renderer->r * inv255f,
                       (GLfloat) renderer->g * inv255f,
                       (GLfloat) renderer->b * inv255f,
                       (GLfloat) renderer->a * inv255f);

    data->glColor4f((GLfloat) renderer->r * inv255f,
                    (GLfloat) renderer->g * inv255f,
                    (GLfloat) renderer->b * inv255f,
                    (GLfloat) renderer->a * inv255f);

    SDL_memcpy(&viewport, &renderer->viewport, sizeof (viewport));
    data->glMatrixMode(GL_PROJECTION);
    data->glLoadIdentity();
    data->glViewport(viewport.x,
            istarget ? viewport.y : (drawableh - viewport.y - viewport.h),
            viewport.w, viewport.h);
    if (viewport.w && viewport.h) {
        data->glOrtho((GLdouble) 0, (GLdouble) renderer->viewport.w,
                      (GLdouble) istarget ? 0 : renderer->viewport.h,
                      (GLdouble) istarget ? renderer->viewport.h : 0,
                      0.0, 1.0);
    }
    data->glMatrixMode(GL_MODELVIEW);
    data->glLoadIdentity();

    SDL_memcpy(&cliprect, &renderer->clip_rect, sizeof (cliprect));
    cliprect_enabled = renderer->clipping_enabled;
    if (cliprect_enabled) {
        data->glEnable(GL_SCISSOR_TEST);
    } else {
        data->glDisable(GL_SCISSOR_TEST);
    }

    data->glDisable(data->textype);

    data->glScissor(viewport.x + cliprect.x,
        istarget ? viewport.y + cliprect.y : drawableh - viewport.y - cliprect.y - cliprect.h,
        cliprect.w, cliprect.h);

    while (cmd) {
        switch (cmd->command) {
            case SDL_RENDERCMD_SETDRAWCOLOR: {
                // !!! FIXME: use this.
                break;
            }

            case SDL_RENDERCMD_SETVIEWPORT: {
                if (SDL_memcmp(&cmd->data.viewport.rect, &viewport, sizeof (viewport)) != 0) {
                    SDL_memcpy(&viewport, &cmd->data.viewport.rect, sizeof (viewport));
                    data->glMatrixMode(GL_PROJECTION);
                    data->glLoadIdentity();
                    data->glViewport(viewport.x,
                        istarget ? viewport.y : (drawableh - viewport.y - viewport.h),
                        viewport.w, viewport.h);
                    if (viewport.w && viewport.h) {
                        data->glOrtho((GLdouble) 0, (GLdouble) renderer->viewport.w,
                                      (GLdouble) istarget ? 0 : renderer->viewport.h,
                                      (GLdouble) istarget ? renderer->viewport.h : 0,
                                      0.0, 1.0);
                    }
                    data->glMatrixMode(GL_MODELVIEW);
                }
                break;
            }

            case SDL_RENDERCMD_SETCLIPRECT: {
                const SDL_Rect *rect = &cmd->data.cliprect.rect;
                const SDL_bool changed = (SDL_memcmp(&cliprect, rect, sizeof (SDL_Rect)) != 0);
                if (cliprect_enabled != cmd->data.cliprect.enabled) {
                    cliprect_enabled = cmd->data.cliprect.enabled;
                    if (cliprect_enabled) {
                        data->glEnable(GL_SCISSOR_TEST);
                    } else {
                        data->glDisable(GL_SCISSOR_TEST);
                    }
                }

                if (cliprect_enabled && changed) {
                    SDL_memcpy(&cliprect, rect, sizeof (SDL_Rect));
                    data->glScissor(viewport.x + rect->x,
                                    istarget ? viewport.y + rect->y : drawableh - viewport.y - rect->y - rect->h,
                                    rect->w, rect->h);
                }
                break;
            }

            case SDL_RENDERCMD_CLEAR: {
                const Uint8 r = cmd->data.color.r;
                const Uint8 g = cmd->data.color.g;
                const Uint8 b = cmd->data.color.b;
                const Uint8 a = cmd->data.color.a;
                const Uint32 color = ((a << 24) | (r << 16) | (g << 8) | b);
                if (color != clear_color) {
                    data->glClearColor((GLfloat) r * inv255f, (GLfloat) g * inv255f,
                                       (GLfloat) b * inv255f, (GLfloat) a * inv255f);
                    clear_color = color;
                }

                if (clipping_enabled) {
                    data->glDisable(GL_SCISSOR_TEST);
                }

                data->glClear(GL_COLOR_BUFFER_BIT);

                if (clipping_enabled) {
                    data->glEnable(GL_SCISSOR_TEST);
                }
                break;
            }

            case SDL_RENDERCMD_DRAW_POINTS: {
                const size_t count = cmd->data.draw.count;
                const GLfloat *verts = (GLfloat *) (((Uint8 *) vertices) + cmd->data.draw.first);
                SetDrawState(data, cmd, SHADER_SOLID, &draw_color, &blend, &shader, &texturing);
                data->glBegin(GL_POINTS);
                for (i = 0; i < count; i++, verts += 2) {
                    data->glVertex2f(verts[0], verts[1]);
                }
                data->glEnd();
                break;
            }

            case SDL_RENDERCMD_DRAW_LINES: {
                const GLfloat *verts = (GLfloat *) (((Uint8 *) vertices) + cmd->data.draw.first);
                size_t count = cmd->data.draw.count;
                SetDrawState(data, cmd, SHADER_SOLID, &draw_color, &blend, &shader, &texturing);
                if (count > 2 && (verts[0] == verts[(count-1)*2]) && (verts[1] == verts[(count*2)-1])) {
                    --count;  /* GL_LINE_LOOP takes care of the final segment */
                    data->glBegin(GL_LINE_LOOP);
                    for (i = 0; i < count; ++i, verts += 2) {
                        data->glVertex2f(verts[0], verts[1]);
                    }
                    data->glEnd();
                } else {
                    #if defined(__MACOSX__) || defined(__WIN32__)
                    #else
                    int x1, y1, x2, y2;
                    #endif

                    data->glBegin(GL_LINE_STRIP);
                    for (i = 0; i < count; ++i, verts += 2) {
                        data->glVertex2f(verts[0], verts[1]);
                    }
                    data->glEnd();

                    /* The line is half open, so we need one more point to complete it.
                     * http://www.opengl.org/documentation/specs/version1.1/glspec1.1/node47.html
                     * If we have to, we can use vertical line and horizontal line textures
                     * for vertical and horizontal lines, and then create custom textures
                     * for diagonal lines and software render those.  It's terrible, but at
                     * least it would be pixel perfect.
                     */

                    data->glBegin(GL_POINTS);
                    #if defined(__MACOSX__) || defined(__WIN32__)
                    /* Mac OS X and Windows seem to always leave the last point open */
                    data->glVertex2f(verts[(count-1)*2], verts[(count*2)-1]);
                    #else
                    /* Linux seems to leave the right-most or bottom-most point open */
                    x1 = verts[0];
                    y1 = verts[1];
                    x2 = verts[(count-1)*2];
                    y2 = verts[(count*2)-1];

                    if (x1 > x2) {
                        data->glVertex2f(x1, y1);
                    } else if (x2 > x1) {
                        data->glVertex2f(x2, y2);
                    }
                    if (y1 > y2) {
                        data->glVertex2f(x1, y1);
                    } else if (y2 > y1) {
                        data->glVertex2f(x2, y2);
                    }
                    #endif
                    data->glEnd();
                }
                break;
            }

            case SDL_RENDERCMD_FILL_RECTS: {
                const size_t count = cmd->data.draw.count;
                const GLfloat *verts = (GLfloat *) (((Uint8 *) vertices) + cmd->data.draw.first);
                SetDrawState(data, cmd, SHADER_SOLID, &draw_color, &blend, &shader, &texturing);
                for (i = 0; i < count; ++i, verts += 4) {
                    data->glRectf(verts[0], verts[1], verts[2], verts[3]);
                }
                break;
            }

            case SDL_RENDERCMD_COPY: {
                const GLfloat *verts = (GLfloat *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const GLfloat minx = verts[0];
                const GLfloat miny = verts[1];
                const GLfloat maxx = verts[2];
                const GLfloat maxy = verts[3];
                const GLfloat minu = verts[4];
                const GLfloat maxu = verts[5];
                const GLfloat minv = verts[6];
                const GLfloat maxv = verts[7];
                SetCopyState(data, cmd, &draw_color, &blend, &shader, &texturing, &bound_texture);
                data->glBegin(GL_TRIANGLE_STRIP);
                data->glTexCoord2f(minu, minv);
                data->glVertex2f(minx, miny);
                data->glTexCoord2f(maxu, minv);
                data->glVertex2f(maxx, miny);
                data->glTexCoord2f(minu, maxv);
                data->glVertex2f(minx, maxy);
                data->glTexCoord2f(maxu, maxv);
                data->glVertex2f(maxx, maxy);
                data->glEnd();
                break;
            }

            case SDL_RENDERCMD_COPY_EX: {
                const GLfloat *verts = (GLfloat *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const GLfloat minx = verts[0];
                const GLfloat miny = verts[1];
                const GLfloat maxx = verts[2];
                const GLfloat maxy = verts[3];
                const GLfloat minu = verts[4];
                const GLfloat maxu = verts[5];
                const GLfloat minv = verts[6];
                const GLfloat maxv = verts[7];
                const GLfloat translatex = verts[8];
                const GLfloat translatey = verts[9];
                const GLdouble angle = verts[10];
                SetCopyState(data, cmd, &draw_color, &blend, &shader, &texturing, &bound_texture);

                /* Translate to flip, rotate, translate to position */
                data->glPushMatrix();
                data->glTranslatef(translatex, translatey, 0.0f);
                data->glRotated(angle, 0.0, 0.0, 1.0);
                data->glBegin(GL_TRIANGLE_STRIP);
                data->glTexCoord2f(minu, minv);
                data->glVertex2f(minx, miny);
                data->glTexCoord2f(maxu, minv);
                data->glVertex2f(maxx, miny);
                data->glTexCoord2f(minu, maxv);
                data->glVertex2f(minx, maxy);
                data->glTexCoord2f(maxu, maxv);
                data->glVertex2f(maxx, maxy);
                data->glEnd();
                data->glPopMatrix();
                break;
            }

            case SDL_RENDERCMD_NO_OP:
                break;
        }

        cmd = cmd->next;
    }

    return GL_CheckError("", renderer);
}

static int
GL_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                    Uint32 pixel_format, void * pixels, int pitch)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;
    Uint32 temp_format = renderer->target ? renderer->target->format : SDL_PIXELFORMAT_ARGB8888;
    void *temp_pixels;
    int temp_pitch;
    GLint internalFormat;
    GLenum format, type;
    Uint8 *src, *dst, *tmp;
    int w, h, length, rows;
    int status;

    GL_ActivateRenderer(renderer);

    if (!convert_format(data, temp_format, &internalFormat, &format, &type)) {
        return SDL_SetError("Texture format %s not supported by OpenGL",
                            SDL_GetPixelFormatName(temp_format));
    }

    if (!rect->w || !rect->h) {
        return 0;  /* nothing to do. */
    }

    temp_pitch = rect->w * SDL_BYTESPERPIXEL(temp_format);
    temp_pixels = SDL_malloc(rect->h * temp_pitch);
    if (!temp_pixels) {
        return SDL_OutOfMemory();
    }

    SDL_GetRendererOutputSize(renderer, &w, &h);

    data->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    data->glPixelStorei(GL_PACK_ROW_LENGTH,
                        (temp_pitch / SDL_BYTESPERPIXEL(temp_format)));

    data->glReadPixels(rect->x, renderer->target ? rect->y : (h-rect->y)-rect->h,
                       rect->w, rect->h, format, type, temp_pixels);

    if (GL_CheckError("glReadPixels()", renderer) < 0) {
        SDL_free(temp_pixels);
        return -1;
    }

    /* Flip the rows to be top-down if necessary */
    if (!renderer->target) {
        length = rect->w * SDL_BYTESPERPIXEL(temp_format);
        src = (Uint8*)temp_pixels + (rect->h-1)*temp_pitch;
        dst = (Uint8*)temp_pixels;
        tmp = SDL_stack_alloc(Uint8, length);
        rows = rect->h / 2;
        while (rows--) {
            SDL_memcpy(tmp, dst, length);
            SDL_memcpy(dst, src, length);
            SDL_memcpy(src, tmp, length);
            dst += temp_pitch;
            src -= temp_pitch;
        }
        SDL_stack_free(tmp);
    }

    status = SDL_ConvertPixels(rect->w, rect->h,
                               temp_format, temp_pixels, temp_pitch,
                               pixel_format, pixels, pitch);
    SDL_free(temp_pixels);

    return status;
}

static void
GL_RenderPresent(SDL_Renderer * renderer)
{
    GL_ActivateRenderer(renderer);

    SDL_GL_SwapWindow(renderer->window);
}

static void
GL_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    GL_RenderData *renderdata = (GL_RenderData *) renderer->driverdata;
    GL_TextureData *data = (GL_TextureData *) texture->driverdata;

    GL_ActivateRenderer(renderer);

    if (!data) {
        return;
    }
    if (data->texture) {
        renderdata->glDeleteTextures(1, &data->texture);
    }
    if (data->yuv) {
        renderdata->glDeleteTextures(1, &data->utexture);
        renderdata->glDeleteTextures(1, &data->vtexture);
    }
    SDL_free(data->pixels);
    SDL_free(data);
    texture->driverdata = NULL;
}

static void
GL_DestroyRenderer(SDL_Renderer * renderer)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;

    if (data) {
        if (data->context != NULL) {
            /* make sure we delete the right resources! */
            GL_ActivateRenderer(renderer);
        }

        GL_ClearErrors(renderer);
        if (data->GL_ARB_debug_output_supported) {
            PFNGLDEBUGMESSAGECALLBACKARBPROC glDebugMessageCallbackARBFunc = (PFNGLDEBUGMESSAGECALLBACKARBPROC) SDL_GL_GetProcAddress("glDebugMessageCallbackARB");

            /* Uh oh, we don't have a safe way of removing ourselves from the callback chain, if it changed after we set our callback. */
            /* For now, just always replace the callback with the original one */
            glDebugMessageCallbackARBFunc(data->next_error_callback, data->next_error_userparam);
        }
        if (data->shaders) {
            GL_DestroyShaderContext(data->shaders);
        }
        if (data->context) {
            while (data->framebuffers) {
                GL_FBOList *nextnode = data->framebuffers->next;
                /* delete the framebuffer object */
                data->glDeleteFramebuffersEXT(1, &data->framebuffers->FBO);
                GL_CheckError("", renderer);
                SDL_free(data->framebuffers);
                data->framebuffers = nextnode;
            }
            SDL_GL_DeleteContext(data->context);
        }
        SDL_free(data);
    }
    SDL_free(renderer);
}

static int
GL_BindTexture (SDL_Renderer * renderer, SDL_Texture *texture, float *texw, float *texh)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;
    GL_TextureData *texturedata = (GL_TextureData *) texture->driverdata;
    const GLenum textype = data->textype;

    GL_ActivateRenderer(renderer);

    data->glEnable(textype);
    if (texturedata->yuv) {
        data->glActiveTextureARB(GL_TEXTURE2_ARB);
        data->glBindTexture(textype, texturedata->vtexture);

        data->glActiveTextureARB(GL_TEXTURE1_ARB);
        data->glBindTexture(textype, texturedata->utexture);

        data->glActiveTextureARB(GL_TEXTURE0_ARB);
    }
    data->glBindTexture(textype, texturedata->texture);

    if(texw) *texw = (float)texturedata->texw;
    if(texh) *texh = (float)texturedata->texh;

    return 0;
}

static int
GL_UnbindTexture (SDL_Renderer * renderer, SDL_Texture *texture)
{
    GL_RenderData *data = (GL_RenderData *) renderer->driverdata;
    GL_TextureData *texturedata = (GL_TextureData *) texture->driverdata;
    const GLenum textype = data->textype;

    GL_ActivateRenderer(renderer);

    if (texturedata->yuv) {
        data->glActiveTextureARB(GL_TEXTURE2_ARB);
        data->glDisable(textype);

        data->glActiveTextureARB(GL_TEXTURE1_ARB);
        data->glDisable(textype);

        data->glActiveTextureARB(GL_TEXTURE0_ARB);
    }

    data->glDisable(textype);

    return 0;
}

#endif /* SDL_VIDEO_RENDER_OGL && !SDL_RENDER_DISABLED */

/* vi: set ts=4 sw=4 expandtab: */
