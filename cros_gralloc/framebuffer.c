#define LOG_TAG "fb0"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/gralloc.h>
#include <cutils/log.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <hardware/fb.h>

#include "cros_gralloc_handle.h"

#define SWAP_INTERVAL 1

struct cros_gralloc_framebuffer {
	struct framebuffer_device_t device;
	struct hw_device_t *gralloc;

	int fd;
	uint32_t connector_id, crtc_id;
	drmModeModeInfo mode;

	uint32_t current_fb, next_fb;
	drmEventContext evctx;
};

extern cros_gralloc_handle_t cros_gralloc_convert_handle(buffer_handle_t handle);

static drmModeConnectorPtr fb0_find_connector(int fd, drmModeResPtr res) {
	drmModeConnectorPtr connector;
	int i;

	connector = NULL;
	for (i = 0; i < res->count_connectors; ++i) {
		connector = drmModeGetConnector(fd, res->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			break;
		}

		drmModeFreeConnector(connector);
		connector = NULL;
	}

	return connector;
}

static uint32_t fb0_find_crtc(int fd, drmModeResPtr res, drmModeConnectorPtr connector) {
	drmModeEncoderPtr encoder;
	int i;

	encoder = drmModeGetEncoder(fd, connector->encoders[0]);
	for (i = 0; i < res->count_crtcs; ++i) {
		if (encoder->possible_crtcs & (1 << i)) {
			drmModeFreeEncoder(encoder);
			return res->crtcs[i];
		}
	}

	drmModeFreeEncoder(encoder);
	return 0;
}

static drmModeModeInfoPtr fb0_find_preferred_mode(drmModeConnectorPtr connector) {
	int i;
	drmModeModeInfoPtr mode = NULL;

	for (i = 0; i < connector->count_modes; ++i) {
		mode = &connector->modes[i];
		if (mode->type & DRM_MODE_TYPE_PREFERRED) {
			break;
		}
	}

	return mode;
}

static void fb0_handle_page_flip(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *data) {
	struct cros_gralloc_framebuffer *fb = data;
	fb->current_fb = fb->next_fb;
	fb->next_fb = 0;
}

static int fb0_init(struct cros_gralloc_framebuffer *fb) {
	drmModeResPtr res;
	drmModeConnectorPtr connector;
	drmModeModeInfoPtr mode;

	res = drmModeGetResources(fb->fd);

	connector = fb0_find_connector(fb->fd, res);
	if (!connector) {
		ALOGE("No connector found");
		drmModeFreeResources(res);
		return -ENODEV;
	}

	fb->connector_id = connector->connector_id;

	fb->crtc_id = fb0_find_crtc(fb->fd, res, connector);
	drmModeFreeResources(res);
	if (!fb->crtc_id) {
		ALOGE("No CRTC found");
		return -ENODEV;
	}

	ALOGI("Connector: %d, CRTC: %d", fb->connector_id, fb->crtc_id);

	mode = fb0_find_preferred_mode(connector);
	if (!mode) {
		ALOGE("No preferred mode found");
		drmModeFreeConnector(connector);
		return -ENODEV;
	}

	fb->mode = *mode;
	fb->current_fb = 0;
	fb->next_fb = 0;

	*(uint32_t*) &fb->device.flags = 0;
	*(uint32_t*) &fb->device.width = mode->hdisplay;
	*(uint32_t*) &fb->device.height = mode->vdisplay;
	*(int*) &fb->device.stride = mode->vdisplay;
	*(int*) &fb->device.format = HAL_PIXEL_FORMAT_RGBA_8888;
	*(float*) &fb->device.xdpi = mode->hdisplay * 25.4 / connector->mmWidth;
	*(float*) &fb->device.ydpi = mode->vdisplay * 25.4 / connector->mmHeight;
	*(float*) &fb->device.fps = mode->vrefresh;
	*(int*) &fb->device.minSwapInterval = SWAP_INTERVAL;
	*(int*) &fb->device.maxSwapInterval = SWAP_INTERVAL;

	memset(&fb->evctx, 0, sizeof(fb->evctx));
	fb->evctx.version = DRM_EVENT_CONTEXT_VERSION;
	fb->evctx.page_flip_handler = fb0_handle_page_flip;

	drmModeFreeConnector(connector);
	return 0;
}

