// OpenGL ES 3.0 / WebGL 2 access: direct on Emscripten, loaded through SDL on desktop (an ES 3 context, or a
// desktop GL 3.3 core context when the driver offers no ES).
#pragma once
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#define GL_LOAD_ALL() 1
#else
#define GL_GLES_PROTOTYPES 0
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#include <SDL_opengles2.h>
#include <stdint.h>
// GLES 3.0 names that SDL's GLES 2 headers lack
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#define GL_R8 0x8229
#define GL_RG 0x8227
#define GL_RG8 0x822B
#define GL_RGBA8 0x8058
#define GL_RGB8 0x8051
#define GL_RGB565 0x8D62
#define GL_RGB5_A1 0x8057
#define GL_RGBA4 0x8056
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_NUM_EXTENSIONS 0x821D
#define GL_MAX 0x8008
#define GL_MIN 0x8007
#define GL_STREAM_DRAW 0x88E0
#define GL_MIRRORED_REPEAT 0x8370
#define GL_HALF_FLOAT 0x140B

#define GL_FUNCS(X) \
    X(glGenTextures, void, (GLsizei, GLuint *)) X(glDeleteTextures, void, (GLsizei, const GLuint *)) \
    X(glBindTexture, void, (GLenum, GLuint)) \
    X(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(glCompressedTexImage2D, void, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(glCompressedTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(glTexParameteri, void, (GLenum, GLenum, GLint)) X(glTexParameterf, void, (GLenum, GLenum, GLfloat)) \
    X(glActiveTexture, void, (GLenum)) X(glGenBuffers, void, (GLsizei, GLuint *)) \
    X(glDeleteBuffers, void, (GLsizei, const GLuint *)) X(glBindBuffer, void, (GLenum, GLuint)) \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(glBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(glGenFramebuffers, void, (GLsizei, GLuint *)) X(glDeleteFramebuffers, void, (GLsizei, const GLuint *)) \
    X(glBindFramebuffer, void, (GLenum, GLuint)) \
    X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(glFramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint)) \
    X(glGenRenderbuffers, void, (GLsizei, GLuint *)) X(glBindRenderbuffer, void, (GLenum, GLuint)) \
    X(glRenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(glDeleteRenderbuffers, void, (GLsizei, const GLuint *)) X(glCheckFramebufferStatus, GLenum, (GLenum)) \
    X(glCreateShader, GLuint, (GLenum)) X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(glCompileShader, void, (GLuint)) X(glGetShaderiv, void, (GLuint, GLenum, GLint *)) \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) X(glCreateProgram, GLuint, (void)) \
    X(glAttachShader, void, (GLuint, GLuint)) X(glLinkProgram, void, (GLuint)) \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *)) X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(glUseProgram, void, (GLuint)) X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *)) \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *)) X(glUniform1i, void, (GLint, GLint)) \
    X(glUniform1f, void, (GLint, GLfloat)) X(glUniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glUniform4fv, void, (GLint, GLsizei, const GLfloat *)) X(glUniform3fv, void, (GLint, GLsizei, const GLfloat *)) \
    X(glUniform1fv, void, (GLint, GLsizei, const GLfloat *)) \
    X(glUniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(glVertexAttribIPointer, void, (GLuint, GLint, GLenum, GLsizei, const void *)) \
    X(glEnableVertexAttribArray, void, (GLuint)) X(glDisableVertexAttribArray, void, (GLuint)) \
    X(glVertexAttrib4f, void, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei)) X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *)) \
    X(glViewport, void, (GLint, GLint, GLsizei, GLsizei)) X(glScissor, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(glEnable, void, (GLenum)) X(glDisable, void, (GLenum)) \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum)) X(glBlendEquation, void, (GLenum)) \
    X(glDepthFunc, void, (GLenum)) X(glDepthMask, void, (GLboolean)) X(glDepthRangef, void, (GLfloat, GLfloat)) \
    X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat)) X(glClear, void, (GLbitfield)) \
    X(glClearDepthf, void, (GLfloat)) X(glClearStencil, void, (GLint)) \
    X(glColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(glPixelStorei, void, (GLenum, GLint)) X(glGetError, GLenum, (void)) X(glGetString, const GLubyte *, (GLenum)) \
    X(glGetStringi, const GLubyte *, (GLenum, GLuint)) \
    X(glFinish, void, (void)) X(glFlush, void, (void)) X(glDeleteShader, void, (GLuint)) X(glDeleteProgram, void, (GLuint)) \
    X(glGenerateMipmap, void, (GLenum)) X(glGetIntegerv, void, (GLenum, GLint *)) \
    X(glCullFace, void, (GLenum)) X(glFrontFace, void, (GLenum)) \
    X(glStencilFunc, void, (GLenum, GLint, GLuint)) X(glStencilOp, void, (GLenum, GLenum, GLenum)) \
    X(glStencilMask, void, (GLuint)) X(glPolygonOffset, void, (GLfloat, GLfloat)) \
    X(glGenVertexArrays, void, (GLsizei, GLuint *)) X(glBindVertexArray, void, (GLuint)) \
    X(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(glGenSamplers, void, (GLsizei, GLuint *)) X(glBindSampler, void, (GLuint, GLuint)) \
    X(glSamplerParameteri, void, (GLuint, GLenum, GLint)) X(glSamplerParameterf, void, (GLuint, GLenum, GLfloat)) \
    X(glCopyTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))

#define GL_DECL(n, r, a) typedef r (GL_APIENTRY *n##_fn) a; extern n##_fn p_##n;
GL_FUNCS(GL_DECL)
#undef GL_DECL
int gl_load_all(void);
#define GL_LOAD_ALL() gl_load_all()

#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glCompressedTexSubImage2D p_glCompressedTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glTexParameterf p_glTexParameterf
#define glActiveTexture p_glActiveTexture
#define glGenBuffers p_glGenBuffers
#define glDeleteBuffers p_glDeleteBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glGenFramebuffers p_glGenFramebuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glBindAttribLocation p_glBindAttribLocation
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform4f p_glUniform4f
#define glUniform4fv p_glUniform4fv
#define glUniform3fv p_glUniform3fv
#define glUniform1fv p_glUniform1fv
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glVertexAttribPointer p_glVertexAttribPointer
#define glVertexAttribIPointer p_glVertexAttribIPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttrib4f p_glVertexAttrib4f
#define glDrawArrays p_glDrawArrays
#define glDrawElements p_glDrawElements
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendEquation p_glBlendEquation
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glDepthRangef p_glDepthRangef
#define glClearColor p_glClearColor
#define glClear p_glClear
#define glClearDepthf p_glClearDepthf
#define glClearStencil p_glClearStencil
#define glColorMask p_glColorMask
#define glReadPixels p_glReadPixels
#define glPixelStorei p_glPixelStorei
#define glGetError p_glGetError
#define glGetString p_glGetString
#define glGetStringi p_glGetStringi
#define glFinish p_glFinish
#define glFlush p_glFlush
#define glDeleteShader p_glDeleteShader
#define glDeleteProgram p_glDeleteProgram
#define glGenerateMipmap p_glGenerateMipmap
#define glGetIntegerv p_glGetIntegerv
#define glCullFace p_glCullFace
#define glFrontFace p_glFrontFace
#define glStencilFunc p_glStencilFunc
#define glStencilOp p_glStencilOp
#define glStencilMask p_glStencilMask
#define glPolygonOffset p_glPolygonOffset
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glBlitFramebuffer p_glBlitFramebuffer
#define glGenSamplers p_glGenSamplers
#define glBindSampler p_glBindSampler
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterf p_glSamplerParameterf
#define glCopyTexSubImage2D p_glCopyTexSubImage2D
#endif
