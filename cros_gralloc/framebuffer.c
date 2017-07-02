#define LOG_TAG "fb0"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/gralloc.h>
#include <GLES/gl.h>
#include <cutils/log.h>

#include "framebuffer.h"
#include "cros_gralloc_handle.h"

#define SWAP_INTERVAL 1

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
	fb->next_fb = -1;
}

static int fb0_init(struct cros_gralloc_framebuffer *fb) {
	drmModeResPtr res;
	drmModeConnectorPtr connector;
	drmModeModeInfoPtr mode;

	res = drmModeGetResources(fb->fd);

	connector = fb0_find_connector(fb->fd, res);
	if (!connector) {
		ALOGE("No connector found\n");
		drmModeFreeResources(res);
		return -ENODEV;
	}

	fb->connector_id = connector->connector_id;

	fb->crtc_id = fb0_find_crtc(fb->fd, res, connector);
	drmModeFreeResources(res);
	if (!fb->crtc_id) {
		ALOGE("No CRTC found\n");
		return -ENODEV;
	}

	ALOGI("Connector: %d, CRTC: %d\n", fb->connector_id, fb->crtc_id);

	mode = fb0_find_preferred_mode(connector);
	if (!mode) {
		ALOGE("No preferred mode found\n");
		drmModeFreeConnector(connector);
		return -ENODEV;
	}

	fb->mode = *mode;
	fb->current_fb = -1;
	fb->next_fb = -1;

	*(uint32_t*) &fb->device.flags = 0;
	*(uint32_t*) &fb->device.width = mode->hdisplay;
	*(uint32_t*) &fb->device.height = mode->vdisplay;
	*(int*) &fb->device.stride = mode->vdisplay;
	*(int*) &fb->device.format = HAL_PIXEL_FORMAT_BGRA_8888;
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
	// There is another flip pending
	if (fb->next_fb > 0) {
		drmHandleEvent(fb->fd, &fb->evctx);
		if (fb->next_fb > 0) {
			ALOGE("drmHandleEvent returned without flipping");
			fb->current_fb = fb->next_fb;
			fb->next_fb = -1;
		}
	}
}

static int fb0_page_flip(struct cros_gralloc_framebuffer *fb, int fb_id) {
	int ret;
	fb0_await_page_flip(fb);
	ret = drmModePageFlip(fb->fd, fb->crtc_id, fb_id,
		DRM_MODE_PAGE_FLIP_EVENT, fb);
	if (ret) {
		ALOGE("Failed to perform page flip: %s\n", strerror(errno));
		if (errno != EBUSY) {
			fb->current_fb = -1;
		}
		return errno;
	} else {
		fb->next_fb = fb_id;
	}

	return 0;
}


static int fb0_post(struct cros_gralloc_framebuffer *fb, buffer_handle_t buffer) {
	int ret;
	cros_gralloc_handle_t handle = cros_gralloc_convert_handle(buffer);
	if (!handle || handle->fb_id < 0) {
		return -EINVAL;
	}

	if (fb->current_fb < 0) {
		// First post
		ret = drmModeSetCrtc(fb->fd, fb->crtc_id, handle->fb_id, 0, 0,
				&fb->connector_id, 1, &fb->mode);
		if (ret) {
			ALOGE("Failed to set CRTC: %d\n", ret);
		} else {
			fb->current_fb = handle->fb_id;
		}

		return ret;
	}

	if (fb->current_fb == handle->fb_id) {
		return 0; // Already current
	}

	return fb0_page_flip(fb, handle->fb_id);
}

static int fb0_composition_complete(struct framebuffer_device_t* dev) {
	glFlush();
	return 0;
}

static int fb0_set_swap_interval(struct framebuffer_device_t* window, int interval) {
	if (interval != SWAP_INTERVAL) {
		return -EINVAL;
	}
	return 0;
}

int cros_gralloc_open_framebuffer(struct cros_gralloc_framebuffer **fb, int fd,
	const struct hw_module_t *mod, struct hw_device_t *gralloc_dev, struct hw_device_t **dev) {

	int ret;

	if (*fb) {
		// Already initialized
		ALOGI("Returning existing framebuffer device\n");
		*dev = &(*fb)->device.common;
		return 0;
	}

	*fb = calloc(1, sizeof(**fb));

	(*fb)->fd = fd;
	ret = fb0_init(*fb);
	if (ret)
		return ret;

	(*fb)->device.common.tag = HARDWARE_DEVICE_TAG;
	(*fb)->device.common.version = 0;
	(*fb)->device.common.module = mod;
	(*fb)->device.common.close = gralloc_dev->close;

	(*fb)->device.setSwapInterval = fb0_set_swap_interval;
	(*fb)->device.post = fb0_post;
	(*fb)->device.compositionComplete = fb0_composition_complete;

	*dev = &(*fb)->device.common;
	ALOGI("Opened framebuffer device\n");
	return 0;
}
