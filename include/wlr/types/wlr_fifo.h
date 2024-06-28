/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FIFO_H
#define WLR_TYPES_WLR_FIFO_H

#include <wayland-server-core.h>

#include <wlr/types/wlr_compositor.h>

struct wlr_fifo_manager {
	struct wl_global *global;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal fifo_create;
		struct wl_signal destroy;
	} events;
};

struct fifo_commit {
	struct wl_list link; // wlr_fifo.fifo_commits
	bool barrier_pending;
	uint32_t pending_seq; // locked surface commit sequence
};

struct wlr_fifo {
	struct wl_resource *resource;
	struct wlr_addon fifo_addon;
	struct wlr_surface *surface;

	struct wl_listener surface_client_commit;
	struct wl_listener surface_commit;
	struct wl_listener fifo_manager_destroy;

	struct {
		struct wl_signal fifo_barrier;
	} events;

	/*
	 * .fifo request for this commit.
	 * used to lock a commit after a previous one with a .fifo_barrier has been committed.
	 * if there is no .fifo_barrier committed, this request is a no-op.
	 *
	 * set when client requests a .fifo.
	 * reset after each .commit request, in the client_commit event. */
	bool fifo;

	/*
	 * .fifo_barrier request for this commit.
	 * used to send the fifo_barrier signal to compositors.
	 * it sets barrier_committed when the commit is applied.
	 *
	 * set when client requests a .fifo_barrier.
	 * reset after each .commit request, either in client_commit or commit events, depending on the
	 * state of barrier_committed. */
	bool fifo_barrier;
	uint64_t barrier_commit_seq; // surface commit sequence for .fifo_barrier request

	/*
	 * used to process further .fifo and .fifo_barrier requests after a barrier has been set and
	 * committed; it conditions the meaning of .fifo and .fifo_barrier requests.
	 *
	 * set when a .fifo_barrier request has been committed.
	 * reset when compositor calls wlr_fifo_signal_barrier(). */
	bool barrier_committed;

	struct wl_list commits; // fifo_commit.link
};

/**
 * Used by compositors to clear the fifo barrier for the wlr_fifo.
 */
void wlr_fifo_signal_barrier(struct wlr_fifo *fifo);

/**
 * Create the wp_fifo_manager_v1_interface global, which can be used by clients to
 * queue commits on a wl_surface for presentation.
 */
struct wlr_fifo_manager *wlr_fifo_manager_create(struct wl_display *display,
	uint32_t version);

#endif
