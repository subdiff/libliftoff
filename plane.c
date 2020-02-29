#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "private.h"

static int guess_plane_zpos_from_type(struct liftoff_device *device,
				      uint32_t plane_id, uint32_t type)
{
	struct liftoff_plane *primary;

	/* From far to close to the eye: primary, overlay, cursor. Unless
	 * the overlay ID < primary ID. */
	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		return 0;
	case DRM_PLANE_TYPE_CURSOR:
		return 2;
	case DRM_PLANE_TYPE_OVERLAY:
		if (liftoff_list_empty(&device->planes)) {
			return 0; /* No primary plane, shouldn't happen */
		}
		primary = liftoff_container_of(device->planes.next,
					       primary, link);
		if (plane_id < primary->id) {
			return -1;
		} else {
			return 1;
		}
	}
	return 0;
}

static void log_plane(struct liftoff_device *device,
					  struct liftoff_plane *plane)
{
	size_t i;
	int per_line = 0;

	if (!log_has(LIFTOFF_DEBUG)) {
		return;
	}

	debug_cnt(device, "     type: ");
	if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
		debug_cnt(device, "Primary");
	} else if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		debug_cnt(device, "Cursor");
	} else {
		debug_cnt(device, "Overlay");
	}
	debug_end(device, 0);

	liftoff_log(LIFTOFF_DEBUG, "     zpos: %"PRIu32, plane->zpos);

	debug_cnt(device, "    props:");
	for (i = 0; i < plane->props_len; i++) {
		if (per_line == 5) {
			debug_end(device, 0);
			debug_cnt(device, "          ");
			per_line = 0;
		}
		debug_cnt(device, " %s", plane->props[i].name);
		per_line++;
	}
	debug_end(device, 0);
}

struct liftoff_plane *plane_create(struct liftoff_device *device, uint32_t id)
{
	struct liftoff_plane *plane, *cur;
	drmModePlane *drm_plane;
	drmModeObjectProperties *drm_props;
	uint32_t i;
	drmModePropertyRes *drm_prop;
	struct liftoff_plane_property *prop;
	uint64_t value;
	bool has_type = false, has_zpos = false;

	debug_cnt(device, "Plane %"PRIu32, id);
	debug_end(device, 0);

	plane = calloc(1, sizeof(*plane));
	if (plane == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		return NULL;
	}

	drm_plane = drmModeGetPlane(device->drm_fd, id);
	if (drm_plane == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetPlane");
		return NULL;
	}
	plane->id = drm_plane->plane_id;
	plane->possible_crtcs = drm_plane->possible_crtcs;
	drmModeFreePlane(drm_plane);

	drm_props = drmModeObjectGetProperties(device->drm_fd, id,
					       DRM_MODE_OBJECT_PLANE);
	if (drm_props == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeObjectGetProperties");
		return NULL;
	}
	plane->props = calloc(drm_props->count_props,
			      sizeof(struct liftoff_plane_property));
	if (plane->props == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		drmModeFreeObjectProperties(drm_props);
		return NULL;
	}
	for (i = 0; i < drm_props->count_props; i++) {
		drm_prop = drmModeGetProperty(device->drm_fd,
					      drm_props->props[i]);
		if (drm_prop == NULL) {
			liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetProperty");
			drmModeFreeObjectProperties(drm_props);
			return NULL;
		}
		prop = &plane->props[i];
		memcpy(prop->name, drm_prop->name, sizeof(prop->name));
		prop->id = drm_prop->prop_id;
		drmModeFreeProperty(drm_prop);
		plane->props_len++;

		value = drm_props->prop_values[i];
		if (strcmp(prop->name, "type") == 0) {
			plane->type = value;
			has_type = true;
		} else if (strcmp(prop->name, "zpos") == 0) {
			plane->zpos = value;
			has_zpos = true;
		}
	}
	drmModeFreeObjectProperties(drm_props);

	if (!has_type) {
		liftoff_log(LIFTOFF_ERROR,
					"plane %"PRIu32" is missing the 'type' property",
					plane->id);
		free(plane);
		return NULL;
	} else if (!has_zpos) {
		plane->zpos = guess_plane_zpos_from_type(device, plane->id,
							 plane->type);
	}

