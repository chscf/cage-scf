/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

char *
view_get_title(struct cg_view *view)
{
	const char *title = view->impl->get_title(view);
	if (!title) {
		return NULL;
	}
	return strndup(title, strlen(title));
}

bool
view_is_primary(struct cg_view *view)
{
	return view->impl->is_primary(view);
}

bool
view_is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	return child->impl->is_transient_for(child, parent);
}

void
view_activate(struct cg_view *view, bool activate)
{
	view->impl->activate(view, activate);
	wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_toplevel_handle, activate);
}

static bool
view_extends_output_layout(struct cg_view *view, struct wlr_box *layout_box)
{
	int width, height;
	view->impl->get_geometry(view, &width, &height);

	return (layout_box->height < height || layout_box->width < width);
}

static void
view_maximize(struct cg_view *view, struct wlr_box *layout_box)
{
	view->lx = layout_box->x;
	view->ly = layout_box->y;

	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	}

	view->impl->maximize(view, layout_box->width, layout_box->height);
}

static void
view_center(struct cg_view *view, struct wlr_box *layout_box)
{
	int width, height;
	view->impl->get_geometry(view, &width, &height);

	view->lx = (layout_box->width - width) / 2;
	view->ly = (layout_box->height - height) / 2;

	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	}
}

/* The box views are laid out in: the primary output's size, at the layout
 * origin. The other outputs show scaled copies, see view_mirrors_rebuild().
 * Falls back to the whole layout when there is no usable primary output. */
void
view_primary_box(struct cg_server *server, struct wlr_box *box)
{
	struct cg_output *output = output_primary(server);

	if (output) {
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, box);

		if (!wlr_box_empty(box)) {
			/* The view lives at the layout origin of the primary output. */
			box->x = 0;
			box->y = 0;
			return;
		}
	}

	wlr_output_layout_get_box(server->output_layout, NULL, box);
	wlr_log(WLR_DEBUG, "mirror: no primary output, fell back to layout box");
}

/* Removes all copies from an output's mirror tree. Copies are rebuilt from
 * scratch rather than updated, as wlr_scene has no signals for node changes. */
static void
output_mirror_clear(struct cg_output *output)
{
	struct wlr_scene_node *node, *tmp;

	if (!output->mirror_tree) {
		return;
	}

	wl_list_for_each_safe (node, tmp, &output->mirror_tree->children, link) {
		wlr_scene_node_destroy(node);
	}
}

/* Copies a scene node and its whole subtree into an output's mirror tree.
 *
 * The whole subtree is walked so that popups and subsurfaces, which have scene
 * trees of their own below the view's, are copied as well. x and y accumulate
 * the node's offset from the view in unscaled coordinates; each copy is placed
 * at that offset times scale, plus the offset that centres the image. */
static void
view_mirror_copy_node(struct wlr_scene_tree *dst, struct wlr_scene_node *node, int x, int y, double scale,
		      int off_x, int off_y)
{
	if (!node->enabled) {
		return;
	}

	x += node->x;
	y += node->y;

	switch (node->type) {
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *src = wlr_scene_buffer_from_node(node);
		struct wlr_scene_buffer *copy;
		int width, height;

		if (!src->buffer) {
			return;
		}

		copy = wlr_scene_buffer_create(dst, src->buffer);
		if (!copy) {
			return;
		}

		/* dst_width of 0 means "use the buffer's own size". */
		width = src->dst_width > 0 ? src->dst_width : src->buffer->width;
		height = src->dst_height > 0 ? src->dst_height : src->buffer->height;

		wlr_scene_buffer_set_transform(copy, src->transform);
		wlr_scene_buffer_set_dest_size(copy, (int) (width * scale), (int) (height * scale));
		wlr_scene_node_set_position(&copy->node, off_x + (int) (x * scale), off_y + (int) (y * scale));
		break;
	}
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
		struct wlr_scene_node *child;

		wl_list_for_each (child, &tree->children, link) {
			view_mirror_copy_node(dst, child, x, y, scale, off_x, off_y);
		}
		break;
	}
	default:
		break;
	}
}

static void
handle_view_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, surface_commit);
	view_mirrors_rebuild(view->server);
}

/* Rebuilds the copies of all views on every output except the primary.
 *
 * Called whenever what is shown may have changed: a surface or popup commit,
 * a popup going away, a view being mapped or unmapped, or the outputs changing. */
