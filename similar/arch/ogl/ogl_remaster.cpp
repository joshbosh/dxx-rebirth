/*
 * This file is part of the DXX-Rebirth project <https://www.dxx-rebirth.com/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Remaster renderer.  See ogl_remaster.h for the overview.
 *
 * Pieces:
 *  - High resolution texture replacements, loaded by PIG bitmap name from
 *    "hires/<name>.png" anywhere in the PhysFS search path (so a .dxa
 *    archive next to the game data, or the app bundle's Resources folder).
 *  - Per-bitmap materials ("remaster/textures/<name>.material" and
 *    "<name>_emissive.png", the format used by DXX-Raytracer) that make
 *    light fixtures and glowing details emissive.
 *  - A GLSL 1.20 world shader: vertex lighting as before, plus emissive
 *    output and a subtle view-aligned specular on metal surfaces.
 *  - A post chain on each 3D view: SSAO, bloom, highlight-preserving
 *    tonemap, vignette and FXAA.
 *
 * Environment overrides (all default on):
 *   DXX_REMASTER=0          disable everything (classic renderer)
 *   DXX_REMASTER_POST=0     disable the post chain only
 *   DXX_REMASTER_HIRES=0    ignore high resolution textures
 *   DXX_REMASTER_AO=0       disable ambient occlusion
 */

#include "dxxsconf.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ogl_init.h"
#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#else
#include <GL/gl.h>
#include <GL/glu.h>
#endif

#include <SDL.h>
#if DXX_USE_SDLIMAGE
#include <SDL_image.h>
#endif

#include "ogl_remaster.h"
#include "ogl_extensions.h"
#include "internal.h"
#include "physfsrwops.h"
#include "physfsx.h"
#include "piggy.h"
#include "bm.h"
#include "textures.h"
#include "effects.h"
#include "vclip.h"
#include "console.h"
#include "gr.h"
#include <algorithm>

