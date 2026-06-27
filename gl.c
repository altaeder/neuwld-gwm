/**
 * @file gl.c
 * @brief General OpenGL implementation for hardware accelerated rendering.
 *
 * @author uint
 * @copyright Copyright (c) 2026 uint
 * @licenseblock{ISC License}
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 * @endlicenseblock
 */

#include <string.h>

#include "drm-private.h"
#include "gl.h"

#define GL_WAIT_NS (50ULL * 1000ULL * 1000ULL)
#define GL_WAIT_ATTEMPTS 40

/* prototypes */
static const char *
egl_error_name(EGLint error);
static bool
gl_buffer_ensure(struct gl_buffer *buffer);
static void
gl_buffer_import_cleanup(struct gl_context *context, EGLImageKHR *egl_image,
                         GLuint *texture, GLuint *fbo);
static void
gl_context_cleanup(struct gl_context *context);
static void
gl_context_reset(struct gl_context *context);
static bool
gl_copy_program_create(struct gl_context *context);
static void
gl_copy_program_cleanup(GLuint *vert, GLuint *frag, GLuint *program,
                        GLuint *copy_vbo);
static void
gl_error_clear(void);
static const char *
gl_error_name(GLenum error);
static bool
gl_error_report(const char *where);
static GLuint
gl_shader_compile(GLenum type, const char *source);
static bool
gpu_access_ensure(struct gl_buffer *buffer, bool write);

/**
 * @brief Convert EGL error code to  string
 * @param error EGL error code.
 * @return Const string describing error message of code.
 */
static const char *
egl_error_name(EGLint error)
{
	switch (error) {
	case EGL_SUCCESS:
		return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED:
		return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS:
		return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC:
		return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE:
		return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONTEXT:
		return "EGL_BAD_CONTEXT";
	case EGL_BAD_CONFIG:
		return "EGL_BAD_CONFIG";
	case EGL_BAD_CURRENT_SURFACE:
		return "EGL_BAD_CURRENT_SURFACE";
	case EGL_BAD_DISPLAY:
		return "EGL_BAD_DISPLAY";
	case EGL_BAD_SURFACE:
		return "EGL_BAD_SURFACE";
	case EGL_BAD_MATCH:
		return "EGL_BAD_MATCH";
	case EGL_BAD_PARAMETER:
		return "EGL_BAD_PARAMETER";
	case EGL_BAD_NATIVE_PIXMAP:
		return "EGL_BAD_NATIVE_PIXMAP";
	case EGL_BAD_NATIVE_WINDOW:
		return "EGL_BAD_NATIVE_WINDOW";
	case EGL_CONTEXT_LOST:
		return "EGL_CONTEXT_LOST";
	default:
		return "EGL_UNKNOWN";
	}
}
 
/**
 * @brief Cleanup EGLImage, texture, and framebuffer created for buffer import
 * @param context GL context
 * @param egl_image EGLImageKHR to destroy
 * @param texture GL texture to reset
 * @param fbo GL framebuffer to reset
 */
static void
gl_buffer_import_cleanup(struct gl_context *context, EGLImageKHR *egl_image,
                         GLuint *texture, GLuint *fbo)
{
	if (*fbo) {
		glDeleteFramebuffers(1, fbo);
		*fbo = 0;
	}

	if (*texture) {
		glDeleteTextures(1, texture);
		*texture = 0;
	}

	if (*egl_image != EGL_NO_IMAGE_KHR) {
		context->eglDestroyImageKHR(context->egl_display, *egl_image);
		*egl_image = EGL_NO_IMAGE_KHR;
	}
}

/**
 * @brief Cleanup GL resources, EGL context
 * Deletes GL textures, buffers, shader programs, EGLImages.
 * Destroys EGL context and surface.
 * @param context GL context to cleanup.
 */
static void
gl_context_cleanup(struct gl_context *context)
{
	bool current = false;

	if (!context) {
		return;
	}

	/* make context current for cleanup */
	if (context->egl_initialized && context->egl_display != EGL_NO_DISPLAY &&
	    context->egl_context != EGL_NO_CONTEXT) {
		current = eglMakeCurrent(context->egl_display, context->egl_surface,
		                         context->egl_surface,
		                         context->egl_context) == EGL_TRUE;
		if (!current && (context->copy_program || context->copy_vbo)) {
			DEBUG("amdgpu: cleanup could not make GL context current\n");
		}
	}

	if (current) {
		if (context->copy_program) {
			glDeleteProgram(context->copy_program);
		}
		if (context->copy_vbo) {
			glDeleteBuffers(1, &context->copy_vbo);
		}

		eglMakeCurrent(context->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		               EGL_NO_CONTEXT);
	}
	context->copy_program = 0;
	context->copy_vbo = 0;

	/* destroy EGL context, surface */
	if (context->egl_initialized && context->egl_display != EGL_NO_DISPLAY) {
		if (context->egl_surface != EGL_NO_SURFACE) {
			eglDestroySurface(context->egl_display, context->egl_surface);
		}

		if (context->egl_context != EGL_NO_CONTEXT) {
			eglDestroyContext(context->egl_display, context->egl_context);
		}

		eglTerminate(context->egl_display);
	}

	if (context->gbm) {
		gbm_device_destroy(context->gbm);
	}

	gl_context_reset(context);
}

