/*
 * ladybird9 plan9 compat header: upstream Ladybird builds against ANGLE,
 * whose <GLES2/gl2ext_angle.h> carries ANGLE-only entry points. On Plan 9
 * "angle" is satisfied by gl9 (real Mesa GLES, see angle.pc): Mesa's Khronos
 * gl2ext.h already defines the registered GL_ANGLE_* extensions, so this
 * shim only supplies the ANGLE-private surface Ladybird's WebGL glue
 * references:
 *
 *  - GL_ANGLE_robust_client_memory (gl*RobustANGLE): implemented inline in
 *    terms of the core GLES3 calls. The extension's extra parameters are
 *    client-side bounds (bufSize) and out-counts (length); Ladybird's
 *    generated glue validates sizes before calling, and every in-tree
 *    caller passes length=nullptr except readPixels (handled below).
 *  - GL_ANGLE_instanced_arrays entry points: Mesa exports the core GLES3
 *    names, not the ANGLE aliases, so alias via macro.
 *  - glRequestExtensionANGLE: no-op; Mesa does not advertise
 *    GL_ANGLE_request_extension, so nothing may be requested.
 *
 * WebGL on gl9 is a ledgered deferral: see parity/deferrals.md (WebGL row).
 */
#ifndef GL2EXT_ANGLE_H_
#define GL2EXT_ANGLE_H_

#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL_ANGLE_instanced_arrays: Mesa exports the core symbols only. */
#define glDrawArraysInstancedANGLE glDrawArraysInstanced
#define glDrawElementsInstancedANGLE glDrawElementsInstanced
#define glVertexAttribDivisorANGLE glVertexAttribDivisor

/* GLES2-era extension aliases Ladybird's GL glue calls: Mesa exports the
 * GLES3 core names, not the OES/EXT suffixes. */
#define glBindVertexArrayOES glBindVertexArray
#define glDeleteVertexArraysOES glDeleteVertexArrays
#define glGenVertexArraysOES glGenVertexArrays
#define glIsVertexArrayOES glIsVertexArray
#define glDrawBuffersEXT glDrawBuffers

/* GL_ANGLE_request_extension */
static inline void glRequestExtensionANGLE(const GLchar* name)
{
    (void)name; /* Mesa exposes what it exposes; nothing to enable. */
}

/* GL_ANGLE_robust_client_memory.
 * length out-params: ANGLE reports the number of values written. Mesa can't
 * tell us; every in-tree Ladybird caller passes nullptr, so report 0 when
 * asked (callers that pass a pointer must not rely on it here). */
static inline void glGetBooleanvRobustANGLE(GLenum pname, GLsizei bufSize, GLsizei* length, GLboolean* data)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetBooleanv(pname, data);
}
static inline void glGetFloatvRobustANGLE(GLenum pname, GLsizei bufSize, GLsizei* length, GLfloat* data)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetFloatv(pname, data);
}
static inline void glGetIntegervRobustANGLE(GLenum pname, GLsizei bufSize, GLsizei* length, GLint* data)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetIntegerv(pname, data);
}
static inline void glGetInteger64vRobustANGLE(GLenum pname, GLsizei bufSize, GLsizei* length, GLint64* data)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetInteger64v(pname, data);
}
static inline void glGetActiveUniformBlockivRobustANGLE(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetActiveUniformBlockiv(program, uniformBlockIndex, pname, params);
}
static inline void glGetBufferParameterivRobustANGLE(GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetBufferParameteriv(target, pname, params);
}
static inline void glGetInternalformativRobustANGLE(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    if (length)
        *length = 0;
    glGetInternalformativ(target, internalformat, pname, bufSize, params);
}
static inline void glGetProgramivRobustANGLE(GLuint program, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetProgramiv(program, pname, params);
}
static inline void glGetQueryObjectuivRobustANGLE(GLuint id, GLenum pname, GLsizei bufSize, GLsizei* length, GLuint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetQueryObjectuiv(id, pname, params);
}
static inline void glGetRenderbufferParameterivRobustANGLE(GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetRenderbufferParameteriv(target, pname, params);
}
static inline void glGetShaderivRobustANGLE(GLuint shader, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetShaderiv(shader, pname, params);
}
static inline void glGetTexParameterfvRobustANGLE(GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLfloat* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetTexParameterfv(target, pname, params);
}
static inline void glGetTexParameterivRobustANGLE(GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetTexParameteriv(target, pname, params);
}
static inline void glGetVertexAttribfvRobustANGLE(GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, GLfloat* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetVertexAttribfv(index, pname, params);
}
static inline void glGetVertexAttribivRobustANGLE(GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetVertexAttribiv(index, pname, params);
}
static inline void glGetVertexAttribPointervRobustANGLE(GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, void** pointer)
{
    (void)bufSize;
    if (length)
        *length = 0;
    glGetVertexAttribPointerv(index, pname, pointer);
}

/* Ladybird's readPixels expects length = bytes written (it copies exactly
 * that many out). Compute it for the format/type pairs WebGL permits;
 * row strides here are naturally packed (callers size the buffer the same
 * way), and glReadPixels either fills the rect or records a GL error. */
static inline GLsizei gl9_read_pixels_bytes_(GLsizei width, GLsizei height, GLenum format, GLenum type)
{
    GLsizei components;
    GLsizei component_size;
    switch (format) {
    case GL_RGBA:
    case GL_RGBA_INTEGER:
        components = 4;
        break;
    case GL_RGB:
    case GL_RGB_INTEGER:
        components = 3;
        break;
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA:
        components = 2;
        break;
    default:
        components = 1;
        break;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
        component_size = 1;
        break;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT:
        component_size = 2;
        break;
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        /* packed: one 16-bit unit per pixel */
        return width * height * 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        return width * height * 4;
    default:
        component_size = 4;
        break;
    }
    return width * height * components * component_size;
}

static inline void glReadPixelsRobustANGLE(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels)
{
    GLsizei needed = gl9_read_pixels_bytes_(width, height, format, type);
    if (needed > bufSize) {
        /* ANGLE raises GL_INVALID_OPERATION rather than overflowing. */
        if (length)
            *length = 0;
        return;
    }
    glReadPixels(x, y, width, height, format, type, pixels);
    if (length)
        *length = needed;
    if (columns)
        *columns = width;
    if (rows)
        *rows = height;
}

static inline void glTexImage2DRobustANGLE(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, GLsizei bufSize, const void* pixels)
{
    (void)bufSize;
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
static inline void glTexImage3DRobustANGLE(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, GLsizei bufSize, const void* pixels)
{
    (void)bufSize;
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}
static inline void glTexSubImage2DRobustANGLE(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, const void* pixels)
{
    (void)bufSize;
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}
static inline void glTexSubImage3DRobustANGLE(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, const void* pixels)
{
    (void)bufSize;
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}
static inline void glCompressedTexImage2DRobustANGLE(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, GLsizei bufSize, const void* data)
{
    (void)bufSize;
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}
static inline void glCompressedTexImage3DRobustANGLE(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, GLsizei bufSize, const void* data)
{
    (void)bufSize;
    glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data);
}
static inline void glCompressedTexSubImage2DRobustANGLE(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, GLsizei bufSize, const void* data)
{
    (void)bufSize;
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}
static inline void glCompressedTexSubImage3DRobustANGLE(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, GLsizei bufSize, const void* data)
{
    (void)bufSize;
    glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
}

#ifdef __cplusplus
}
#endif

#endif /* GL2EXT_ANGLE_H_ */