/* Enum values not guaranteed by every platform's legacy gl.h. */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace dsx {
namespace {

/* ------------------------------------------------------------------ */
/* GL 2.0 / FBO entry points, loaded through SDL so the same code works */
/* on macOS, Linux and Windows without depending on glext prototypes.  */

using PFN_CreateShader = GLuint (APIENTRY *)(GLenum);
using PFN_ShaderSource = void (APIENTRY *)(GLuint, GLsizei, const char *const *, const GLint *);
using PFN_CompileShader = void (APIENTRY *)(GLuint);
using PFN_GetShaderiv = void (APIENTRY *)(GLuint, GLenum, GLint *);
using PFN_GetShaderInfoLog = void (APIENTRY *)(GLuint, GLsizei, GLsizei *, char *);
using PFN_DeleteShader = void (APIENTRY *)(GLuint);
using PFN_CreateProgram = GLuint (APIENTRY *)();
using PFN_AttachShader = void (APIENTRY *)(GLuint, GLuint);
using PFN_LinkProgram = void (APIENTRY *)(GLuint);
using PFN_GetProgramiv = void (APIENTRY *)(GLuint, GLenum, GLint *);
using PFN_GetProgramInfoLog = void (APIENTRY *)(GLuint, GLsizei, GLsizei *, char *);
using PFN_UseProgram = void (APIENTRY *)(GLuint);
using PFN_GetUniformLocation = GLint (APIENTRY *)(GLuint, const char *);
using PFN_Uniform1i = void (APIENTRY *)(GLint, GLint);
using PFN_Uniform1f = void (APIENTRY *)(GLint, GLfloat);
using PFN_Uniform2f = void (APIENTRY *)(GLint, GLfloat, GLfloat);
using PFN_Uniform4f = void (APIENTRY *)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_ActiveTexture = void (APIENTRY *)(GLenum);
using PFN_GenFramebuffers = void (APIENTRY *)(GLsizei, GLuint *);
using PFN_DeleteFramebuffers = void (APIENTRY *)(GLsizei, const GLuint *);
using PFN_BindFramebuffer = void (APIENTRY *)(GLenum, GLuint);
using PFN_FramebufferTexture2D = void (APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_CheckFramebufferStatus = GLenum (APIENTRY *)(GLenum);

struct gl_api
{
	PFN_CreateShader CreateShader;
	PFN_ShaderSource ShaderSource;
	PFN_CompileShader CompileShader;
	PFN_GetShaderiv GetShaderiv;
	PFN_GetShaderInfoLog GetShaderInfoLog;
	PFN_DeleteShader DeleteShader;
	PFN_CreateProgram CreateProgram;
	PFN_AttachShader AttachShader;
	PFN_LinkProgram LinkProgram;
	PFN_GetProgramiv GetProgramiv;
	PFN_GetProgramInfoLog GetProgramInfoLog;
	PFN_UseProgram UseProgram;
	PFN_GetUniformLocation GetUniformLocation;
	PFN_Uniform1i Uniform1i;
	PFN_Uniform1f Uniform1f;
	PFN_Uniform2f Uniform2f;
	PFN_Uniform4f Uniform4f;
	PFN_ActiveTexture ActiveTexture;
	PFN_GenFramebuffers GenFramebuffers;
	PFN_DeleteFramebuffers DeleteFramebuffers;
	PFN_BindFramebuffer BindFramebuffer;
	PFN_FramebufferTexture2D FramebufferTexture2D;
	PFN_CheckFramebufferStatus CheckFramebufferStatus;
};

gl_api gl{};

template <typename F>
bool load_proc(F &out, const char *name, const char *alt = nullptr)
{
	void *p = SDL_GL_GetProcAddress(name);
	if (!p && alt)
		p = SDL_GL_GetProcAddress(alt);
	out = reinterpret_cast<F>(p);
	return p != nullptr;
}

bool load_gl_api()
{
	bool ok = true;
	ok &= load_proc(gl.CreateShader, "glCreateShader", "glCreateShaderObjectARB");
	ok &= load_proc(gl.ShaderSource, "glShaderSource", "glShaderSourceARB");
	ok &= load_proc(gl.CompileShader, "glCompileShader", "glCompileShaderARB");
	ok &= load_proc(gl.GetShaderiv, "glGetShaderiv");
	ok &= load_proc(gl.GetShaderInfoLog, "glGetShaderInfoLog");
	ok &= load_proc(gl.DeleteShader, "glDeleteShader");
	ok &= load_proc(gl.CreateProgram, "glCreateProgram", "glCreateProgramObjectARB");
	ok &= load_proc(gl.AttachShader, "glAttachShader", "glAttachObjectARB");
	ok &= load_proc(gl.LinkProgram, "glLinkProgram", "glLinkProgramARB");
	ok &= load_proc(gl.GetProgramiv, "glGetProgramiv");
	ok &= load_proc(gl.GetProgramInfoLog, "glGetProgramInfoLog");
	ok &= load_proc(gl.UseProgram, "glUseProgram", "glUseProgramObjectARB");
	ok &= load_proc(gl.GetUniformLocation, "glGetUniformLocation", "glGetUniformLocationARB");
	ok &= load_proc(gl.Uniform1i, "glUniform1i", "glUniform1iARB");
	ok &= load_proc(gl.Uniform1f, "glUniform1f", "glUniform1fARB");
	ok &= load_proc(gl.Uniform2f, "glUniform2f", "glUniform2fARB");
	ok &= load_proc(gl.Uniform4f, "glUniform4f", "glUniform4fARB");
	ok &= load_proc(gl.ActiveTexture, "glActiveTexture", "glActiveTextureARB");
	return ok;
}

bool load_fbo_api()
{
	bool ok = true;
	ok &= load_proc(gl.GenFramebuffers, "glGenFramebuffers", "glGenFramebuffersEXT");
	ok &= load_proc(gl.DeleteFramebuffers, "glDeleteFramebuffers", "glDeleteFramebuffersEXT");
	ok &= load_proc(gl.BindFramebuffer, "glBindFramebuffer", "glBindFramebufferEXT");
	ok &= load_proc(gl.FramebufferTexture2D, "glFramebufferTexture2D", "glFramebufferTexture2DEXT");
	ok &= load_proc(gl.CheckFramebufferStatus, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
	return ok;
}

bool env_enabled(const char *name)
{
	const char *v = getenv(name);
	return !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F'));
}

/* ------------------------------------------------------------------ */
/* Shaders                                                             */

constexpr char world_vs[] = R"GLSL(
#version 120
varying vec2 v_uv;
varying vec4 v_col;
varying vec3 v_pos;
void main()
{
	v_uv = gl_MultiTexCoord0.xy;
	v_col = gl_Color;
	v_pos = (gl_ModelViewMatrix * gl_Vertex).xyz;
	gl_Position = ftransform();
}
)GLSL";

/* u_mode: 0 none, 1 emissive mask texture, 2 luminance-keyed (light
 * fixtures without a mask), 3 fully emissive (unlit sprites).
 */
constexpr char world_fs[] = R"GLSL(
#version 120
uniform sampler2D u_tex;
uniform sampler2D u_emis;
uniform float u_mode;
uniform float u_emis_strength;
uniform float u_metal;
uniform float u_rough;
uniform float u_lit;
uniform float u_detail;
varying vec2 v_uv;
varying vec4 v_col;
varying vec3 v_pos;
float hash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float vnoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
	           mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
void main()
{
	vec4 t = texture2D(u_tex, v_uv);
	if (u_detail > 0.0)
	{
		/* Detail layer: walls are magnified ~10x on screen, far beyond
		 * what any upscale holds.  Fine grain (4 cells per original texel)
		 * fades out as soon as it would start to shimmer.
		 */
		vec2 dp = v_uv * 256.0;
		float fade = clamp(1.5 - length(fwidth(dp)) * 1.2, 0.0, 1.0);
		float n = (vnoise(dp) - 0.5) * 0.65 + (vnoise(dp * 2.7 + 17.0) - 0.5) * 0.35;
		t.rgb *= 1.0 + n * u_detail * fade;
	}
	vec3 light = v_col.rgb;
	vec3 c = t.rgb * light;

	if (u_lit > 0.5)
	{
		/* View-aligned specular: the ship's own light glinting off metal.
		 * Scaled by the existing vertex light so dark rooms stay dark.
		 */
		vec3 n = normalize(cross(dFdx(v_pos), dFdy(v_pos)));
		vec3 v = normalize(-v_pos);
		float ndv = abs(dot(n, v));
		float gloss = 1.0 - u_rough;
		float spec = pow(ndv, mix(6.0, 48.0, gloss)) * gloss * gloss;
		vec3 tint = mix(vec3(1.0), t.rgb, u_metal);
		float dist_fade = clamp(1.0 - length(v_pos) / 120.0, 0.0, 1.0);
		c += tint * spec * dist_fade * mix(0.10, 0.40, u_metal) * light;
	}

	vec3 e = vec3(0.0);
	if (u_mode > 2.5)
		e = t.rgb;
	else if (u_mode > 1.5)
	{
		float l = dot(t.rgb, vec3(0.299, 0.587, 0.114));
		e = t.rgb * smoothstep(0.45, 0.85, l);
	}
	else if (u_mode > 0.5)
		e = t.rgb * texture2D(u_emis, v_uv).r;
	c += e * u_emis_strength;
	gl_FragColor = vec4(c, t.a * v_col.a);
}
)GLSL";

constexpr char quad_vs[] = R"GLSL(
#version 120
varying vec2 v_uv;
void main()
{
	v_uv = gl_Vertex.xy;
	gl_Position = vec4(gl_Vertex.xy * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

/* Screen-space ambient occlusion from the depth buffer, at half
 * resolution.  The projection is the game's gluPerspective(90, 1, near,
 * far), so view-space x/y are simply ndc * linear depth.
 */
constexpr char ssao_fs[] = R"GLSL(
#version 120
uniform sampler2D u_depth;
uniform vec2 u_near_far;
uniform vec2 u_texel;
uniform float u_radius;
varying vec2 v_uv;
float lin(float d)
{
	float n = u_near_far.x, f = u_near_far.y;
	float z = d * 2.0 - 1.0;
	return 2.0 * n * f / (f + n - z * (f - n));
}
vec3 viewpos(vec2 uv)
{
	float z = lin(texture2D(u_depth, uv).r);
	return vec3((uv * 2.0 - 1.0) * z, z);
}
void main()
{
	float d = texture2D(u_depth, v_uv).r;
	if (d >= 0.99999)
	{
		gl_FragColor = vec4(1.0);
		return;
	}
	vec3 p = viewpos(v_uv);
	vec3 px = viewpos(v_uv + vec2(u_texel.x, 0.0)) - p;
	vec3 nx = p - viewpos(v_uv - vec2(u_texel.x, 0.0));
	vec3 py = viewpos(v_uv + vec2(0.0, u_texel.y)) - p;
	vec3 ny = p - viewpos(v_uv - vec2(0.0, u_texel.y));
	/* Pick the smaller difference on each axis to avoid smearing across
	 * depth discontinuities.
	 */
	vec3 dx = dot(px, px) < dot(nx, nx) ? px : nx;
	vec3 dy = dot(py, py) < dot(ny, ny) ? py : ny;
	vec3 n = normalize(cross(dx, dy));
	if (dot(n, -p) < 0.0)
		n = -n;

	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float ang0 = noise * 6.2831853;
	float rad = clamp(u_radius / p.z, 0.004, 0.12);
	float occ = 0.0;
	const int N = 12;
	for (int i = 0; i < N; ++i)
	{
		float fi = (float(i) + 0.5) / float(N);
		float a = ang0 + float(i) * 2.3999632;
		vec2 off = vec2(cos(a), sin(a)) * rad * sqrt(fi);
		vec3 s = viewpos(v_uv + off);
		vec3 v = s - p;
		float dist = length(v);
		float w = 1.0 - smoothstep(u_radius * 0.5, u_radius * 1.5, dist);
		occ += max(dot(n, v / max(dist, 1e-4)) - 0.08, 0.0) * w;
	}
	float ao = clamp(1.0 - 1.25 * occ / float(N), 0.0, 1.0);
	gl_FragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";

/* Depth-aware separable blur for the AO buffer. */
constexpr char blur_fs[] = R"GLSL(
#version 120
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform vec2 u_dir;
uniform vec2 u_near_far;
varying vec2 v_uv;
float lin(float d)
{
	float n = u_near_far.x, f = u_near_far.y;
	float z = d * 2.0 - 1.0;
	return 2.0 * n * f / (f + n - z * (f - n));
}
void main()
{
	float z0 = lin(texture2D(u_depth, v_uv).r);
	float sum = 0.0, wsum = 0.0;
	for (int i = -3; i <= 3; ++i)
	{
		vec2 uv = v_uv + u_dir * float(i);
		float z = lin(texture2D(u_depth, uv).r);
		float w = (4.0 - abs(float(i))) * exp(-abs(z - z0) / max(z0 * 0.05, 0.05));
		sum += texture2D(u_src, uv).r * w;
		wsum += w;
	}
	float ao = sum / max(wsum, 1e-4);
	gl_FragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";

/* Bloom downsample.  On the first level it also applies a soft
 * threshold so only highlights (emissive surfaces, explosions, lasers)
 * bleed.
 */
constexpr char down_fs[] = R"GLSL(
#version 120
uniform sampler2D u_src;
uniform vec2 u_texel;
uniform float u_threshold;
varying vec2 v_uv;
vec3 prefilter(vec3 c)
{
	float br = max(c.r, max(c.g, c.b));
	float knee = u_threshold * 0.5;
	float soft = clamp(br - u_threshold + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee + 1e-4);
	float contrib = max(soft, br - u_threshold) / max(br, 1e-4);
	return c * contrib;
}
void main()
{
	vec2 o = u_texel;
	vec3 a = texture2D(u_src, v_uv + vec2(-o.x, -o.y)).rgb;
	vec3 b = texture2D(u_src, v_uv + vec2( o.x, -o.y)).rgb;
	vec3 c = texture2D(u_src, v_uv + vec2(-o.x,  o.y)).rgb;
	vec3 d = texture2D(u_src, v_uv + vec2( o.x,  o.y)).rgb;
	vec3 m = texture2D(u_src, v_uv).rgb;
	vec3 s = (a + b + c + d) * 0.125 + m * 0.5;
	if (u_threshold > 0.0)
		s = prefilter(s);
	gl_FragColor = vec4(s, 1.0);
}
)GLSL";

/* Bloom upsample: 9-tap tent, additively blended into the next level. */
constexpr char up_fs[] = R"GLSL(
#version 120
uniform sampler2D u_src;
uniform vec2 u_texel;
varying vec2 v_uv;
void main()
{
	vec2 o = u_texel;
	vec3 s = texture2D(u_src, v_uv).rgb * 4.0;
	s += texture2D(u_src, v_uv + vec2(-o.x, 0.0)).rgb * 2.0;
	s += texture2D(u_src, v_uv + vec2( o.x, 0.0)).rgb * 2.0;
	s += texture2D(u_src, v_uv + vec2(0.0, -o.y)).rgb * 2.0;
	s += texture2D(u_src, v_uv + vec2(0.0,  o.y)).rgb * 2.0;
	s += texture2D(u_src, v_uv + vec2(-o.x, -o.y)).rgb;
	s += texture2D(u_src, v_uv + vec2( o.x, -o.y)).rgb;
	s += texture2D(u_src, v_uv + vec2(-o.x,  o.y)).rgb;
	s += texture2D(u_src, v_uv + vec2( o.x,  o.y)).rgb;
	gl_FragColor = vec4(s / 16.0, 1.0);
}
)GLSL";

/* Combine scene, AO and bloom; tonemap with a shoulder that leaves the
 * original palette untouched below ~0.7 and rolls off HDR highlights.
 */
constexpr char composite_fs[] = R"GLSL(
#version 120
uniform sampler2D u_scene;
uniform sampler2D u_ao;
uniform sampler2D u_bloom;
uniform float u_ao_strength;
uniform float u_bloom_strength;
uniform float u_vignette;
uniform float u_exposure;
varying vec2 v_uv;
vec3 shoulder(vec3 x)
{
	const float k = 0.8;
	vec3 over = max(x - k, 0.0);
	return min(x, vec3(k)) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k)));
}
void main()
{
	vec4 s = texture2D(u_scene, v_uv);
	float ao = texture2D(u_ao, v_uv).r;
	vec3 bloom = texture2D(u_bloom, v_uv).rgb;
	vec3 c = s.rgb * mix(1.0, ao, u_ao_strength) * u_exposure;
	c += bloom * u_bloom_strength;
	c = shoulder(c);
	/* gentle contrast and saturation lift */
	float l = dot(c, vec3(0.299, 0.587, 0.114));
	c = mix(vec3(l), c, 1.08);
	c = clamp(c, 0.0, 1.0);
	c = c * c * (3.0 - 2.0 * c) * 0.10 + c * 0.90;
	vec2 q = v_uv - 0.5;
	c *= 1.0 - u_vignette * dot(q, q) * 1.6;
	float a = clamp(s.a + dot(bloom, vec3(0.333)) * u_bloom_strength, 0.0, 1.0);
	gl_FragColor = vec4(c, a);
}
)GLSL";

