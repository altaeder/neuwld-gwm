/**
 * @file amdgpu.c
 * @brief AMDGPU backend implementation for wld.
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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <amdgpu_drm.h>
#include <xf86drm.h>

#include "amdgpu.h"
#include "drm-private.h"
#include "drm.h"
#include "interface/buffer.h"
#include "interface/context.h"
#include "interface/renderer.h"
#include "wld.h"
#define DRM_DRIVER_NAME amdgpu
#include "interface/drm.h"
#include "pixman.h"
#include "wld-private.h"

IMPL(amdgpu_context, wld_context)
IMPL(amdgpu_renderer, wld_renderer)
IMPL(amdgpu_buffer, wld_buffer)

/* prototypes */
void
buffer_destroy(struct buffer *base);
bool
buffer_get_dmabuf_fd(void *data, int *fd);
bool
buffer_map(struct buffer *base);
bool
buffer_unmap(struct buffer *base);
struct buffer *
context_create_buffer(struct wld_context *base, uint32_t width, uint32_t height,
                      uint32_t format, uint32_t flags);
struct wld_renderer *
context_create_renderer(struct wld_context *base);
void
context_destroy(struct wld_context *base);
struct buffer *
context_import_buffer(struct wld_context *base, uint32_t type,
                      union wld_object object, uint32_t width, uint32_t height,
                      uint32_t format, uint32_t pitch);
struct wld_context *
driver_create_context(int drm_fd);
bool
driver_device_supported(uint32_t vendor_id, uint32_t device_id);
uint32_t
renderer_capabilities(struct wld_renderer *renderer, struct buffer *buffer);
void
renderer_copy_rectangle(struct wld_renderer *base, struct buffer *buffer_base,
                        int32_t dst_x, int32_t dst_y, int32_t src_x,
                        int32_t src_y, uint32_t width, uint32_t height);
void
renderer_destroy(struct wld_renderer *base);
void
renderer_draw_text(struct wld_renderer *base, struct font *font, uint32_t color,
                   int32_t x, int32_t y, const char *text, uint32_t length,
                   struct wld_extents *extents);
void
renderer_fill_rectangle(struct wld_renderer *base, uint32_t color, int32_t x,
                        int32_t y, uint32_t width, uint32_t height);
void
renderer_flush(struct wld_renderer *base);
bool
renderer_set_target(struct wld_renderer *base, struct buffer *buffer);

static inline uint64_t
align_u64(uint64_t value, uint64_t alignment);
static bool
buffer_export(struct wld_exporter *exporter, struct wld_buffer *base,
              uint32_t type, union wld_object *object);
static struct buffer *
buffer_new(struct amdgpu_context *context, uint32_t width, uint32_t height,
           uint32_t format, uint32_t pitch, amdgpu_bo_handle bo);
static bool
cpu_access_ensure_copy(struct amdgpu_buffer *src, struct amdgpu_buffer *dst);
static void
context_cleanup(struct amdgpu_context *context);
static bool
context_init_device(struct amdgpu_context *context, int drm_fd,
                    uint32_t *majorv, uint32_t *minorv);
static void
fallback_copy_rectangle(struct amdgpu_renderer *renderer,
                        struct amdgpu_buffer *src, int32_t dst_x, int32_t dst_y,
                        int32_t src_x, int32_t src_y, uint32_t width,
                        uint32_t height);
static void
fallback_fill_rectangle(struct amdgpu_renderer *renderer, uint32_t color,
                        int32_t x, int32_t y, uint32_t width, uint32_t height);
static bool
pixman_target_set(struct amdgpu_renderer *renderer,
                  struct amdgpu_buffer *buffer);
static bool
rect_in_bounds(struct amdgpu_buffer *buffer, int32_t x, int32_t y,
               uint32_t width, uint32_t height);
static bool
rect_overlap(int32_t x1, int32_t y1, uint32_t w1, uint32_t h1, int32_t x2,
             int32_t y2, uint32_t w2, uint32_t h2);

/**
 * @brief Destroy AMDGPU buffer, release related resources.
 *
 * @param base Pointer to base buffer implementation.
 */