/**
 * @brief Reset GL context struct fields to default
 * @param context GL context to reset
 */
static void
gl_context_reset(struct gl_context *context)
{
	memset(context, 0, sizeof *context);
	context->egl_display = EGL_NO_DISPLAY;
	context->egl_context = EGL_NO_CONTEXT;
	context->egl_surface = EGL_NO_SURFACE;
	context->copy_uniform_sampler = -1;
}

/**
 * @brief Clean up program resources
 * @param vert Vertex shader to reset
 * @param frag Fragment shader to reset
 * @param program Shader program reset
 * @param copy_vbo VBO to reset
 */
static void
gl_copy_program_cleanup(GLuint *vert, GLuint *frag, GLuint *program,
                        GLuint *copy_vbo)
{
	if (*copy_vbo) {
		glDeleteBuffers(1, copy_vbo);
		*copy_vbo = 0;
	}

	if (*program) {
		glDeleteProgram(*program);
		*program = 0;
	}

	if (*frag) {
		glDeleteShader(*frag);
		*frag = 0;
	}

	if (*vert) {
		glDeleteShader(*vert);
		*vert = 0;
	}
}

bool
gl_buffer_cpu_access_ensure(struct gl_buffer *buffer, uint32_t access)
{
	bool hazard;

	if ((access & (GL_ACCESS_READ | GL_ACCESS_WRITE)) == 0) {
		return false;
	}

	hazard = false;
	if (access & GL_ACCESS_WRITE) {
		hazard = buffer->gpu_pending_access != 0;
	} else if (access & GL_ACCESS_READ) {
		hazard = (buffer->gpu_pending_access & GL_ACCESS_WRITE) != 0;
	}

	if (hazard && buffer->gpu_pending_ops > 0 && !gl_buffer_wait_idle(buffer)) {
		return false;
	}

	gl_buffer_mark_cpu_access(buffer, false);
	return true;
}

void
gl_buffer_destroy(struct gl_buffer *buffer)
{
	struct gl_context *ctx = buffer->context;

	if (!buffer->gl_buffer_ready || !ctx->gl_ready) {
		return;
	}

	if (!gl_make_current(ctx)) {
		DEBUG("amdgpu: gl_buffer_destroy could not make context current\n");
		return;
	}

	if (buffer->fbo) {
		glDeleteFramebuffers(1, &buffer->fbo);
	}

	if (buffer->texture) {
		glDeleteTextures(1, &buffer->texture);
	}

	if (buffer->egl_image != EGL_NO_IMAGE_KHR) {
		ctx->eglDestroyImageKHR(ctx->egl_display, buffer->egl_image);
	}

	buffer->egl_image = EGL_NO_IMAGE_KHR;
	buffer->texture = 0;
	buffer->fbo = 0;
	buffer->gl_buffer_ready = false;
}

void
gl_buffer_mark_cpu_access(struct gl_buffer *buffer, bool write)
{
	buffer->domain = GL_DOMAIN_CPU;
	if (write) {
		buffer->cpu_dirty = true;
	}
}

bool
gl_buffer_mark_gpu_busy(struct gl_buffer *buffer, uint32_t access)
{
	struct gl_context *ctx = buffer->context;
	EGLSyncKHR fence;

	/* check has access */
	if ((access & (GL_ACCESS_READ | GL_ACCESS_WRITE)) == 0) {
		DEBUG("amdgpu: gl_buffer_mark_gpu_busy with no access bits\n");
		return false;
	}

	if (!ctx->eglCreateSyncKHR) {
		DEBUG("amdgpu: explicit fence sync unavailable\n");
		return false;
	}

	if (buffer->gl_fence != EGL_NO_SYNC_KHR) {
		if (ctx->eglDestroySyncKHR) {
			ctx->eglDestroySyncKHR(ctx->egl_display, buffer->gl_fence);
		}
		buffer->gl_fence = EGL_NO_SYNC_KHR;
	}

	/* create fence for buffer */
	fence = ctx->eglCreateSyncKHR(ctx->egl_display, EGL_SYNC_FENCE_KHR, NULL);
	if (fence == EGL_NO_SYNC_KHR) {
		DEBUG("amdgpu: eglCreateSyncKHR failed: 0x%x\n", eglGetError());
		return false;
	}
	buffer->gl_fence = fence;

	/* mark pending ops and access */
	if (buffer->gpu_pending_ops != UINT32_MAX) {
		buffer->gpu_pending_ops++;
	}
	buffer->gpu_pending_access |= access;
	buffer->domain = GL_DOMAIN_GPU;
	if (access & GL_ACCESS_WRITE) {
		buffer->cpu_dirty = false;
	}
	return true;
}