/* FXAA (after Timothy Lottes' FXAA 2 / "console" variant). */
constexpr char fxaa_fs[] = R"GLSL(
#version 120
uniform sampler2D u_src;
uniform vec2 u_texel;
varying vec2 v_uv;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main()
{
	vec2 r = u_texel;
	vec4 m4 = texture2D(u_src, v_uv);
	vec3 rgbM = m4.rgb;
	float lNW = luma(texture2D(u_src, v_uv + vec2(-1.0, -1.0) * r).rgb);
	float lNE = luma(texture2D(u_src, v_uv + vec2( 1.0, -1.0) * r).rgb);
	float lSW = luma(texture2D(u_src, v_uv + vec2(-1.0,  1.0) * r).rgb);
	float lSE = luma(texture2D(u_src, v_uv + vec2( 1.0,  1.0) * r).rgb);
	float lM = luma(rgbM);
	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
	vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
	float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
	float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
	dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * r;
	vec3 a = 0.5 * (texture2D(u_src, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
	                texture2D(u_src, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
	vec3 b = a * 0.5 + 0.25 * (texture2D(u_src, v_uv - dir * 0.5).rgb +
	                           texture2D(u_src, v_uv + dir * 0.5).rgb);
	float lB = luma(b);
	vec3 outc = (lB < lMin || lB > lMax) ? a : b;
	gl_FragColor = vec4(outc, m4.a);
}
)GLSL";

GLuint compile(GLenum type, const char *src, const char *label)
{
	const GLuint s = gl.CreateShader(type);
	gl.ShaderSource(s, 1, &src, nullptr);
	gl.CompileShader(s);
	GLint ok = 0;
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		std::array<char, 2048> log{};
		gl.GetShaderInfoLog(s, log.size(), nullptr, log.data());
		con_printf(CON_URGENT, "remaster: %s shader failed to compile:\n%s", label, log.data());
		gl.DeleteShader(s);
		return 0;
	}
	return s;
}

GLuint link(const char *vs, const char *fs, const char *label)
{
	const GLuint v = compile(GL_VERTEX_SHADER, vs, label);
	const GLuint f = compile(GL_FRAGMENT_SHADER, fs, label);
	if (!v || !f)
		return 0;
	const GLuint p = gl.CreateProgram();
	gl.AttachShader(p, v);
	gl.AttachShader(p, f);
	gl.LinkProgram(p);
	gl.DeleteShader(v);
	gl.DeleteShader(f);
	GLint ok = 0;
	gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		std::array<char, 2048> log{};
		gl.GetProgramInfoLog(p, log.size(), nullptr, log.data());
		con_printf(CON_URGENT, "remaster: %s program failed to link:\n%s", label, log.data());
		return 0;
	}
	return p;
}