void
buffer_destroy(struct buffer *base)
{
	struct amdgpu_buffer *b = amdgpu_buffer(&base->base);

	if (b->mapped) {
		amdgpu_bo_cpu_unmap(b->bo);
	}

	gl_buffer_wait_idle(&b->gl);
	gl_buffer_destroy(&b->gl);
	if (b->dma_buf_fd >= 0) {
		close(b->dma_buf_fd);
	}
	DEBUG("amdgpu: freeing buffer\n");
	amdgpu_bo_free(b->bo);
	free(b);
}

/**
 * @brief Get/export DMA-BUF fd from an AMDGPU BO.
 *
 * @param data Pointer to @ref amdgpu_buffer.
 * @param fd Output DMA-BUF fd.
 * @return true on success, false on export failure.
 */
bool
buffer_get_dmabuf_fd(void *data, int *fd)
{
	struct amdgpu_buffer *buffer = data;
	uint32_t exported_fd;

	if (buffer->dma_buf_fd >= 0) {
		*fd = buffer->dma_buf_fd;
		return true;
	}

	if (amdgpu_bo_export(buffer->bo, amdgpu_bo_handle_type_dma_buf_fd,
	                     &exported_fd) != 0) {
		DEBUG("amdgpu: amdgpu_bo_export(dmabuf) failed\n");
		return false;
	}

	buffer->dma_buf_fd = (int)exported_fd;
	*fd = buffer->dma_buf_fd;
	return true;
}

/**
 * @brief Map AMDGPU buffer for CPU access.
 *
 * Also ensures queued GPU work is complete before mapping.
 *
 * @param base Pointer to base buffer implementation.
 * @return true on success, false on sync/map failure.
 */
bool
buffer_map(struct buffer *base)
{
	struct amdgpu_buffer *b = amdgpu_buffer(&base->base);
	void *data;

	if (b->mapped) {
		return true;
	}

	if (!gl_buffer_wait_idle(&b->gl)) {
		DEBUG("amdgpu: buffer_map wait_buffer_idle failed\n");
		return false;
	}

	/* map buffer to cpu adddress space */
	if (amdgpu_bo_cpu_map(b->bo, &data) != 0) {
		DEBUG("amdgpu: amdgpu_bo_cpu_map failed\n");
		return false;
	}

	b->mapped = true;
	b->base.base.map = data;
	gl_buffer_mark_cpu_access(&b->gl, false);
	return true;
}

/**
 * @brief Unmap amdgu_buffer from CPU access.
 *
 * @param base Pointer to base buffer implementation.
 * @return true on success, false if unmap fails or buffer is not mapped.
 */
bool
buffer_unmap(struct buffer *base)
{
	struct amdgpu_buffer *b = amdgpu_buffer(&base->base);

	if (!b->mapped) {
		return false;
	}

	if (amdgpu_bo_cpu_unmap(b->bo) != 0) {
		DEBUG("amdgpu: amdgpu_bo_cpu_unmap failed\n");
		return false;
	}

	b->mapped = false;
	b->base.base.map = NULL;
	gl_buffer_mark_cpu_access(&b->gl, true);
	return true;
}

/**
 * @brief Allocate new AMDGPU wld buffer.
 *
 * @param base AMDGPU context as base wld_context.
 * @param width Buffer width (px).
 * @param height Buffer height (px).
 * @param format Pixel format.
 * @param flags Buffer creation flags.
 * @return Newly created buffer. NULL on failure.
 */
