/*
 * This file is part of the DXX-Rebirth project <https://www.dxx-rebirth.com/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Remaster renderer: optional shader-based world rendering and
 * post-processing layered on top of the classic OpenGL renderer.
 *
 * Everything here targets OpenGL 2.1 + GLSL 1.20 + framebuffer objects,
 * which is what macOS provides in its legacy (compatibility) context on
 * both Intel and Apple Silicon.  If any piece is unavailable at runtime
 * the classic fixed-function path is used unchanged.
 */
#pragma once

#include "dxxsconf.h"
#if DXX_USE_OGL
#include "gr.h"
#include "ogl_init.h"

#ifdef DXX_BUILD_DESCENT
namespace dsx {

enum class remaster_draw : uint8_t
{
	world,		// walls, overlays and textured polymodel faces
	sprite,		// billboards: fireballs, powerups, weapon blobs
};

/* Called once the OpenGL context exists. */
void ogl_remaster_init();
/* Called when the OpenGL context is going away (or textures are smashed). */
void ogl_remaster_reset_textures();

/* Texture upload hook.  Returns true if a high resolution replacement was
 * found for `bm` and uploaded into `tex`; the caller must then skip its own
 * upload.  `tex.w`/`tex.h` must already hold the original bitmap size.
 */
bool ogl_remaster_load_override(const grs_bitmap &bm, ogl_texture &tex);

/* Bracket a textured draw.  Texture unit 0 must already have the bitmap's
 * texture bound.  Safe to call when the remaster renderer is disabled.
 */
void ogl_remaster_begin_textured(const grs_bitmap &bm, remaster_draw kind);
void ogl_remaster_end_textured();

/* Bracket a 3D frame (called from ogl_start_frame / ogl_end_frame). */
void ogl_remaster_begin_frame(const grs_canvas &canvas);
void ogl_remaster_end_frame();

}
#endif
#endif