struct program
{
	GLuint id = 0;
	GLint loc(const char *name) const { return gl.GetUniformLocation(id, name); }
};

struct world_program : program
{
	GLint mode, emis_strength, metal, rough, lit, detail;
};

/* ------------------------------------------------------------------ */
/* State                                                               */

struct state_t
{
	bool initialized = false;
	bool shaders = false;		// world shader usable
	bool post = false;		// FBO post chain usable
	bool hires = true;
	bool ao = true;
	bool hdr = true;		// RGBA16F render target available
	world_program world;
	program ssao, blur, down, up, composite, fxaa;
};
state_t st;

/* Per-bitmap material, resolved lazily the first time a bitmap is drawn. */
struct bm_material
{
	bool resolved = false;
	uint8_t mode = 0;
	float strength = 0.f;
	float metal = 0.f;
	float rough = 0.8f;
	GLuint emis_tex = 0;
};
std::array<bm_material, MAX_BITMAP_FILES> materials;

/* Bitmap indices of textures that cast light, from TmapInfo/Effects. */
std::vector<bool> light_bitmaps;

void make_file_stem(const char *pig_name, std::array<char, 32> &out)
{
	std::size_t i = 0;
	for (; pig_name[i] && i + 1 < out.size(); ++i)
	{
		char c = pig_name[i];
		if (c == '#')
			c = '_';
		else if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
		out[i] = c;
	}
	out[i] = 0;
}