void
view_mirrors_rebuild(struct cg_server *server)
{
	struct cg_output *primary = output_primary(server);
	struct cg_output *output;

	wl_list_for_each (output, &server->outputs, link) {
		struct wlr_box output_box;
		struct cg_view *view;
		double scale;
		int src_width = 0, src_height = 0;
		int dst_width, dst_height;

		if (output == primary || !output->wlr_output->enabled || !output->mirror_tree) {
			continue;
		}

		output_mirror_clear(output);

		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &output_box);
		if (wlr_box_empty(&output_box)) {
			continue;
		}

		/* One scale per output, based on the primary's size, so that all copies on
		 * that output stay aligned with each other. */
		{
			struct wlr_box primary_box;

			view_primary_box(server, &primary_box);
			src_width = primary_box.width;
			src_height = primary_box.height;
		}

		if (src_width <= 0 || src_height <= 0) {
			continue;
		}

		scale = (double) output_box.width / src_width;
		if ((double) output_box.height / src_height < scale) {
			scale = (double) output_box.height / src_height;
		}

		dst_width = (int) (src_width * scale);
		dst_height = (int) (src_height * scale);

		wl_list_for_each (view, &server->views, link) {
			if (!view->scene_tree) {
				continue;
			}

			view_mirror_copy_node(output->mirror_tree, &view->scene_tree->node, 0, 0, scale,
					      (output_box.width - dst_width) / 2, (output_box.height - dst_height) / 2);
		}
	}
}

void
view_position(struct cg_view *view)
{
	struct wlr_box layout_box;
	view_primary_box(view->server, &layout_box);

	if (view_is_primary(view) || view_extends_output_layout(view, &layout_box)) {
		view_maximize(view, &layout_box);
	} else {
		view_center(view, &layout_box);
	}
}

void
view_position_all(struct cg_server *server)
{
	struct cg_view *view;
	wl_list_for_each (view, &server->views, link) {
		view_position(view);
	}
}

void
view_unmap(struct cg_view *view)
{
	wl_list_remove(&view->link);

	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->request_close.link);
	wlr_foreign_toplevel_handle_v1_destroy(view->foreign_toplevel_handle);
	view->foreign_toplevel_handle = NULL;

	wl_list_remove(&view->surface_commit.link);

	wlr_scene_node_destroy(&view->scene_tree->node);
	view->scene_tree = NULL;
	view_mirrors_rebuild(view->server);

	view->wlr_surface->data = NULL;
	view->wlr_surface = NULL;
}

void
handle_surface_request_activate(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, request_activate);

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	seat_set_focus(view->server->seat, view);
}

void
handle_surface_request_close(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, request_close);
	view->impl->close(view);
}

void
view_map(struct cg_view *view, struct wlr_surface *surface)
{
	view->scene_tree = wlr_scene_subsurface_tree_create(&view->server->scene->tree, surface);
	if (!view->scene_tree)
		goto fail;
	view->scene_tree->node.data = view;

	view->wlr_surface = surface;
	surface->data = view;

	view->surface_commit.notify = handle_view_surface_commit;
	wl_signal_add(&surface->events.commit, &view->surface_commit);

#if CAGE_HAS_XWAYLAND
	/* We shouldn't position override-redirect windows. They set
	   their own (x,y) coordinates in handle_wayland_surface_map. */
	if (view->type != CAGE_XWAYLAND_VIEW || xwayland_view_should_manage(view))
#endif
	{
		view_position(view);
	}

	wl_list_insert(&view->server->views, &view->link);

	view_mirrors_rebuild(view->server);

	view->foreign_toplevel_handle = wlr_foreign_toplevel_handle_v1_create(view->server->foreign_toplevel_manager);
	if (!view->foreign_toplevel_handle)
		goto fail;

	view->request_activate.notify = handle_surface_request_activate;
	wl_signal_add(&view->foreign_toplevel_handle->events.request_activate, &view->request_activate);
	view->request_close.notify = handle_surface_request_close;
	wl_signal_add(&view->foreign_toplevel_handle->events.request_close, &view->request_close);

	seat_set_focus(view->server->seat, view);
	return;

fail:
	wl_resource_post_no_memory(surface->resource);
}

void
view_destroy(struct cg_view *view)
{
	struct cg_server *server = view->server;

	if (view->wlr_surface != NULL) {
		view_unmap(view);
	}

	view->impl->destroy(view);

	/* If there is a previous view in the list, focus that. */
	bool empty = wl_list_empty(&server->views);
	if (!empty) {
		struct cg_view *prev = wl_container_of(server->views.next, prev, link);
		seat_set_focus(server->seat, prev);
	}
}

void
view_init(struct cg_view *view, struct cg_server *server, enum cg_view_type type, const struct cg_view_impl *impl)
{
	view->server = server;
	view->type = type;
	view->impl = impl;
}

struct cg_view *
view_from_wlr_surface(struct wlr_surface *surface)
{
	assert(surface);
	return surface->data;
}
