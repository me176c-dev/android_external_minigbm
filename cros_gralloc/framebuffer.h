#ifndef CROS_GRALLOC_FRAMEBUFFER_H
#define CROS_GRALLOC_FRAMEBUFFER_H

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <hardware/fb.h>

struct cros_gralloc_framebuffer {
	struct framebuffer_device_t device;
	int fd;
	uint32_t connector_id, crtc_id;
	drmModeModeInfo mode;

	int current_fb, next_fb;
	drmEventContext evctx;
};

#ifdef __cplusplus
extern "C"
#endif
int cros_gralloc_open_framebuffer(struct cros_gralloc_framebuffer **fb, int fd, const struct hw_module_t *mod, struct hw_device_t **dev);

#endif