struct buffer *
context_create_buffer(struct wld_context *base, uint32_t width, uint32_t height,
                      uint32_t format, uint32_t flags)
{
	struct amdgpu_buffer *b;
	struct amdgpu_context *ctx = amdgpu_context(base);
	struct amdgpu_bo_alloc_request alloc_req = {};
	struct amdgpu_buffer_size_alignments alignments = {};

	uint8_t bpp = format_bytes_per_pixel(format);
	uint32_t pitch;
	uint64_t size;
	uint64_t size_alignment = 4096;
	amdgpu_bo_handle bo;

	DEBUG("amdgpu: create_buffer %ux%u fmt=0x%x flags=0x%x\n", width, height,
	      format, flags);
	/* only support fromats that can be imported to GL as textures. Otherwise,
	 * fallback to pixman */
	if (bpp == 0 || !gl_drm_fourcc_from_wld_format(format)) {
		DEBUG("amdgpu: unsupported format 0x%x\n", format);
		return NULL;
	}

	if (amdgpu_query_buffer_size_alignment(ctx->device, &alignments) == 0 &&
	    alignments.size_remote) {
		size_alignment = alignments.size_remote;
	}

	pitch = align_u64((uint64_t)width * bpp, 256);
	size = align_u64((uint64_t)pitch * height, size_alignment);

	alloc_req.alloc_size = size;
	alloc_req.phys_alignment = 4096;
	alloc_req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	alloc_req.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

	if (amdgpu_bo_alloc(ctx->device, &alloc_req, &bo) != 0) {
		DEBUG("amdgpu: amdgpu_bo_alloc failed size=%llu pitch=%u align=%llu\n",
		      (unsigned long long)size, pitch,
		      (unsigned long long)size_alignment);
		return NULL;
	}

	/* wrap to wld buffer */
	b = (void *)buffer_new(ctx, width, height, format, pitch, bo);
	if (!b) {
		DEBUG("amdgpu: buffer_new allocation failed\n");
		amdgpu_bo_free(bo);
		return NULL;
	}

	DEBUG("amdgpu: create_buffer success pitch=%u\n", pitch);

	return &b->base;
}

/**
 * @brief Create amdgpu_renderer (pixman fallback).
 *
 * @param base amdgpu_context as base wld_context.
 * @return renderer. NULL on failure.
 */
struct wld_renderer *
context_create_renderer(struct wld_context *base)
{
	struct amdgpu_context *ctx = amdgpu_context(base);
	struct amdgpu_renderer *r;

	/* create amdgpu renderer */
	r = malloc(sizeof *r);
	if (!r) {
		DEBUG("amdgpu: renderer allocation failed\n");
		return NULL;
	}

	/* create flal back pixman renderer */
	r->pixman_renderer = wld_create_renderer(wld_pixman_context);
	if (!r->pixman_renderer) {
		DEBUG("amdgpu: pixman renderer creation failed\n");
		free(r);
		return NULL;
	}

	renderer_initialize(&r->base, &wld_renderer_impl);
	r->context = ctx;
	r->target = NULL;
	DEBUG("amdgpu: renderer created\n");
	return &r->base;
}

/**
 * @brief Cleanup AMDGPU context
 * @param context AMDGPU context to cleanup
 */
static void
context_cleanup(struct amdgpu_context *context)
{
	if (!context) {
		return;
	}

	gl_finalize(&context->gl);
	if (context->device) {
		amdgpu_device_deinitialize(context->device);
		context->device = NULL;
	}
	if (context->close_drm_fd) {
		close(context->drm_fd);
		context->close_drm_fd = false;
	}
}

/**
 * @brief Initialise GL context for AMDGPU device.
 *
 * @param context AMDGPU context to initialise
 * @param drm_fd DRM fd for device
 * @param majorv Output major version of amdgpu device
 * @param minorv Output minor version of amdgpu device
 * @return true on success, false on failure
 */