bool
gl_buffer_wait_idle(struct gl_buffer *buffer)
{
	if (buffer->gpu_pending_ops == 0) {
		return true;
	}

	EGLint wait_status;
	uint32_t tries;
	/* wait for fence when available */
	if (buffer->gl_fence == EGL_NO_SYNC_KHR) {
		DEBUG("amdgpu: pending GPU ops without fence\n");
		return false;
	}

	if (!buffer->context->gl_ready || !buffer->context->eglClientWaitSyncKHR ||
	    !gl_make_current(buffer->context)) {
		DEBUG("amdgpu: cannot wait on GL fence for busy buffer\n");
		return false;
	}

	/* wait for fence to be signaled */
	for (tries = 0; tries < GL_WAIT_ATTEMPTS; tries++) {
		wait_status = buffer->context->eglClientWaitSyncKHR(
		    buffer->context->egl_display, buffer->gl_fence, 0, GL_WAIT_NS);

		if (wait_status == EGL_CONDITION_SATISFIED_KHR) {
			if (buffer->context->eglDestroySyncKHR) {
				buffer->context->eglDestroySyncKHR(buffer->context->egl_display,
				                                   buffer->gl_fence);
			}
			buffer->gl_fence = EGL_NO_SYNC_KHR;
			buffer->gpu_pending_ops = 0;
			buffer->gpu_pending_access = 0;
			return true;
		}

		if (wait_status != EGL_TIMEOUT_EXPIRED_KHR) {
			DEBUG("amdgpu: eglClientWaitSyncKHR failed status=0x%x\n",
			      wait_status);
			return false;
		}
	}
	DEBUG("amdgpu: eglClientWaitSyncKHR timed out after %u tries\n",
	      GL_WAIT_ATTEMPTS);
	return false;
}

uint32_t
gl_drm_fourcc_from_wld_format(uint32_t format)
{
	/* sigma */
	switch (format) {
	case WLD_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	case WLD_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	default:
		return 0;
	}
}

/**
 * @brief Import DMA-BUF buffer ito GL objects.
 *
 * Creates an EGLImage, GL texture, and framebuffer for rendering.
 *
 * @param buffer Target buffer.
 * @return true on success, false on failure.
 */
