#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "private.h"

struct liftoff_device *liftoff_device_create(int drm_fd)
{
	struct liftoff_device *device;
	drmModeRes *drm_res;
	drmModePlaneRes *drm_plane_res;
	uint32_t i;

	liftoff_log_cnt(LIFTOFF_DEBUG, "\nCreating device for fd %d. ", drm_fd);

	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		return NULL;
	}

	liftoff_list_init(&device->planes);
	liftoff_list_init(&device->outputs);

	device->drm_fd = dup(drm_fd);
	if (device->drm_fd < 0) {
		liftoff_log_errno(LIFTOFF_ERROR, "dup");
		liftoff_device_destroy(device);
		return NULL;
	}

	drm_res = drmModeGetResources(drm_fd);
	if (drm_res == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetResources");
		liftoff_device_destroy(device);
		return NULL;
	}

	device->crtcs = malloc(drm_res->count_crtcs * sizeof(uint32_t));
	if (device->crtcs == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "malloc");
		drmModeFreeResources(drm_res);
		liftoff_device_destroy(device);
		return NULL;
	}
	device->crtcs_len = drm_res->count_crtcs;
	memcpy(device->crtcs, drm_res->crtcs,
	       drm_res->count_crtcs * sizeof(uint32_t));

	drmModeFreeResources(drm_res);

	/* TODO: allow users to choose which layers to hand over */
	drm_plane_res = drmModeGetPlaneResources(drm_fd);
	if (drm_plane_res == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetPlaneResources");
		liftoff_device_destroy(device);
		return NULL;
	}

	liftoff_log(LIFTOFF_DEBUG, "The device has %"PRIu32 " planes:", drm_plane_res->count_planes);

	for (i = 0; i < drm_plane_res->count_planes; i++) {
		if (i < 9 && drm_plane_res->count_planes >= 9) {
			liftoff_log_cnt(LIFTOFF_DEBUG, " ");
		}
		liftoff_log_cnt(LIFTOFF_DEBUG, "[%d] ", i + 1);
		if (plane_create(device, drm_plane_res->planes[i]) == NULL) {
			liftoff_device_destroy(device);
			return NULL;
		}
	}
	drmModeFreePlaneResources(drm_plane_res);

	liftoff_log_cnt(LIFTOFF_DEBUG, "\n");

	return device;
}

void liftoff_device_destroy(struct liftoff_device *device)
{
	struct liftoff_plane *plane, *tmp;

	if (device == NULL) {
		return;
	}

	close(device->drm_fd);
	liftoff_list_for_each_safe(plane, tmp, &device->planes, link) {
		plane_destroy(plane);
	}
	free(device->crtcs);
	free(device);
}

bool device_test_commit(struct liftoff_device *device,
			drmModeAtomicReq *req, bool *compatible)
{
	int ret;

	do {
		ret = drmModeAtomicCommit(device->drm_fd, req,
					  DRM_MODE_ATOMIC_TEST_ONLY, NULL);
	} while (-ret == EINTR || -ret == EAGAIN);
	if (ret == 0) {
		*compatible = true;
	} else if (-ret == EINVAL || -ret == ERANGE) {
		liftoff_log(LIFTOFF_DEBUG, "Atomic test commit failed: %d", ret);
		*compatible = false;
	} else {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeAtomicCommit");
		*compatible = false;
		return false;
	}

	return true;
}