static bool
context_init_device(struct amdgpu_context *context, int drm_fd, uint32_t *majorv,
                    uint32_t *minorv)
{
	char *render_name = NULL;
	int render_fd = -1;
	int ret;

	ret = amdgpu_device_initialize(drm_fd, majorv, minorv, &context->device);
	if (ret == 0) {
		return true;
	}

	DEBUG("amdgpu: amdgpu_device_initialize failed ret=%d\n", ret);
	if (ret != -EACCES && ret != -EPERM) {
		return false;
	}

	/* if permission error, retry using render node */
	render_name = drmGetRenderDeviceNameFromFd(drm_fd);
	DEBUG("amdgpu: retrying on render node '%s'\n",
	      render_name ? render_name : "null");
	if (!render_name) {
		return false;
	}

	render_fd = open(render_name, O_RDWR | O_CLOEXEC);
	drmFree(render_name);
	if (render_fd < 0) {
		DEBUG("amdgpu: opening render node failed errno=%d (%s)\n", errno,
		      strerror(errno));
		return false;
	}

	/* initialise device with render node */
	if (amdgpu_device_initialize(render_fd, majorv, minorv, &context->device) !=
	    0) {
		DEBUG("amdgpu: amdgpu_device_initialize(render fd=%d) failed\n",
		      render_fd);
		close(render_fd);
		return false;
	}

	context->drm_fd = render_fd;
	context->close_drm_fd = true;
	context->gl.drm_fd = render_fd;
	DEBUG("amdgpu: render-node initialize succeeded fd=%d\n", render_fd);
	return true;
}

/**
 * @brief Destroy amdgpu_context and all owned driver state.
 *
 * @param base amdgpu_context as base wld_context.
 */
void
context_destroy(struct wld_context *base)
{
	struct amdgpu_context *ctx = amdgpu_context(base);

	DEBUG("amdgpu: destroying context\n");

	context_cleanup(ctx);
	free(ctx);
}

/**
 * @brief Import external buffer object into AMDGPU backend storage.
 *
 * @note Supports PRIME DMA-BUF fd imports.
 *
 * @param base amdgpu_context as base wld_context.
 * @param type Import object type.
 * @param object Import object.
 * @param width Buffer width.
 * @param height Buffer height.
 * @param format Pixel format.
 * @param pitch Row stride in bytes.
 * @return Imported buffer. NULL on failure.
 */
struct buffer *
context_import_buffer(struct wld_context *base, uint32_t type,
                      union wld_object object, uint32_t width, uint32_t height,
                      uint32_t format, uint32_t pitch)
{
	struct buffer *b;
	struct amdgpu_context *ctx = amdgpu_context(base);
	struct amdgpu_bo_import_result res;
	int import_fd = -1;

	switch (type) {
	case WLD_DRM_OBJECT_PRIME_FD:
		/* dup fd to own imported fd */
		import_fd = dup(object.i);
		if (import_fd < 0) {
			DEBUG("amdgpu: dup(PRIME_FD) failed errno=%d (%s)\n", errno,
			      strerror(errno));
			return NULL;
		}

		/* import dmabuf fd to bo handle */
		if (amdgpu_bo_import(ctx->device, amdgpu_bo_handle_type_dma_buf_fd,
		                     import_fd, &res) != 0) {
			DEBUG("amdgpu: import PRIME_FD failed\n");
			close(import_fd);
			return NULL;
		}
		break;

	default:
		DEBUG("amdgpu: unsupported import type=0x%x\n", type);
		return NULL;
	}

	/* wrap to wld buffer */
	b = buffer_new(ctx, width, height, format, pitch, res.buf_handle);
	if (!b) {
		DEBUG("amdgpu: buffer_new failed for import\n");
		amdgpu_bo_free(res.buf_handle);
		close(import_fd);
		return NULL;
	}
	amdgpu_buffer(&b->base)->dma_buf_fd = import_fd;
	DEBUG("amdgpu: import_buffer success\n");

	return b;
}

/**
 * @brief Create and initialise amdgpu_backend context for DRM device.
 *
 * @param drm_fd DRM fd for targeted device.
 * @return Initialised wld_context. NULL on failure.
 */
struct wld_context *
driver_create_context(int drm_fd)
{
	struct amdgpu_context *ctx;
	uint32_t majorv;
	uint32_t minorv;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx) {
		DEBUG("amdgpu: context allocation failed\n");
		return NULL;
	}
	/* own drm fd, initialise device */
	ctx->drm_fd = drm_fd;
	ctx->close_drm_fd = false;
	ctx->gl.drm_fd = drm_fd;
	ctx->gl.egl_display = EGL_NO_DISPLAY;
	ctx->gl.egl_context = EGL_NO_CONTEXT;
	ctx->gl.egl_surface = EGL_NO_SURFACE;

	if (!context_init_device(ctx, drm_fd, &majorv, &minorv)) {
		free(ctx);
		return NULL;
	}
	DEBUG("amdgpu: device initialised (version %u.%u)\n", majorv, minorv);

	/* initialise GL */
	if (!gl_init(&ctx->gl)) {
		DEBUG("amdgpu: gl_init failed\n");
		context_cleanup(ctx);
		free(ctx);
		return NULL;
	}
	context_initialize(&ctx->base, &wld_context_impl);
	DEBUG("amdgpu: context initialised\n");
	return &ctx->base;
}