static bool
gl_buffer_ensure(struct gl_buffer *buffer)
{
	struct gl_context *ctx = buffer->context;
	uint32_t drm_format;
	int dmabuf_fd;
	EGLint attrs[16];
	EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
	GLuint texture = 0;
	GLuint fbo = 0;
	uint32_t attr = 0;
	GLenum fb_status;

	if (!ctx->gl_ready) {
		DEBUG("amdgpu: gl_buffer_ensure stopped, gl_ready=0\n");
		return false;
	}

	if (buffer->gl_buffer_ready) {
		return true;
	}

	/* get drm format for buffer */
	drm_format = gl_drm_fourcc_from_wld_format(buffer->format);
	if (!drm_format) {
		DEBUG("amdgpu: gl_buffer_ensure unsupported format 0x%x\n",
		      buffer->format);
		return false;
	}

	if (!gl_make_current(ctx)) {
		DEBUG("amdgpu: gl_buffer_ensure gl_make_current failed\n");
		return false;
	}

	if (!buffer->get_dmabuf_fd ||
	    !buffer->get_dmabuf_fd(buffer->get_dmabuf_fd_data, &dmabuf_fd)) {
		DEBUG("amdgpu: gl_buffer_ensure get_dmabuf_fd failed\n");
		return false;
	}

	gl_error_clear();

	/* create egl image from dmabufffer */
	attrs[attr++] = EGL_WIDTH;
	attrs[attr++] = buffer->width;
	attrs[attr++] = EGL_HEIGHT;
	attrs[attr++] = buffer->height;
	attrs[attr++] = EGL_LINUX_DRM_FOURCC_EXT;
	attrs[attr++] = drm_format;
	attrs[attr++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attrs[attr++] = dmabuf_fd;
	attrs[attr++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attrs[attr++] = 0;
	attrs[attr++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attrs[attr++] = buffer->pitch;
	attrs[attr++] = EGL_NONE;
	egl_image = ctx->eglCreateImageKHR(ctx->egl_display, EGL_NO_CONTEXT,
	                                   EGL_LINUX_DMA_BUF_EXT, NULL, attrs);

	if (egl_image == EGL_NO_IMAGE_KHR) {
		EGLint error = eglGetError();
		DEBUG("amdgpu: eglCreateImageKHR failed: 0x%x (%s)\n", error,
		      egl_error_name(error));
		return false;
	}

	/* create texture and framebuffer for egl image */
	glGenTextures(1, &texture);
	if (!texture) {
		DEBUG("amdgpu: glGenTextures -> 0\n");
		gl_buffer_import_cleanup(ctx, &egl_image, &texture, &fbo);
		return false;
	}

	/* bind egl image to texture */
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image);
	if (!gl_error_report("gl_buffer_ensure texture import")) {
		gl_buffer_import_cleanup(ctx, &egl_image, &texture, &fbo);
		return false;
	}

	/* create framebuffer and attach texture */
	glGenFramebuffers(1, &fbo);
	if (!fbo) {
		DEBUG("amdgpu: glGenFramebuffers -> 0\n");
		gl_buffer_import_cleanup(ctx, &egl_image, &texture, &fbo);
		return false;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       texture, 0);
	if (!gl_error_report("gl_buffer_ensure framebuffer attach")) {
		gl_buffer_import_cleanup(ctx, &egl_image, &texture, &fbo);
		return false;
	}

	fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (!gl_error_report("gl_buffer_ensure framebuffer status")) {
		gl_buffer_import_cleanup(ctx, &egl_image, &texture, &fbo);
		return false;
	}
	if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
		DEBUG("amdgpu: framebuffer incomplete status=0x%x\n", fb_status);
		gl_buffer_import_cleanup(ctx, &egl_image, &texture, &fbo);
		return false;
	}

	buffer->egl_image = egl_image;
	buffer->texture = texture;
	buffer->fbo = fbo;
	buffer->gl_buffer_ready = true;
	DEBUG("amdgpu: GL import ready for buffer %ux%u fmt=0x%x pitch=%u\n",
	      buffer->width, buffer->height, buffer->format, buffer->pitch);
	return true;
}

/**
 * @brief Build and initialise shader program.
 *
 * Used for texture copties
 *
 * @param context GL context (that stores created program/VBO handles).
 * @return true on success, false on failure.
 */
static bool
gl_copy_program_create(struct gl_context *context)
{
	/* TODO? we dont need a more complex shader right? */
	static const char vertex_src[] = "attribute vec2 a_pos;"
	                                 "attribute vec2 a_uv;"
	                                 "varying vec2 v_uv;"
	                                 "void main() {"
	                                 "    gl_Position = vec4(a_pos, 0.0, 1.0);"
	                                 "    v_uv = a_uv;"
	                                 "}";
	static const char fragment_src[] =
	    "precision mediump float;"
	    "varying vec2 v_uv;"
	    "uniform sampler2D u_tex;"
	    "void main() {"
	    "    gl_FragColor = texture2D(u_tex, v_uv);"
	    "}";

	GLint status;
	GLint log_len = 0;
	GLint sampler_uniform = -1;
	GLuint vert = 0;
	GLuint frag = 0;
	GLuint program = 0;
	GLuint copy_vbo = 0;

	gl_error_clear();

	vert = gl_shader_compile(GL_VERTEX_SHADER, vertex_src);
	if (!vert) {
		return false;
	}

	frag = gl_shader_compile(GL_FRAGMENT_SHADER, fragment_src);
	if (!frag) {
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}

	program = glCreateProgram();
	if (!program) {
		gl_error_report("gl_copy_program_create create");
		DEBUG("amdgpu: glCreateProgram failed\n");
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}

	glAttachShader(program, vert);
	glAttachShader(program, frag);
	glBindAttribLocation(program, 0, "a_pos");
	glBindAttribLocation(program, 1, "a_uv");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!gl_error_report("gl_copy_program_create link")) {
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}
	if (!status) {
		char log_buf[512];
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
		if (log_len > 1) {
			glGetProgramInfoLog(program, sizeof(log_buf), NULL, log_buf);
			DEBUG("amdgpu: shader program link failed: %s\n", log_buf);
		} else {
			DEBUG("amdgpu: shader program link failed\n");
		}
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}

	sampler_uniform = glGetUniformLocation(program, "u_tex");
	if (sampler_uniform < 0) {
		DEBUG("amdgpu: shader uniform 'u_tex' not found\n");
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}
	if (!gl_error_report("gl_copy_program_create uniforms")) {
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}

	glGenBuffers(1, &copy_vbo);
	if (!copy_vbo) {
		DEBUG("amdgpu: glGenBuffers(copy_vbo) failed\n");
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}
	glBindBuffer(GL_ARRAY_BUFFER, copy_vbo);
	glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	if (!gl_error_report("gl_copy_program_create vbo")) {
		gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
		return false;
	}

	context->copy_program = program;
	context->copy_vbo = copy_vbo;
	context->copy_attr_pos = 0;
	context->copy_attr_uv = 1;
	context->copy_uniform_sampler = sampler_uniform;

	program = 0;
	copy_vbo = 0;
	gl_copy_program_cleanup(&vert, &frag, &program, &copy_vbo);
	return true;
}

