#ifndef CG_OUTPUT_H
#define CG_OUTPUT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

#include "server.h"
#include "view.h"

struct cg_output {
	struct cg_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;

	/* Scaled copies of the views shown on this output. Stays empty while this
	 * output is the primary one. */
	struct wlr_scene_tree *mirror_tree;

	struct wl_listener commit;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_listener frame;

	struct wl_list link; // cg_server::outputs
};

void handle_output_manager_apply(struct wl_listener *listener, void *data);
void handle_output_manager_test(struct wl_listener *listener, void *data);
void handle_output_layout_change(struct wl_listener *listener, void *data);
void handle_new_output(struct wl_listener *listener, void *data);
struct cg_output *output_primary(struct cg_server *server);
void output_set_window_title(struct cg_output *output, const char *title);

#endif