/**
 * @brief Check if (PCI) device is supported by driver.
 *
 * @param vendor_id PCI vendor ID.
 * @param device_id PCI device ID.
 * @return true for AMD deviecs, false otherwise.
 */
bool
driver_device_supported(uint32_t vendor_id, uint32_t device_id)
{
	return vendor_id == 0x1002; /* AMD/ATI */
}

/**
 * @brief Queuery read/write capabilities for buffer.
 *
 * @param renderer Renderer.
 * @param buffer Target buffer.
 * @return Capabilities bitmask, 0 if renderer/buffer are incompatible.
 */
uint32_t
renderer_capabilities(struct wld_renderer *renderer, struct buffer *buffer)
{
	struct amdgpu_renderer *r;
	struct amdgpu_buffer *b;

	if (!renderer || !buffer) {
		return 0;
	}

	if (renderer->impl != &wld_renderer_impl ||
	    buffer->base.impl != &wld_buffer_impl) {
		return 0;
	}

	r = amdgpu_renderer(renderer);
	b = amdgpu_buffer(&buffer->base);

	/* require buffer ownership from same context */
	if (!r->context || !b->context || b->context != r->context) {
		return 0;
	}

	return WLD_CAPABILITY_READ | WLD_CAPABILITY_WRITE;
}

/**
 * @brief Copy rectangle using GL (pixman fallback).
 *
 * @param base Renderer.
 * @param buffer_base Source buffer.
 * @param dst_x Destination X.
 * @param dst_y Destination Y.
 * @param src_x Source X.
 * @param src_y Source Y.
 * @param width Rectangle width.
 * @param height Rectangle height.
 */
void
renderer_copy_rectangle(struct wld_renderer *base, struct buffer *buffer_base,
                        int32_t dst_x, int32_t dst_y, int32_t src_x,
                        int32_t src_y, uint32_t width, uint32_t height)
{
	struct amdgpu_renderer *r = amdgpu_renderer(base);
	struct amdgpu_buffer *buf_dst = r->target;
	struct amdgpu_buffer *buf_src;
	const char *fallback_reason = NULL;

	if (!buf_dst || !buffer_base ||
	    buffer_base->base.impl != &wld_buffer_impl || width == 0 ||
	    height == 0) {
		return;
	}

	buf_src = amdgpu_buffer(&buffer_base->base);

	if (!rect_in_bounds(buf_src, src_x, src_y, width, height)) {
		fallback_reason = "buf_src-out-of-bounds";
	} else if (!rect_in_bounds(buf_dst, dst_x, dst_y, width, height)) {
		fallback_reason = "buf_dst-out-of-bounds";
	} else if (buf_src == buf_dst &&
	           rect_overlap(src_x, src_y, width, height, dst_x, dst_y, width,
	                        height)) {
		fallback_reason = "self-overlap";
	} else if (!gl_draw_copy(&buf_src->gl, &buf_dst->gl, dst_x, dst_y, src_x,
	                         src_y, width, height)) {
		fallback_reason = "gl_draw_copy-failed";
	}

	if (fallback_reason) {
		DEBUG("amdgpu: copy fallback reason=%s\n", fallback_reason);
		fallback_copy_rectangle(r, buf_src, dst_x, dst_y, src_x, src_y, width,
		                        height);
	}
}

/**
 * @brief Destroy an amdgpu_renderer.
 *
 * @param base Renderer.
 */
void
renderer_destroy(struct wld_renderer *base)
{
	struct amdgpu_renderer *r = amdgpu_renderer(base);

	wld_destroy_renderer(r->pixman_renderer);
	free(r);
}