#if DXX_USE_SDLIMAGE
/* Load an image from PhysFS as tightly packed RGBA8. */
struct rgba_image
{
	int w = 0, h = 0;
	std::unique_ptr<uint8_t[]> px;
};

bool load_rgba(const char *path, rgba_image &out)
{
	auto rw = PHYSFSRWOPS_openRead(path).first;
	if (!rw)
		return false;
	SDL_Surface *const raw = IMG_Load_RW(rw.get(), 0);
	if (!raw)
		return false;
	SDL_Surface *const s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ABGR8888, 0);
	SDL_FreeSurface(raw);
	if (!s)
		return false;
	out.w = s->w;
	out.h = s->h;
	out.px = std::make_unique<uint8_t[]>(static_cast<std::size_t>(s->w) * s->h * 4);
	SDL_LockSurface(s);
	for (int y = 0; y < s->h; ++y)
		memcpy(out.px.get() + static_cast<std::size_t>(y) * s->w * 4, static_cast<const uint8_t *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch, static_cast<std::size_t>(s->w) * 4);
	SDL_UnlockSurface(s);
	SDL_FreeSurface(s);
	return true;
}
#endif

void set_trilinear(bool repeat)
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	if (ogl_maxanisotropy > 1.0f)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, ogl_maxanisotropy);
}

void build_light_bitmaps()
{
	if (!light_bitmaps.empty() || !NumTextures)
		return;
	light_bitmaps.assign(MAX_BITMAP_FILES, false);
	auto &TmapInfo = LevelUniqueTmapInfoState.TmapInfo;
	auto &Effects = LevelUniqueEffectsClipState.Effects;
	for (unsigned i = 0; i < NumTextures && i < TmapInfo.size(); ++i)
	{
		const auto &ti = TmapInfo[i];
		if (ti.lighting <= 0)
			continue;
		const auto bi = static_cast<std::size_t>(Textures[i]);
		if (bi < light_bitmaps.size())
			light_bitmaps[bi] = true;
		if (ti.eclip_num != effect_index::None)
		{
			const auto ei = static_cast<std::size_t>(ti.eclip_num);
			if (ei < Effects.size())
			{
				const auto &vc = Effects[static_cast<effect_index>(ei)].vc;
				for (unsigned f = 0; f < vc.num_frames && f < vc.frames.size(); ++f)
				{
					const auto fb = static_cast<std::size_t>(vc.frames[f]);
					if (fb < light_bitmaps.size())
						light_bitmaps[fb] = true;
				}
			}
		}
	}
}

/* Parse the subset of DXX-Raytracer's .material format we use. */
void read_material_file(const char *stem, bm_material &m, bool &is_light, float &strength)
{
	std::array<char, 96> path;
	snprintf(path.data(), path.size(), "remaster/textures/%s.material", stem);
	auto f = PHYSFSX_openReadBuffered(path.data()).first;
	if (!f)
		return;
	for (PHYSFSX_gets_line_t<128> line; const auto rc{PHYSFSX_fgets(line, f)};)
	{
		const std::string text(rc.begin(), rc.end());
		char key[64];
		float val;
		if (sscanf(text.c_str(), " %63[a-z_] = %f", key, &val) != 2)
			continue;
		if (!strcmp(key, "metalness"))
			m.metal = val;
		else if (!strcmp(key, "roughness"))
			m.rough = val;
		else if (!strcmp(key, "emissive_strength"))
			strength = val;
		else if (!strcmp(key, "is_light"))
			is_light = val > 0.5f;
	}
}

#if DXX_USE_SDLIMAGE
bool is_pow2(unsigned v)
{
	return v && !(v & (v - 1));
}
#endif

