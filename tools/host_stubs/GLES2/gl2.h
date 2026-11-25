/*
 * Host-only compile shim for <GLES2/gl2.h>.
 *
 * The Android app includes the NDK's GLES2 header, which does not exist
 * on a desktop host. The vendored SDL tree already ships the Khronos
 * generated GLES2 headers, so desktop test harnesses point -I here and
 * get the exact same declarations. This directory is test-only: it is
 * never part of the APK (Android.mk does not reference it).
 */

#ifndef JFA_HOST_GLES2_GL2_H
#define JFA_HOST_GLES2_GL2_H

/* The Khronos header below expects the platform calling-convention
 * macros to be provided by gl2platform.h, which the vendored SDL copy
 * omits. Desktop C needs no special convention. */
#ifndef GL_APIENTRY
#define GL_APIENTRY
#endif
#ifndef GL_APIENTRYP
#define GL_APIENTRYP
#endif
#ifndef GL_APICALL
#define GL_APICALL
#endif
#ifndef GL_API
#define GL_API
#endif

#include "SDL_opengles2_khrplatform.h"
#include "SDL_opengles2_gl2.h"

#endif /* JFA_HOST_GLES2_GL2_H */