/**
 * @brief Draw UTF8 text over target.
 *
 * @note There is no native GL text rendering yet. Pixman will always be used
 *
 * @param base Renderer.
 * @param font Font object.
 * @param color Text color (ARGB).
 * @param x Baseline X.
 * @param y Baseline Y.
 * @param text UTF8 text bytes.
 * @param length Byte length.
 * @param extents Output extents (Optional).
 */
void
renderer_draw_text(struct wld_renderer *base, struct font *font, uint32_t color,
                   int32_t x, int32_t y, const char *text, uint32_t length,
                   struct wld_extents *extents)
{
	/* TODO: Dont fallback on pixman! */
	struct amdgpu_renderer *r = amdgpu_renderer(base);
	if (!r->target) {
		return;
	}

	if (!gl_buffer_cpu_access_ensure(&r->target->gl, GL_ACCESS_WRITE)) {
		DEBUG("amdgpu: draw_text cpu_access_ensure failed\n");
		return;
	}

	if (!pixman_target_set(r, r->target)) {
		DEBUG("amdgpu: draw_text pixman_target_set failed\n");
		return;
	}

	wld_draw_text(r->pixman_renderer, &font->base, color, x, y, text, length,
	              extents);
	wld_flush(r->pixman_renderer);
	gl_buffer_mark_cpu_access(&r->target->gl, true);
	pixman_target_set(r, NULL);
}

/**
 * @brief Fill rectangle using GL path (pixman fallback).
 *
 * @param base Renderer.
 * @param color ARGB fill color.
 * @param x Rectangle X.
 * @param y Rectangle Y.
 * @param width Rectangle width.
 * @param height Rectangle height.
 */
void
renderer_fill_rectangle(struct wld_renderer *base, uint32_t color, int32_t x,
                        int32_t y, uint32_t width, uint32_t height)
{
	struct amdgpu_renderer *r = amdgpu_renderer(base);
	struct amdgpu_buffer *buf_dst = r->target;
	const char *fallback_reason = NULL;

	if (!buf_dst || width == 0 || height == 0) {
		return;
	}

	if (!rect_in_bounds(buf_dst, x, y, width, height)) {
		fallback_reason = "out-of-bounds";
	} else if (!gl_draw_fill(&buf_dst->gl, color, x, y, width, height)) {
		fallback_reason = "gl_draw_fill-failed";
	}

	if (fallback_reason) {
		DEBUG("amdgpu: fill fallback reason=%s\n", fallback_reason);
		fallback_fill_rectangle(r, color, x, y, width, height);
	}
}

/**
 * @brief Flush queued GPU work.
 *
 * @param base Renderer.
 */
void
renderer_flush(struct wld_renderer *base)
{
	struct amdgpu_renderer *r = amdgpu_renderer(base);
	if (r->context->gl.gl_ready && gl_make_current(&r->context->gl)) {
		glFlush();
	} else if (r->context->gl.gl_ready) {
		DEBUG("amdgpu: renderer_flush gl_make_current failed\n");
	}
}

/**
 * @brief Set render target buffer.
 *
 * @param base Renderer.
 * @param buffer Target buffer, NULL to unset.
 * @return true on success, false if buffer type incompatible.
 */
bool
renderer_set_target(struct wld_renderer *base, struct buffer *buffer)
{
	struct amdgpu_renderer *r = amdgpu_renderer(base);
	if (buffer && buffer->base.impl != &wld_buffer_impl) {
		DEBUG("amdgpu: renderer_set_target rejected non-wld buffer\n");
		return false;
	}

	r->target = buffer ? amdgpu_buffer(&buffer->base) : NULL;
	return true;
}

/**
 * @brief Align a value upward to a power of two alignment.
 *
 * @param value Input value.
 * @param alignment Power of two alignment.
 * @return Aligned value.
 */
static inline uint64_t
align_u64(uint64_t value, uint64_t alignment)
{
	assert(alignment != 0);
	assert((alignment & (alignment - 1)) == 0);

	if (alignment <= 1) {
		return value;
	}

	return (value + alignment - 1) & -alignment;
}