static void fb0_await_page_flip(struct cros_gralloc_framebuffer *fb) {
	if (fb->next_fb) {
		/* There is another flip pending */
		drmHandleEvent(fb->fd, &fb->evctx);
		if (fb->next_fb) {
			ALOGE("drmHandleEvent returned without flipping");
			fb->current_fb = fb->next_fb;
			fb->next_fb = 0;
		}
	}
}

static int fb0_page_flip(struct cros_gralloc_framebuffer *fb, int fb_id) {
	int ret;

	/* Finish current page flip */
	fb0_await_page_flip(fb);

	ret = drmModePageFlip(fb->fd, fb->crtc_id, fb_id,
		DRM_MODE_PAGE_FLIP_EVENT, fb);
	if (ret) {
		ALOGE("Failed to perform page flip: %d", ret);
		if (errno != -EBUSY) {
			fb->current_fb = 0;
		}
		return errno;
	} else {
		fb->next_fb = fb_id;
	}

	return 0;
}

static int fb0_enable_crtc(struct cros_gralloc_framebuffer *fb, uint32_t fb_id) {
	int ret = drmModeSetCrtc(fb->fd, fb->crtc_id, fb_id, 0, 0,
			&fb->connector_id, 1, &fb->mode);
	if (ret) {
		ALOGE("Failed to enable CRTC: %d", ret);
	} else {
		fb->current_fb = fb_id;
	}

	return ret;
}

static int fb0_disable_crtc(struct cros_gralloc_framebuffer *fb) {
	int ret;

	/* Finish current page flip */
	fb0_await_page_flip(fb);

	ret = drmModeSetCrtc(fb->fd, fb->crtc_id, 0, 0, 0, NULL, 0, NULL);
	if (ret) {
		ALOGE("Failed to disable CRTC: %d", ret);
	} else {
		fb->current_fb = 0;
	}

	return ret;
}

static int fb0_post(struct cros_gralloc_framebuffer *fb, buffer_handle_t buffer) {
	cros_gralloc_handle_t handle  = cros_gralloc_convert_handle(buffer);
	if (!handle || !handle->fb_id) {
		return -EINVAL;
	}

	if (fb->current_fb == handle->fb_id) {
		/* Already current */
		return 0;
	}

	if (fb->current_fb) {
		return fb0_page_flip(fb, handle->fb_id);
	} else {
		return fb0_enable_crtc(fb, handle->fb_id);
	}
}

static int fb0_enable_screen(struct cros_gralloc_framebuffer *fb, int enable) {
	ALOGI("Updating screen state: %d", enable);

	/* Only need to disable screen here, will be re-enabled with next post */
	if (!enable && fb->current_fb) {
		return fb0_disable_crtc(fb);
	} else {
		return 0;
	}
}

static int fb0_composition_complete(struct framebuffer_device_t* dev) {
	return 0;
}

static int fb0_set_swap_interval(struct framebuffer_device_t* window, int interval) {
	if (interval != SWAP_INTERVAL) {
		return -EINVAL;
	}
	return 0;
}

static int fb0_close(struct cros_gralloc_framebuffer *fb) {
	return fb->gralloc->close(fb->gralloc);
}

int cros_gralloc_init_framebuffer(struct hw_device_t *gralloc, int fd, struct hw_device_t **dev) {
	struct cros_gralloc_framebuffer *fb;
	int ret;

	fb = calloc(1, sizeof(*fb));
	if (!fb) {
		return -ENOMEM;
	}

	fb->fd = fd;
	ret = fb0_init(fb);
	if (ret) {
		free(fb);
		return ret;
	}

	fb->gralloc = gralloc;
	fb->device.common.tag = HARDWARE_DEVICE_TAG;
	fb->device.common.version = 0;
	fb->device.common.close = fb0_close;

	fb->device.setSwapInterval = fb0_set_swap_interval;
	fb->device.post = fb0_post;
	fb->device.compositionComplete = fb0_composition_complete;
	fb->device.enableScreen = fb0_enable_screen;

	*dev = &fb->device.common;
	return 0;
}