bm_material &resolve_material(const grs_bitmap &bm, remaster_draw kind)
{
	static bm_material scratch;
	unsigned idx = 0;
	const grs_bitmap *root = &bm;
	const char *name = piggy_game_bitmap_name(*root, &idx);
	while (!name && root->bm_parent)
	{
		root = root->bm_parent;
		name = piggy_game_bitmap_name(*root, &idx);
	}
	if (!name)
	{
		scratch = {};
		scratch.resolved = true;
		if (bm.get_flag_mask(BM_FLAG_NO_LIGHTING) && kind == remaster_draw::sprite)
		{
			scratch.mode = 3;
			scratch.strength = 0.6f;
		}
		return scratch;
	}
	auto &m = materials[idx];
	if (m.resolved)
		return m;
	m = {};
	m.resolved = true;

	std::array<char, 32> stem;
	make_file_stem(name, stem);
	if (strstr(stem.data(), "metl"))
	{
		m.metal = 1.0f;
		m.rough = 0.5f;
	}
	bool is_light = false;
	float strength = 3.5f;
	read_material_file(stem.data(), m, is_light, strength);
	build_light_bitmaps();
	if (!light_bitmaps.empty() && light_bitmaps[idx])
		is_light = true;
	/* DXX-Raytracer uses 3.5 for ordinary emissives, larger for lamps. */
	const float scaled = std::clamp(strength / 3.5f, 0.5f, 3.0f);

#if DXX_USE_SDLIMAGE
	if (is_pow2(root->bm_w) && is_pow2(root->bm_h))
	{
		std::array<char, 96> path;
		snprintf(path.data(), path.size(), "remaster/textures/%s_emissive.png", stem.data());
		rgba_image img;
		if (load_rgba(path.data(), img) && is_pow2(img.w) && is_pow2(img.h))
		{
			/* Keep only red as an intensity mask. */
			std::vector<uint8_t> lum(static_cast<std::size_t>(img.w) * img.h);
			for (std::size_t i = 0; i < lum.size(); ++i)
				lum[i] = img.px[i * 4];
			glGenTextures(1, &m.emis_tex);
			glBindTexture(GL_TEXTURE_2D, m.emis_tex);
			set_trilinear(true);
			gluBuild2DMipmaps(GL_TEXTURE_2D, GL_LUMINANCE8, img.w, img.h, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum.data());
			m.mode = 1;
			m.strength = 1.1f * scaled;
			return m;
		}
	}
#endif
	if (is_light)
	{
		m.mode = 2;
		m.strength = 1.2f * scaled;
	}
	else if (bm.get_flag_mask(BM_FLAG_NO_LIGHTING) && kind == remaster_draw::sprite)
	{
		m.mode = 3;
		m.strength = 0.6f;
	}
	return m;
}

/* ------------------------------------------------------------------ */
/* Render targets                                                      */

struct target
{
	GLuint fbo = 0, tex = 0, depth = 0;
	int w = 0, h = 0;
};

GLuint make_tex(int w, int h, GLint internal, GLenum format, GLenum type, GLint filter)
{
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
	return t;
}

void destroy(target &t)
{
	if (t.fbo)
		gl.DeleteFramebuffers(1, &t.fbo);
	if (t.tex)
		glDeleteTextures(1, &t.tex);
	if (t.depth)
		glDeleteTextures(1, &t.depth);
	t = {};
}

bool create(target &t, int w, int h, bool hdr, bool with_depth)
{
	t.w = w;
	t.h = h;
	t.tex = make_tex(w, h, hdr ? GL_RGBA16F : GL_RGBA8, GL_RGBA, hdr ? GL_FLOAT : GL_UNSIGNED_BYTE, GL_LINEAR);
	if (with_depth)
		t.depth = make_tex(w, h, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
	gl.GenFramebuffers(1, &t.fbo);
	gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
	if (with_depth)
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
	const bool ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	if (!ok)
		destroy(t);
	return ok;
}

constexpr unsigned bloom_levels = 6;

struct view_targets
{
	int w = 0, h = 0;
	unsigned last_used = 0;
	target scene, ldr;
	std::array<target, 2> ao;
	std::array<target, bloom_levels> bloom;
	unsigned nbloom = 0;
	bool valid = false;
};

std::array<view_targets, 3> views;
unsigned frame_counter;

void destroy(view_targets &v)
{
	destroy(v.scene);
	destroy(v.ldr);
	for (auto &a : v.ao)
		destroy(a);
	for (auto &b : v.bloom)
		destroy(b);
	v = {};
}

bool create(view_targets &v, int w, int h)
{
	v.w = w;
	v.h = h;
	bool ok = create(v.scene, w, h, st.hdr, true);
	if (!ok && st.hdr)
	{
		/* No float render targets: fall back to 8-bit, still useful. */
		st.hdr = false;
		ok = create(v.scene, w, h, false, true);
	}
	ok = ok && create(v.ldr, w, h, false, false);
	const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
	ok = ok && create(v.ao[0], hw, hh, false, false) && create(v.ao[1], hw, hh, false, false);
	v.nbloom = 0;
	int bw = hw, bh = hh;
	for (unsigned i = 0; ok && i < bloom_levels && bw >= 8 && bh >= 8; ++i)
	{
		ok = create(v.bloom[i], bw, bh, st.hdr, false);
		++v.nbloom;
		bw /= 2;
		bh /= 2;
	}
	v.valid = ok && v.nbloom > 0;
	if (!v.valid)
		destroy(v);
	return v.valid;
}

view_targets *get_view(int w, int h)
{
	++frame_counter;
	view_targets *lru = &views[0];
	for (auto &v : views)
	{
		if (v.valid && v.w == w && v.h == h)
		{
			v.last_used = frame_counter;
			return &v;
		}
		if (!v.valid || v.last_used < lru->last_used)
			lru = &v;
	}
	destroy(*lru);
	if (!create(*lru, w, h))
		return nullptr;
	lru->last_used = frame_counter;
	return lru;
}

/* Active frame bookkeeping. */
struct frame_t
{
	view_targets *v = nullptr;
	int x = 0, y = 0, w = 0, h = 0;		// canvas rectangle in screen coordinates (top-left origin)
	GLint prev_fbo = 0;
};
frame_t frame;

void draw_quad()
{
	static const GLfloat q[] = {0, 0, 1, 0, 1, 1, 0, 1};
	glVertexPointer(2, GL_FLOAT, 0, q);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void bind_tex(unsigned unit, GLuint tex)
{
	gl.ActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, tex);
}

void set_sampler(const program &p, const char *name, int unit)
{
	gl.Uniform1i(p.loc(name), unit);
}

void render_to(const target &t)
{
	gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	glViewport(0, 0, t.w, t.h);
}

constexpr float z_near = 0.1f, z_far = 5000.0f;

}