/**
 * @brief Export amdgpu_buffer as DRM handle or PRIME fd.
 *
 * @param exporter Export interface.
 * @param base Source buffer.
 * @param type Requested export type.
 * @param object Output object payload.
 * @return true on export success. Otherwise false.
 */
static bool
buffer_export(struct wld_exporter *exporter, struct wld_buffer *base,
              uint32_t type, union wld_object *object)
{
	struct amdgpu_buffer *b = amdgpu_buffer(base);
	uint32_t hdl;
	int fd;
	int kms = amdgpu_bo_handle_type_kms;

	switch (type) {
	/* export buffer obj handle */
	case WLD_DRM_OBJECT_HANDLE:
		if (amdgpu_bo_export(b->bo, kms, &hdl) != 0) {
			DEBUG("amdgpu: export KMS handle failed\n");
			return false;
		}

		object->u32 = hdl;
		return true;
	case WLD_DRM_OBJECT_PRIME_FD:
		if (!buffer_get_dmabuf_fd(b, &fd)) {
			DEBUG("amdgpu: export PRIME_FD failed\n");
			return false;
		}

		/* keep cached fd, return duped fd */
		object->i = dup(fd);
		if (object->i < 0) {
			DEBUG("amdgpu: dup(PRIME_FD) failed errno=%d (%s)\n", errno,
			      strerror(errno));
			return false;
		}
		return true;
	default:
		return false;
	}
}

/**
 * @brief Allocate and initialise amdgpu_buffer wrapper.
 *
 * @param context Owning amdgpu_context.
 * @param width Buffer width.
 * @param height Buffer height.
 * @param format Pixel format.
 * @param pitch Row stride in bytes.
 * @param bo amdhpu_buffer object handle.
 * @return New buffer wrapper, NULL if allocation failure.
 */
static struct buffer *
buffer_new(struct amdgpu_context *context, uint32_t width, uint32_t height,
           uint32_t format, uint32_t pitch, amdgpu_bo_handle bo)
{
	struct amdgpu_buffer *b = malloc(sizeof *b);
	if (!b) {
		return NULL;
	}

	/* initialize buffer */
	buffer_initialize(&b->base, &wld_buffer_impl, width, height, format, pitch);
	b->context = context;
	b->bo = bo;
	b->mapped = false;
	b->dma_buf_fd = -1;
	b->gl.context = &context->gl;
	b->gl.get_dmabuf_fd = buffer_get_dmabuf_fd;
	b->gl.get_dmabuf_fd_data = b;
	b->gl.width = width;
	b->gl.height = height;
	b->gl.format = format;
	b->gl.pitch = pitch;
	b->gl.gpu_pending_ops = 0;
	b->gl.gpu_pending_access = 0;
	b->gl.gl_fence = EGL_NO_SYNC_KHR;
	b->gl.domain = GL_DOMAIN_NONE;
	b->gl.cpu_dirty = false;
	b->gl.egl_image = EGL_NO_IMAGE_KHR;
	b->gl.texture = 0;
	b->gl.fbo = 0;
	b->gl.gl_buffer_ready = false;
	b->exporter.export = &buffer_export;
	wld_buffer_add_exporter(&b->base.base, &b->exporter);

	return &b->base;
}

/**
 * @brief Make sure CPU access is ok for pixman copy operations.
 *
 * @param src Source buffer.
 * @param dst Destination buffer.
 * @return true if buffers safe. Otherwise false.
 */
static bool
cpu_access_ensure_copy(struct amdgpu_buffer *src, struct amdgpu_buffer *dst)
{
	if (!gl_buffer_cpu_access_ensure(&dst->gl, GL_ACCESS_WRITE)) {
		return false;
	}

	if (src != dst && !gl_buffer_cpu_access_ensure(&src->gl, GL_ACCESS_READ)) {
		return false;
	}

	return true;
}

