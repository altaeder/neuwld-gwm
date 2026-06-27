/**
 * @file gl.h
 * @brief OpenGL/EGL helpers interface for wld buffers.
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

#ifndef NEUWLD_GL_H
#define NEUWLD_GL_H

#include <stdbool.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_NO_IMAGE_KHR
#define EGL_NO_IMAGE_KHR ((EGLImageKHR)0)
#endif
#ifndef EGL_NO_SYNC_KHR
#define EGL_NO_SYNC_KHR ((EGLSyncKHR)0)
#endif
#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR 0x30F9
#endif
#ifndef EGL_TIMEOUT_EXPIRED_KHR
#define EGL_TIMEOUT_EXPIRED_KHR 0x30F5
#endif
#ifndef EGL_CONDITION_SATISFIED_KHR
#define EGL_CONDITION_SATISFIED_KHR 0x30F6
#endif

enum gl_domain {
	GL_DOMAIN_NONE, /**< Buffer contents are not owned */
	GL_DOMAIN_CPU,  /**< Buffer contents are owned by CPU access. */
	GL_DOMAIN_GPU,  /**< Buffer contents are owned by GPU access. */
};

/**
 * @brief Access flags for sync between CPU and GPU domains.
 */
enum gl_access {
	GL_ACCESS_READ = 1 << 0,  /**< Read access to buffer data. */
	GL_ACCESS_WRITE = 1 << 1, /**< Write access to buffer data. */
};

/**
 * @brief Callback used to resolve DMA-BUF file descriptor for a buffer.
 *
 * @param data Opaque data pointer.
 * @param fd Output fd on success.
 * @return true on success, false on failure.
 */
typedef bool (*gl_buffer_get_dmabuf_fd_func)(void *data, int *fd);

/**
 * @brief Shared OpenGL/EGL state.
 */
struct gl_context {
	int drm_fd;             /**< DRM fd used for GBM/EGL setup. */
	struct gbm_device *gbm; /**< GBM device created from @ref drm_fd. */
	EGLDisplay egl_display; /**< EGL display bounded to GBM. */
	EGLConfig egl_config;   /**< EGL configuration. */
	EGLContext egl_context; /**< OpenGL ES context. */
	EGLSurface egl_surface; /**< Dummy EGL surface for making current calls. */

	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR; /**< eglCreateImageKHR entry. */
	PFNEGLDESTROYIMAGEKHRPROC
	    eglDestroyImageKHR;                   /**< eglDestroyImageKHR entry. */
	PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR; /**< eglCreateSyncKHR entry. */
	PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR; /**< eglDestroySyncKHR entry. */
	PFNEGLCLIENTWAITSYNCKHRPROC
	eglClientWaitSyncKHR; /**< eglClientWaitSyncKHR entry. */
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
	glEGLImageTargetTexture2DOES; /**< glEGLImageTargetTexture2DOES entry. */

	GLuint copy_program; /**< Program used for textured blit operations. */
	GLuint copy_vbo;     /**< Fullscreen quad vbo. */
	GLint copy_attr_pos; /**< Attribute location for vertex positions. */
	GLint copy_attr_uv;  /**< Attribute location for texture UVs. */
	GLint copy_uniform_sampler; /**< Uniform location for source texture. */

	bool egl_initialized; /**< True after eglInitialize success. */
	bool gl_ready; /**< True if GL/EGL state are initialised and usable. */
};

/**
 * @brief GL metadata used for interop and sync.
 */
