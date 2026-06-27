/**
 * @file amdgpu.h
 * @brief AMDGPU backend interface for wld.
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

#ifndef NEUWLD_AMDGPU_H
#define NEUWLD_AMDGPU_H

#include <stdbool.h>
#include <stdint.h>

#include <xf86drm.h>

#include_next <amdgpu.h>

#include "gl.h"
#include "wld-private.h"

/**
 * @brief wld_context for AMDGPU.
 */
struct amdgpu_context {
	struct wld_context base;     /**< wld_context interface. */
	amdgpu_device_handle device; /**< libdrm_amdgpu device. */
	int drm_fd;                  /**< DRM fd used by context. */
	bool close_drm_fd;           /**< If context owns and closes @ref drm_fd. */

	struct gl_context gl; /**< OpenGL context for acceleration. */
};

/**
 * @brief wld_renderer for AMDGPU.
 */
struct amdgpu_renderer {
	struct wld_renderer base;       /**< wld_renderer interface. */
	struct amdgpu_context *context; /**< amdgpu_context (owning). */
	struct wld_renderer
	    *pixman_renderer;         /**< Pixman (software) fallback renderer. */
	struct amdgpu_buffer *target; /**< Render target buffer (current). */
};

/**
 * @brief AMDGPU buffer object.
 */
struct amdgpu_buffer {
	struct buffer base;             /**< wld_buffer implementation. */
	struct wld_exporter exporter;   /**< Export hook for DMA-BUF/DRM objects. */
	struct amdgpu_context *context; /**< amdgpu_context (owning). */
	amdgpu_bo_handle bo;            /**< Buffer object handle. */
	bool mapped;                    /**< True while BO is mapped to cpu. */
	int dma_buf_fd;                 /**< Cached exported DMA-BUF fd, or -1. */

	struct gl_buffer gl; /**< GL metadata buffer. */
};

/**
 * @brief Resolve/export DMA-BUF fd for amdgpu_buffer.
 *
 * @param data Pointer to @ref amdgpu_buffer.
 * @param fd Output DMA-BUF file descriptor.
 * @return true on success, false if export failure.
 */
bool
buffer_get_dmabuf_fd(void *data, int *fd);

#endif