/**
 * @brief Copy rectangle with pixman fallback renderer.
 *
 * @param renderer Renderer.
 * @param src Source amdgpu_buffer.
 * @param dst_x Destination X.
 * @param dst_y Destination Y.
 * @param src_x Source X.
 * @param src_y Source Y.
 * @param width Rectangle width.
 * @param height Rectangle height.
 */
static void
fallback_copy_rectangle(struct amdgpu_renderer *renderer,
                        struct amdgpu_buffer *src, int32_t dst_x, int32_t dst_y,
                        int32_t src_x, int32_t src_y, uint32_t width,
                        uint32_t height)
{
	if (!cpu_access_ensure_copy(src, renderer->target)) {
		return;
	}

	if (!pixman_target_set(renderer, renderer->target)) {
		return;
	}

	wld_copy_rectangle(renderer->pixman_renderer, &src->base.base, dst_x, dst_y,
	                   src_x, src_y, width, height);
	wld_flush(renderer->pixman_renderer);
	gl_buffer_mark_cpu_access(&renderer->target->gl, true);
	pixman_target_set(renderer, NULL);
}

/**
 * @brief Fill rectangle with pixman fallback renderer.
 *
 * @param renderer Renderer.
 * @param color Fill color (ARGB).
 * @param x Rectangle X.
 * @param y Rectangle Y.
 * @param width Rectangle width.
 * @param height Rectangle height.
 */
static void
fallback_fill_rectangle(struct amdgpu_renderer *renderer, uint32_t color,
                        int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	if (!gl_buffer_cpu_access_ensure(&renderer->target->gl, GL_ACCESS_WRITE)) {
		return;
	}

	if (!pixman_target_set(renderer, renderer->target)) {
		return;
	}

	wld_fill_rectangle(renderer->pixman_renderer, color, x, y, width, height);
	wld_flush(renderer->pixman_renderer);
	gl_buffer_mark_cpu_access(&renderer->target->gl, true);
	pixman_target_set(renderer, NULL);
}

/**
 * @brief Update pixman renderer target from amdgpu_buffer.
 *
 * @param renderer Renderer.
 * @param buffer Target buffer, NULL to clear target.
 * @return true if target switch succeeded.
 */
static bool
pixman_target_set(struct amdgpu_renderer *renderer,
                  struct amdgpu_buffer *buffer)
{
	return wld_set_target_buffer(renderer->pixman_renderer,
	                             buffer ? &buffer->base.base : NULL);
}

/**
 * @brief Check if a rectangle is fully contained inside a buffer.
 *
 * @param buffer Target buffer.
 * @param x Rectangle X.
 * @param y Rectangle Y.
 * @param width Rectangle width.
 * @param height Rectangle height.
 * @return true when the rectangle lies fully in bounds.
 */
static bool
rect_in_bounds(struct amdgpu_buffer *buffer, int32_t x, int32_t y,
               uint32_t width, uint32_t height)
{
	if (x < 0 || y < 0) {
		return false;
	}

	if ((uint64_t)x + width > buffer->base.base.width ||
	    (uint64_t)y + height > buffer->base.base.height) {
		return false;
	}

	return true;
}

/**
 * @brief Test overlap between two rectangles.
 *
 * @param x1 Rectangle 1 X.
 * @param y1 Rectangle 1 Y.
 * @param w1 Rectangle 1 width.
 * @param h1 Rectangle 1 height.
 * @param x2 Rectangle 2 X.
 * @param y2 Rectangle 2 Y.
 * @param w2 Rectangle 2 width.
 * @param h2 Rectangle 2 height.
 * @return If rectangles overlap, true. Otherwise false.
 */
static bool
rect_overlap(int32_t x1, int32_t y1, uint32_t w1, uint32_t h1, int32_t x2,
             int32_t y2, uint32_t w2, uint32_t h2)
{
	int64_t x1_max = (int64_t)x1 + w1;
	int64_t x2_max = (int64_t)x2 + w2;
	int64_t y1_max = (int64_t)y1 + h1;
	int64_t y2_max = (int64_t)y2 + h2;

	return (int64_t)x1 < x2_max && (int64_t)x2 < x1_max &&
	       (int64_t)y1 < y2_max && (int64_t)y2 < y1_max;
}