struct gl_buffer {
	struct gl_context *context; /**< Owning GL context. */
	gl_buffer_get_dmabuf_fd_func
	    get_dmabuf_fd;           /**< Callback resolving backing DMA-BUF fd. */
	void *get_dmabuf_fd_data;    /**< Opaque callback payload. */
	uint32_t width;              /**< Buffer width (px). */
	uint32_t height;             /**< Buffer height (px). */
	uint32_t format;             /**< wld pixel format (enum wld_format). */
	uint32_t pitch;              /**< Row stride in bytes. */
	uint32_t gpu_pending_ops;    /**< Number of penigng GPU operations. */
	uint32_t gpu_pending_access; /**< O|R'ed pending access flags. */
	EGLSyncKHR gl_fence;         /**< Fence tracking latest GPU use. */
	enum gl_domain domain;       /**< Last ownership domain. */
	bool cpu_dirty;              /**< CPU writes not used by GPU. */
	EGLImageKHR egl_image;       /**< EGLImage imported from DMA-BUF. */
	GLuint texture;              /**< GL texture view of @ref egl_image. */
	GLuint fbo;                  /**< FBO used for rendering into texture. */
	bool gl_buffer_ready;        /**< True when GL objects are initialised. */
};

/**
 * @brief Ensure CPU visibility and mark upcoming GPU access intent.
 *
 * Sync queued GPU work before CPU has access to buffer.
 *
 * @param buffer Buffer to sync.
 * @param access Requested CPU access mask from @ref gl_access.
 * @return true on CPU safe access, false if invalid access/timeout.
 */
bool
gl_buffer_cpu_access_ensure(struct gl_buffer *buffer, uint32_t access);

/**
 * @brief Release GL objects bound to a buffer import.
 *
 * @param buffer Buffer whose objects should be destroyed.
 */
void
gl_buffer_destroy(struct gl_buffer *buffer);

/**
 * @brief Record that CPU access has occurred on a buffer.
 *
 * @param buffer Target GL buffer state.
 * @param write true if CPU writes were performed.
 */
void
gl_buffer_mark_cpu_access(struct gl_buffer *buffer, bool write);

/**
 * @brief Mark buffer (GPU busy), attach sync fence.
 *
 * @param buffer Target buffer.
 * @param access Access flags used by queued GPU operation.
 * @return true on success, false if sync unavailable.
 */
bool
gl_buffer_mark_gpu_busy(struct gl_buffer *buffer, uint32_t access);

/**
 * @brief Wait until all queued GPU operations finish for buffer.
 *
 * @param buffer Target buffer.
 * @return true if buffer reached idle, false on wait failure/timeout.
 */
bool
gl_buffer_wait_idle(struct gl_buffer *buffer);

/**
 * @brief Translate wld format to DRM FourCC format.
 *
 * @param format wld pixel format.
 * @return DRM format code, 0 for unsupported format.
 */
uint32_t
gl_drm_fourcc_from_wld_format(uint32_t format);

/**
 * @brief Copy rectangul between two buffers.
 *
 * @param src Source buffer.
 * @param dst Destination buffer.
 * @param dst_x Destination rectangle X.
 * @param dst_y Destination rectangle Y.
 * @param src_x Source rectangle X.
 * @param src_y Source rectangle Y.
 * @param width Copy width.
 * @param height Copy height.
 * @return true on success, false on validation/GL failure.
 */
bool
gl_draw_copy(struct gl_buffer *src, struct gl_buffer *dst, int32_t dst_x,
             int32_t dst_y, int32_t src_x, int32_t src_y, uint32_t width,
             uint32_t height);

/**
 * @brief Fill rectangle of buffer with solid color.
 *
 * @param dst Destination buffer.
 * @param color Color value (ARGB).
 * @param x Rectangle X coordinate.
 * @param y Rectangle Y coordinate.
 * @param width Rectangle width.
 * @param height Rectangle height.
 * @return true on success, false on validation or GL failure.
 */
bool
gl_draw_fill(struct gl_buffer *dst, uint32_t color, int32_t x, int32_t y,
             uint32_t width, uint32_t height);

/**
 * @brief Tear down GL resources.
 *
 * @param context Context to finalize.
 */
void
gl_finalize(struct gl_context *context);

/**
 * @brief Initialise GBM, EGL, GL rendering.
 *
 * @param context Context to initialise.
 * @return true on success. Otherwise false.
 */
bool
gl_init(struct gl_context *context);

/**
 * @brief Ensure given EGL context is current on this thread.
 *
 * @param context Context to make current.
 * @return true on success, false if eglMakeCurrent failures.
 */
bool
gl_make_current(struct gl_context *context);

#endif