void ogl_remaster_init()
{
	if (st.initialized)
		return;
	st.initialized = true;
	if (!env_enabled("DXX_REMASTER"))
	{
		con_puts(CON_NORMAL, "remaster: disabled by DXX_REMASTER");
		return;
	}
	st.hires = env_enabled("DXX_REMASTER_HIRES");
	st.ao = env_enabled("DXX_REMASTER_AO");
	if (!load_gl_api())
	{
		con_puts(CON_NORMAL, "remaster: OpenGL 2.0 shaders unavailable, using classic renderer");
		return;
	}
	st.world.id = link(world_vs, world_fs, "world");
	if (st.world.id)
	{
		gl.UseProgram(st.world.id);
		set_sampler(st.world, "u_tex", 0);
		set_sampler(st.world, "u_emis", 1);
		st.world.mode = st.world.loc("u_mode");
		st.world.emis_strength = st.world.loc("u_emis_strength");
		st.world.metal = st.world.loc("u_metal");
		st.world.rough = st.world.loc("u_rough");
		st.world.lit = st.world.loc("u_lit");
		st.world.detail = st.world.loc("u_detail");
		gl.UseProgram(0);
		st.shaders = true;
	}
	if (env_enabled("DXX_REMASTER_POST") && load_fbo_api())
	{
		st.ssao.id = link(quad_vs, ssao_fs, "ssao");
		st.blur.id = link(quad_vs, blur_fs, "blur");
		st.down.id = link(quad_vs, down_fs, "bloom-down");
		st.up.id = link(quad_vs, up_fs, "bloom-up");
		st.composite.id = link(quad_vs, composite_fs, "composite");
		st.fxaa.id = link(quad_vs, fxaa_fs, "fxaa");
		st.post = st.ssao.id && st.blur.id && st.down.id && st.up.id && st.composite.id && st.fxaa.id;
	}
	con_printf(CON_NORMAL, "remaster: world shader %s, post-processing %s, hi-res textures %s",
		st.shaders ? "on" : "off", st.post ? "on" : "off", st.hires ? "on" : "off");
}

void ogl_remaster_reset_textures()
{
	for (auto &m : materials)
	{
		if (m.emis_tex)
			glDeleteTextures(1, &m.emis_tex);
		m = {};
	}
	light_bitmaps.clear();
}

bool ogl_remaster_load_override(const grs_bitmap &bm, ogl_texture &tex)
{
#if DXX_USE_SDLIMAGE
	if (!st.initialized || !st.hires || !env_enabled("DXX_REMASTER"))
		return false;
	unsigned idx;
	const char *const name = piggy_game_bitmap_name(bm, &idx);
	if (!name)
		return false;
	std::array<char, 32> stem;
	make_file_stem(name, stem);
	std::array<char, 96> path;
	snprintf(path.data(), path.size(), "hires/%s.png", stem.data());
	rgba_image img;
	if (!load_rgba(path.data(), img))
		return false;
	if (!tex.w || !tex.h || img.w % tex.w || img.h % tex.h || img.w / tex.w != img.h / tex.h)
	{
		con_printf(CON_VERBOSE, "remaster: %s is %dx%d, not a multiple of %ux%u; ignored", path.data(), img.w, img.h, tex.w, tex.h);
		return false;
	}
	const unsigned k = img.w / tex.w;
	tex.tw = std::bit_ceil(tex.w);
	tex.th = std::bit_ceil(tex.h);
	tex.u = static_cast<float>(static_cast<double>(tex.w) / tex.tw);
	tex.v = static_cast<float>(static_cast<double>(tex.h) / tex.th);
	const unsigned TW = tex.tw * k, TH = tex.th * k;
	const uint8_t *pixels = img.px.get();
	std::unique_ptr<uint8_t[]> padded;
	if (TW != static_cast<unsigned>(img.w) || TH != static_cast<unsigned>(img.h))
	{
		padded = std::make_unique<uint8_t[]>(static_cast<std::size_t>(TW) * TH * 4);
		memset(padded.get(), 0, static_cast<std::size_t>(TW) * TH * 4);
		for (int y = 0; y < img.h; ++y)
			memcpy(padded.get() + static_cast<std::size_t>(y) * TW * 4, img.px.get() + static_cast<std::size_t>(y) * img.w * 4, static_cast<std::size_t>(img.w) * 4);
		pixels = padded.get();
	}
	glGenTextures(1, &tex.handle);
	glBindTexture(GL_TEXTURE_2D, tex.handle);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	set_trilinear(true);
	tex.internalformat = GL_RGBA;
	tex.format = GL_RGBA;
	gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	(void)idx;
	return true;
#else
	(void)bm;
	(void)tex;
	return false;
#endif
}

void ogl_remaster_begin_textured(const grs_bitmap &bm, const remaster_draw kind)
{
	if (!st.shaders)
		return;
	const auto &m = resolve_material(bm, kind);
	gl.UseProgram(st.world.id);
	if (kind == remaster_draw::sprite && m.mode == 0)
	{
		/* Sprites are drawn full-bright; lift them slightly so explosions,
		 * weapon blobs and powerups feed the bloom.
		 */
		gl.Uniform1f(st.world.mode, 3.f);
		gl.Uniform1f(st.world.emis_strength, 0.35f);
	}
	else
	{
		gl.Uniform1f(st.world.mode, m.mode);
		gl.Uniform1f(st.world.emis_strength, m.strength);
	}
	gl.Uniform1f(st.world.metal, m.metal);
	gl.Uniform1f(st.world.rough, m.rough);
	const bool lit = kind == remaster_draw::world && !bm.get_flag_mask(BM_FLAG_NO_LIGHTING);
	gl.Uniform1f(st.world.lit, lit ? 1.f : 0.f);
	gl.Uniform1f(st.world.detail, lit && m.mode == 0 ? 0.22f : 0.f);
	if (m.mode == 1)
	{
		gl.ActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m.emis_tex);
		gl.ActiveTexture(GL_TEXTURE0);
	}
}