	/* During plane allocation, we will use the plane list order to fill
	 * planes with FBs. Primary planes need to be filled first, then planes
	 * far from the primary planes, then planes closer and closer to the
	 * primary plane. */
	if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
		liftoff_list_for_each(cur, &device->planes, link) {
			if (cur->type != DRM_PLANE_TYPE_PRIMARY) {
				liftoff_list_insert(cur->link.prev, &plane->link);
				break;
			}
		}
		/* Not inserted, i.e. there are only primary planes in the list,
		 * so just append. */
		if (plane->link.next == NULL) {
			liftoff_list_insert(device->planes.prev, &plane->link);
		}
	} else {
		liftoff_list_for_each(cur, &device->planes, link) {
			if (cur->type == DRM_PLANE_TYPE_PRIMARY ||
				plane->zpos <= cur->zpos) {
				continue;
			}
			liftoff_list_insert(cur->link.prev, &plane->link);
			break;
		}
		/* Not inserted, i.e. only primary planes in the list or closer/similar
		 * far away planes. */
		if (plane->link.next == NULL) {
			liftoff_list_insert(device->planes.prev, &plane->link);
		}
	}

	log_plane(device, plane);

	return plane;
}

void plane_destroy(struct liftoff_plane *plane)
{
	if (plane->layer != NULL) {
		plane->layer->plane = NULL;
	}
	liftoff_list_remove(&plane->link);
	free(plane->props);
	free(plane);
}

struct liftoff_plane_property *plane_get_property(struct liftoff_plane *plane,
						  const char *name)
{
	size_t i;

	for (i = 0; i < plane->props_len; i++) {
		if (strcmp(plane->props[i].name, name) == 0) {
			return &plane->props[i];
		}
	}
	return NULL;
}

static bool plane_set_prop(struct liftoff_plane *plane, drmModeAtomicReq *req,
			   struct liftoff_plane_property *prop, uint64_t value)
{
	int ret;

	ret = drmModeAtomicAddProperty(req, plane->id, prop->id, value);
	if (ret < 0) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeAtomicAddProperty");
		return false;
	}

	return true;
}

static bool set_plane_prop_str(struct liftoff_plane *plane,
			       drmModeAtomicReq *req, const char *name,
			       uint64_t value)
{
	struct liftoff_plane_property *prop;

	prop = plane_get_property(plane, name);
	if (prop == NULL) {
		liftoff_log(LIFTOFF_DEBUG,
			    "plane %"PRIu32" is missing the %s property",
			    plane->id, name);
		return false;
	}

	return plane_set_prop(plane, req, prop, value);
}

bool plane_apply(struct liftoff_plane *plane, struct liftoff_layer *layer,
		 drmModeAtomicReq *req, bool *compatible)
{
	int cursor;
	size_t i;
	struct liftoff_layer_property *layer_prop;
	struct liftoff_plane_property *plane_prop;

	*compatible = true;
	cursor = drmModeAtomicGetCursor(req);

	if (layer == NULL) {
		return set_plane_prop_str(plane, req, "FB_ID", 0) &&
		       set_plane_prop_str(plane, req, "CRTC_ID", 0);
	}

	if (!set_plane_prop_str(plane, req, "CRTC_ID", layer->output->crtc_id)) {
		return false;
	}

	for (i = 0; i < layer->props_len; i++) {
		layer_prop = &layer->props[i];
		if (strcmp(layer_prop->name, "zpos") == 0) {
			/* We don't yet support setting the zpos property. We
			 * only use it (read-only) during plane allocation. */
			continue;
		}

		plane_prop = plane_get_property(plane, layer_prop->name);
		if (plane_prop == NULL) {
			*compatible = false;
			drmModeAtomicSetCursor(req, cursor);
			return true;
		}

		if (!plane_set_prop(plane, req, plane_prop, layer_prop->value)) {
			drmModeAtomicSetCursor(req, cursor);
			return false;
		}
	}

	return true;
}