bool
gl_draw_copy(struct gl_buffer *src, struct gl_buffer *dst, int32_t dst_x,
             int32_t dst_y, int32_t src_x, int32_t src_y, uint32_t width,
             uint32_t height)
{
	struct gl_context *context = dst->context;
	float x0, x1, y0, y1;
	float u0, u1, v0, v1;
	float verts[16];

	if (!src->context || !dst->context || src->context != dst->context) {
		return false;
	}

	if (!gl_buffer_ensure(src) || !gl_buffer_ensure(dst) ||
	    !gl_make_current(context)) {
		DEBUG(
		    "amdgpu: gl_draw_copy precheck failed (src/dst import/current)\n");
		return false;
	}
	if (!context->copy_program || !context->copy_vbo ||
	    context->copy_uniform_sampler < 0) {
		DEBUG("amdgpu: gl_draw_copy invalid copy program state\n");
		return false;
	}
	if (!gpu_access_ensure(src, false) || !gpu_access_ensure(dst, true)) {
		DEBUG("amdgpu: gl_draw_copy gpu_access_ensure failed\n");
		return false;
	}

	gl_error_clear();

	x0 = -1.0f + 2.0f * (float)dst_x / dst->width;
	x1 = -1.0f + 2.0f * (float)(dst_x + width) / dst->width;
	y0 = -1.0f + 2.0f * (float)dst_y / dst->height;
	y1 = -1.0f + 2.0f * (float)(dst_y + height) / dst->height;

	u0 = (float)src_x / src->width;
	u1 = (float)(src_x + width) / src->width;
	v0 = (float)src_y / src->height;
	v1 = (float)(src_y + height) / src->height;

	/* TODO? i dont think ive flipped any viewports? */
	verts[0] = x0;
	verts[1] = y0;
	verts[2] = u0;
	verts[3] = v0;
	verts[4] = x1;
	verts[5] = y0;
	verts[6] = u1;
	verts[7] = v0;
	verts[8] = x0;
	verts[9] = y1;
	verts[10] = u0;
	verts[11] = v1;
	verts[12] = x1;
	verts[13] = y1;
	verts[14] = u1;
	verts[15] = v1;

	/* setup GL state, draw */
	glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
	glViewport(0, 0, dst->width, dst->height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glUseProgram(context->copy_program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src->texture);
	glUniform1i(context->copy_uniform_sampler, 0);

	glBindBuffer(GL_ARRAY_BUFFER, context->copy_vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
	glEnableVertexAttribArray(context->copy_attr_pos);
	glEnableVertexAttribArray(context->copy_attr_uv);
	glVertexAttribPointer(context->copy_attr_pos, 2, GL_FLOAT, GL_FALSE,
	                      4 * sizeof(float), (const GLvoid *)0);
	glVertexAttribPointer(context->copy_attr_uv, 2, GL_FLOAT, GL_FALSE,
	                      4 * sizeof(float),
	                      (const GLvoid *)(uintptr_t)(2 * sizeof(float)));
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(context->copy_attr_pos);
	glDisableVertexAttribArray(context->copy_attr_uv);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	if (!gl_error_report("gl_draw_copy")) {
		return false;
	}

	if (src == dst) {
		if (!gl_buffer_mark_gpu_busy(dst, GL_ACCESS_READ | GL_ACCESS_WRITE)) {
			return false;
		}
	} else if (!gl_buffer_mark_gpu_busy(src, GL_ACCESS_READ) ||
	           !gl_buffer_mark_gpu_busy(dst, GL_ACCESS_WRITE)) {
		return false;
	}

	glFlush();
	return true;
}

bool
gl_draw_fill(struct gl_buffer *dst, uint32_t color, int32_t x, int32_t y,
             uint32_t width, uint32_t height)
{
	struct gl_context *context = dst->context;
	/* ARGB->RGBA-floats */
	float a = ((color >> 24) & 0xff) / 255.0f;
	float r = ((color >> 16) & 0xff) / 255.0f;
	float g = ((color >> 8) & 0xff) / 255.0f;
	float b = ((color >> 0) & 0xff) / 255.0f;
	int scissor_y = y;

	if (!gl_buffer_ensure(dst) || !gl_make_current(context)) {
		DEBUG("amdgpu: gl_draw_fill precheck failed (target import/current)\n");
		return false;
	}
	if (!gpu_access_ensure(dst, true)) {
		DEBUG("amdgpu: gl_draw_fill gpu_access_ensure failed\n");
		return false;
	}

	gl_error_clear();

	glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
	glViewport(0, 0, dst->width, dst->height);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, scissor_y, width, height);
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	if (!gl_error_report("gl_draw_fill")) {
		return false;
	}

	if (!gl_buffer_mark_gpu_busy(dst, GL_ACCESS_WRITE)) {
		return false;
	}

	glFlush();
	return true;
}

/**
 * @brief Rinse out all pending GL error flags.
 */
static void
gl_error_clear(void)
{
	while (glGetError() != GL_NO_ERROR)
		;
}

/**
 * @brief Convert GL error code to message.
 *
 * @param error GL error code.
 * @return Const string describing error message of code.
 */
static const char *
gl_error_name(GLenum error)
{
	switch (error) {
	case GL_NO_ERROR:
		return "GL_NO_ERROR";
	case GL_INVALID_ENUM:
		return "GL_INVALID_ENUM";
	case GL_INVALID_VALUE:
		return "GL_INVALID_VALUE";
	case GL_INVALID_OPERATION:
		return "GL_INVALID_OPERATION";
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		return "GL_INVALID_FRAMEBUFFER_OPERATION";
	case GL_OUT_OF_MEMORY:
		return "GL_OUT_OF_MEMORY";
	default:
		return "GL_UNKNOWN";
	}
}

/**
 * @brief Log and report pending GL errors after an operation.
 *
 * @param where Label describing the operation being checked.
 * @return true if no errors present. Otherwise false.
 */
static bool
gl_error_report(const char *where)
{
	GLenum error;
	bool ok = true;

	while ((error = glGetError()) != GL_NO_ERROR) {
		DEBUG("amdgpu: GL error at %s: 0x%x (%s)\n", where, error,
		      gl_error_name(error));
		ok = false;
	}

	return ok;
}

void
gl_finalize(struct gl_context *context)
{
	if (!context->gl_ready && !context->gbm && !context->egl_initialized) {
		return;
	}

	DEBUG("amdgpu: finalizing GL resources\n");
	gl_context_cleanup(context);
}

bool
gl_init(struct gl_context *context)
{
	struct gl_context next;
	struct gl_context *ctx = &next;
	EGLint major, minor;
	EGLint num_configs;
	EGLConfig configs[16];
	EGLint config_id = 0;
	EGLint red_size = 0;
	EGLint green_size = 0;
	EGLint blue_size = 0;
	EGLint alpha_size = 0;
	bool have_pbuffer_config = false;
	bool surfaceless = false;
	bool chosen = false;

	const EGLint cfg_rgba8_pbuffer[] = {EGL_SURFACE_TYPE,
	                                    EGL_PBUFFER_BIT,
	                                    EGL_RENDERABLE_TYPE,
	                                    EGL_OPENGL_ES2_BIT,
	                                    EGL_RED_SIZE,
	                                    8,
	                                    EGL_GREEN_SIZE,
	                                    8,
	                                    EGL_BLUE_SIZE,
	                                    8,
	                                    EGL_ALPHA_SIZE,
	                                    8,
	                                    EGL_NONE};

	const EGLint cfg_rgb8_pbuffer[] = {EGL_SURFACE_TYPE,
	                                   EGL_PBUFFER_BIT,
	                                   EGL_RENDERABLE_TYPE,
	                                   EGL_OPENGL_ES2_BIT,
	                                   EGL_RED_SIZE,
	                                   8,
	                                   EGL_GREEN_SIZE,
	                                   8,
	                                   EGL_BLUE_SIZE,
	                                   8,
	                                   EGL_NONE};

	const EGLint cfg_rgba8[] = {EGL_RENDERABLE_TYPE,
	                            EGL_OPENGL_ES2_BIT,
	                            EGL_RED_SIZE,
	                            8,
	                            EGL_GREEN_SIZE,
	                            8,
	                            EGL_BLUE_SIZE,
	                            8,
	                            EGL_ALPHA_SIZE,
	                            8,
	                            EGL_NONE};

	const EGLint cfg_minimal_es2[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	                                  EGL_NONE};

	const struct {
		const char *name;
		const EGLint *attrs;
		bool needs_pbuffer;
	} candidates[] = {
	    {"rgba8+pbuffer", cfg_rgba8_pbuffer, true},
	    {"rgb8+pbuffer", cfg_rgb8_pbuffer, true},
	    {"rgba8", cfg_rgba8, false},
	    {"minimal-es2", cfg_minimal_es2, false},
	};

	EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLint pbuffer_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
	const char *egl_ext;
	const char *gl_ext;
	const char *gl_renderer;
	const char *gl_vendor;
	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;

	gl_context_reset(ctx);
	ctx->drm_fd = context->drm_fd;

	/* create gbm device for egl */
	ctx->gbm = gbm_create_device(ctx->drm_fd);
	if (!ctx->gbm) {
		DEBUG("amdgpu: gbm_create_device(fd=%d) failed\n", ctx->drm_fd);
		return false;
	}
	DEBUG("amdgpu: gbm_create_device succeeded (fd=%d)\n", ctx->drm_fd);

	eglGetPlatformDisplayEXT =
	    (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
	        "eglGetPlatformDisplayEXT");
	if (eglGetPlatformDisplayEXT) {
		ctx->egl_display =
		    eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, ctx->gbm, NULL);
	} else {
		ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->gbm);
		DEBUG("amdgpu: eglGetPlatformDisplayEXT unavailable, using "
		      "eglGetDisplay\n");
	}
	if (ctx->egl_display == EGL_NO_DISPLAY) {
		DEBUG("amdgpu: no EGL display: 0x%x\n", eglGetError());
		gl_context_cleanup(ctx);
		return false;
	}

	if (!eglInitialize(ctx->egl_display, &major, &minor)) {
		DEBUG("amdgpu: eglInitialize failed: 0x%x\n", eglGetError());
		gl_context_cleanup(ctx);
		return false;
	}
	ctx->egl_initialized = true;

	DEBUG("amdgpu: eglInitialize succeeded version=%d.%d\n", major, minor);

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		DEBUG("amdgpu: eglBindAPI(EGL_OPENGL_ES_API) failed: 0x%x\n",
		      eglGetError());
		gl_context_cleanup(ctx);
		return false;
	}

	/* find EGL config; prefer configs with pbuffer support */
	for (int i = 0; i < ARRAY_LENGTH(candidates); i++) {
		num_configs = 0;
		if (!eglChooseConfig(ctx->egl_display, candidates[i].attrs, NULL, 0,
		                     &num_configs)) {
			continue;
		}

		if (num_configs <= 0) {
			continue;
		}

		if (num_configs > ARRAY_LENGTH(configs)) {
			num_configs = ARRAY_LENGTH(configs);
		}

		/* get real configs to query id and rgb sizes */
		if (!eglChooseConfig(ctx->egl_display, candidates[i].attrs, configs,
		                     num_configs, &num_configs) ||
		    num_configs <= 0) {
			continue;
		}

		ctx->egl_config = configs[0];
		have_pbuffer_config = candidates[i].needs_pbuffer;
		chosen = true;
		DEBUG("amdgpu: selected EGL config candidate '%s'\n",
		      candidates[i].name);
		break;
	}

	if (!chosen) {
		DEBUG("amdgpu: no usable EGL config found");
		gl_context_cleanup(ctx);
		return false;
	}

	eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_CONFIG_ID,
	                   &config_id);
	eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_RED_SIZE,
	                   &red_size);
	eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_GREEN_SIZE,
	                   &green_size);
	eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_BLUE_SIZE,
	                   &blue_size);
	eglGetConfigAttrib(ctx->egl_display, ctx->egl_config, EGL_ALPHA_SIZE,
	                   &alpha_size);
	DEBUG("amdgpu: using EGL config id=%d rgba=%d/%d/%d/%d pbuffer=%d\n",
	      config_id, red_size, green_size, blue_size, alpha_size,
	      have_pbuffer_config);

	ctx->egl_context =
	    eglCreateContext(ctx->egl_display, ctx->egl_config, EGL_NO_CONTEXT,
	                     ctx_attrs);
	if (ctx->egl_context == EGL_NO_CONTEXT) {
		DEBUG("amdgpu: eglCreateContext failed: 0x%x\n", eglGetError());
		gl_context_cleanup(ctx);
		return false;
	}

	if (have_pbuffer_config) {
		ctx->egl_surface = eglCreatePbufferSurface(ctx->egl_display,
		                                           ctx->egl_config,
		                                           pbuffer_attrs);
		if (ctx->egl_surface == EGL_NO_SURFACE) {
			DEBUG("amdgpu: eglCreatePbufferSurface failed: 0x%x\n",
			      eglGetError());
		}
	}

	if (ctx->egl_surface == EGL_NO_SURFACE) {
		egl_ext = eglQueryString(ctx->egl_display, EGL_EXTENSIONS);
		surfaceless = egl_ext && strstr(egl_ext, "EGL_KHR_surfaceless_context");
		if (!surfaceless) {
			DEBUG("amdgpu: no pbuffer surface and no "
			      "EGL_KHR_surfaceless_context\n");
			gl_context_cleanup(ctx);
			return false;
		}
		DEBUG("amdgpu: using surfaceless EGL context\n");
	}

	if (!eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface,
	                    ctx->egl_context)) {
		DEBUG("amdgpu: initial eglMakeCurrent failed: 0x%x\n", eglGetError());
		gl_context_cleanup(ctx);
		return false;
	}

	egl_ext = eglQueryString(ctx->egl_display, EGL_EXTENSIONS);
	gl_ext = (const char *)glGetString(GL_EXTENSIONS);
	gl_renderer = (const char *)glGetString(GL_RENDERER);
	gl_vendor = (const char *)glGetString(GL_VENDOR);
	/* check for required extensions */
	if (!egl_ext || !gl_ext ||
	    !strstr(egl_ext, "EGL_EXT_image_dma_buf_import") ||
	    !strstr(gl_ext, "GL_OES_EGL_image")) {
		DEBUG(
		    "amdgpu: required extensions missing " /* TODO: make this cleaner */
		    "(egl_ext=%p has_dmabuf=%d gl_ext=%p has_oes_image=%d)\n",
		    egl_ext,
		    egl_ext ? !!strstr(egl_ext, "EGL_EXT_image_dma_buf_import") : 0,
		    gl_ext, gl_ext ? !!strstr(gl_ext, "GL_OES_EGL_image") : 0);
		gl_context_cleanup(ctx);
		return false;
	}
	DEBUG("AMDGPU EGL renderer: vendor='%s' renderer='%s'\n",
	      gl_vendor ? gl_vendor : "(null)",
	      gl_renderer ? gl_renderer : "(null)");
	if (gl_renderer && strstr(gl_renderer, "llvmpipe")) {
		DEBUG("amdgpu: WARNING llvmpipe renderer on GBM EGL path\n");
	}

	/* required entry points */
	ctx->eglCreateImageKHR =
	    (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	ctx->eglDestroyImageKHR =
	    (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	ctx->eglCreateSyncKHR =
	    (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
	ctx->eglDestroySyncKHR =
	    (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
	ctx->eglClientWaitSyncKHR =
	    (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
	ctx->glEGLImageTargetTexture2DOES =
	    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
	        "glEGLImageTargetTexture2DOES");
	if (!ctx->eglCreateImageKHR || !ctx->eglDestroyImageKHR ||
	    !ctx->glEGLImageTargetTexture2DOES) {
		DEBUG("amdgpu: required EGL image entry points missing\n");
		gl_context_cleanup(ctx);
		return false;
	}
	if (!strstr(egl_ext, "EGL_KHR_fence_sync") || !ctx->eglCreateSyncKHR ||
	    !ctx->eglDestroySyncKHR || !ctx->eglClientWaitSyncKHR) {
		DEBUG("amdgpu: EGL_KHR_fence_sync is required\n");
		gl_context_cleanup(ctx);
		return false;
	}

	if (!gl_copy_program_create(ctx)) {
		DEBUG("amdgpu: gl_copy_program_create failed\n");
		gl_context_cleanup(ctx);
		return false;
	}

	ctx->gl_ready = true;
	*context = next;
	return true;
}

bool
gl_make_current(struct gl_context *context)
{
	EGLDisplay current_display;
	EGLContext current_context;
	EGLSurface current_draw;
	EGLSurface current_read;

	if (!context->gl_ready) {
		DEBUG("amdgpu: gl_make_current called while gl_ready=false\n");
		return false;
	}

	current_display = eglGetCurrentDisplay();
	current_context = eglGetCurrentContext();
	current_draw = eglGetCurrentSurface(EGL_DRAW);
	current_read = eglGetCurrentSurface(EGL_READ);

	/* context is already current */
	if (current_display == context->egl_display &&
	    current_context == context->egl_context &&
	    current_draw == context->egl_surface &&
	    current_read == context->egl_surface) {
		return true;
	}

	if (eglMakeCurrent(context->egl_display, context->egl_surface,
	                   context->egl_surface,
	                   context->egl_context) != EGL_TRUE) {
		DEBUG("amdgpu: eglMakeCurrent failed: 0x%x\n", eglGetError());
		return false;
	}

	return true;
}

/**
 * @brief Compile GLSL shader, report compile diagnostics.
 *
 * @param type GL shader type (eg GL_VERTEX_SHADER).
 * @param source NULL terminated GLSL source string.
 * @return Handle to shader on success, 0 on failure.
 */
static GLuint
gl_shader_compile(GLenum type, const char *source)
{
	GLint status;
	GLint log_len = 0;
	GLuint shader;
	char log_buf[512];

	gl_error_clear();
	shader = glCreateShader(type);

	if (!shader) {
		gl_error_report("gl_shader_compile create");
		DEBUG("amdgpu: glCreateShader failed (type=0x%x)\n", type);
		return 0;
	}

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!gl_error_report("gl_shader_compile")) {
		glDeleteShader(shader);
		return 0;
	}
	if (!status) {
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
		if (log_len > 1) {
			glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
			DEBUG("amdgpu: shader compile failed (type=0x%x): %s\n", type,
			      log_buf);
		} else {
			DEBUG("amdgpu: shader compile failed (type=0x%x)\n", type);
		}
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

/**
 * @brief Ensure memory visibileity, mark upcoming GPU access.
 *
 * @param buffer Target buffer.
 * @param write true when upcoming GPU operation writes destinations contents.
 * @return true.
 */
static bool
gpu_access_ensure(struct gl_buffer *buffer, bool write)
{
	/* cpu writes should be complete before gpu commands for bo */
	if (buffer->domain == GL_DOMAIN_CPU && buffer->cpu_dirty) {
		__sync_synchronize();
		buffer->cpu_dirty = false;
	}

	buffer->domain = GL_DOMAIN_GPU;
	if (write) {
		buffer->cpu_dirty = false;
	}

	return true;
}