void ogl_remaster_end_textured()
{
	if (st.shaders)
		gl.UseProgram(0);
}

void ogl_remaster_begin_frame(const grs_canvas &canvas)
{
	frame = {};
	if (!st.post)
		return;
	const int w = canvas.cv_bitmap.bm_w, h = canvas.cv_bitmap.bm_h;
	if (w < 16 || h < 16)
		return;
	const auto v = get_view(w, h);
	if (!v)
	{
		con_puts(CON_URGENT, "remaster: could not create render targets; post-processing disabled");
		st.post = false;
		return;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &frame.prev_fbo);
	frame.v = v;
	frame.x = canvas.cv_bitmap.bm_x;
	frame.y = canvas.cv_bitmap.bm_y;
	frame.w = w;
	frame.h = h;
	gl.BindFramebuffer(GL_FRAMEBUFFER, v->scene.fbo);
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void ogl_remaster_end_frame()
{
	auto *const v = frame.v;
	if (!v)
		return;
	frame.v = nullptr;

	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT);
	glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);

	const float nf[2] = {z_near, z_far};

	/* 1. ambient occlusion at half resolution */
	const bool do_ao = st.ao;
	if (do_ao)
	{
		render_to(v->ao[0]);
		gl.UseProgram(st.ssao.id);
		set_sampler(st.ssao, "u_depth", 0);
		gl.Uniform2f(st.ssao.loc("u_near_far"), nf[0], nf[1]);
		gl.Uniform2f(st.ssao.loc("u_texel"), 1.f / v->scene.w, 1.f / v->scene.h);
		gl.Uniform1f(st.ssao.loc("u_radius"), 4.0f);
		bind_tex(0, v->scene.depth);
		draw_quad();

		gl.UseProgram(st.blur.id);
		set_sampler(st.blur, "u_src", 0);
		set_sampler(st.blur, "u_depth", 1);
		gl.Uniform2f(st.blur.loc("u_near_far"), nf[0], nf[1]);
		bind_tex(1, v->scene.depth);
		render_to(v->ao[1]);
		gl.Uniform2f(st.blur.loc("u_dir"), 1.f / v->ao[0].w, 0.f);
		bind_tex(0, v->ao[0].tex);
		draw_quad();
		render_to(v->ao[0]);
		gl.Uniform2f(st.blur.loc("u_dir"), 0.f, 1.f / v->ao[1].h);
		bind_tex(0, v->ao[1].tex);
		draw_quad();
	}

	/* 2. bloom: thresholded downsample chain, then tent upsample back */
	gl.UseProgram(st.down.id);
	set_sampler(st.down, "u_src", 0);
	for (unsigned i = 0; i < v->nbloom; ++i)
	{
		const target &src = i ? v->bloom[i - 1] : v->scene;
		render_to(v->bloom[i]);
		gl.Uniform2f(st.down.loc("u_texel"), 1.f / src.w, 1.f / src.h);
		gl.Uniform1f(st.down.loc("u_threshold"), i ? 0.f : (st.hdr ? 0.85f : 0.7f));
		bind_tex(0, src.tex);
		draw_quad();
	}
	gl.UseProgram(st.up.id);
	set_sampler(st.up, "u_src", 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	for (unsigned i = v->nbloom - 1; i-- > 0;)
	{
		const target &src = v->bloom[i + 1];
		render_to(v->bloom[i]);
		gl.Uniform2f(st.up.loc("u_texel"), 1.f / src.w, 1.f / src.h);
		bind_tex(0, src.tex);
		draw_quad();
	}
	glDisable(GL_BLEND);

	/* 3. composite into the LDR target */
	render_to(v->ldr);
	gl.UseProgram(st.composite.id);
	set_sampler(st.composite, "u_scene", 0);
	set_sampler(st.composite, "u_ao", 1);
	set_sampler(st.composite, "u_bloom", 2);
	gl.Uniform1f(st.composite.loc("u_ao_strength"), do_ao ? 0.65f : 0.f);
	gl.Uniform1f(st.composite.loc("u_exposure"), 1.06f);
	gl.Uniform1f(st.composite.loc("u_bloom_strength"), st.hdr ? 0.55f : 0.35f);
	gl.Uniform1f(st.composite.loc("u_vignette"), 0.2f);
	bind_tex(0, v->scene.tex);
	bind_tex(1, do_ao ? v->ao[0].tex : v->ldr.tex);
	bind_tex(2, v->bloom[0].tex);
	draw_quad();

	/* 4. FXAA onto the real framebuffer, over the canvas rectangle */
	gl.BindFramebuffer(GL_FRAMEBUFFER, frame.prev_fbo);
	const int screen_h = grd_curscreen->sc_canvas.cv_bitmap.bm_h;
	glViewport(frame.x, screen_h - frame.y - frame.h, frame.w, frame.h);
	gl.UseProgram(st.fxaa.id);
	set_sampler(st.fxaa, "u_src", 0);
	gl.Uniform2f(st.fxaa.loc("u_texel"), 1.f / v->ldr.w, 1.f / v->ldr.h);
	bind_tex(2, 0);
	bind_tex(1, 0);
	bind_tex(0, v->ldr.tex);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	draw_quad();

	gl.UseProgram(0);
	gl.ActiveTexture(GL_TEXTURE0);
	glPopClientAttrib();
	glPopAttrib();
	glDepthMask(GL_TRUE);
}

}
