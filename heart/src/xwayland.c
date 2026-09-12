#include "xwayland_impl.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>

#include "hrt/hrt_scene.h"
#include "hrt/hrt_view.h"
#include "view_impl.h"

// The compositor side of one X11 window that mahogany manages, it becomes an
// its a tiled hrt_view. hrt_view first so lisp sees the pointer as a hrt_view.
struct hrt_xwayland_view {
    struct hrt_view view;
    struct hrt_server *server;

    // The wlr_surface only exists between associate and dissociate, so the
    // map/unmap listeners can only be attached for that window.
    bool associated;
    // Whether hrt_view_init has run and view.scene_tree exists.
    // Separate from `announced` because a window can associate, dissociate and
    // associate again, and the tree is built exactly once.
    bool initialized;
    // Whether new_view has been announced. A window can be created and
    // destroyed without ever associating, and the Lisp side must not be told
    // to destroy a view it was never told about.
    bool announced;

    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener request_configure;
};

// An override-redirect window: menus, tooltips, dnd icons.
// we tell the window manager to keep its hands off these,
// they are not views or tiled and they are placed where the client asks.
struct hrt_xwayland_unmanaged {
    struct wlr_xwayland_surface *xsurface;
    struct hrt_server *server;
    struct wlr_scene_tree *tree;

    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
};

struct hrt_xwayland {
    struct wlr_xwayland *wlr_xwayland;
    struct hrt_server *server;

    struct wl_listener ready;
    struct wl_listener new_surface;
    struct wl_listener destroy;
};

static struct hrt_xwayland *server_xwayland = NULL;

static void handle_view_map(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_view *xview = wl_container_of(listener, xview,
                                                      view.map);
    xview->view.callbacks->view_mapped(&xview->view);
}

static void handle_view_unmap(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_view *xview = wl_container_of(listener, xview,
                                                      view.unmap);
    xview->view.callbacks->view_unmapped(&xview->view);
}

static void handle_view_request_maximize(struct wl_listener *listener,
                                         void *data) {
    struct hrt_xwayland_view *xview =
        wl_container_of(listener, xview, view.request_maximize);
    xview->view.callbacks->request_maximize(&xview->view);
}

static void handle_view_request_minimize(struct wl_listener *listener,
                                         void *data) {
    struct hrt_xwayland_view *xview =
        wl_container_of(listener, xview, view.request_minimize);
    xview->view.callbacks->request_minimize(&xview->view);
}

static void handle_view_request_fullscreen(struct wl_listener *listener,
                                           void *data) {
    struct hrt_xwayland_view *xview =
        wl_container_of(listener, xview, view.request_fullscreen);
    struct wlr_xwayland_surface *xsurface = xview->view.xwayland_surface;

    xview->view.callbacks->request_fullscreen(&xview->view, NULL,
                                              !xsurface->fullscreen);
}

static void handle_view_request_configure(struct wl_listener *listener,
                                          void *data) {
    struct wlr_xwayland_surface_configure_event *event = data;

    struct wlr_xwayland_surface *xsurface = event->surface;
    wlr_xwayland_surface_configure(xsurface, xsurface->x, xsurface->y,
                                   xsurface->width, xsurface->height);
}

static void handle_view_associate(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_view *xview = wl_container_of(listener, xview,
                                                      associate);
    struct wlr_surface *surface = xview->view.xwayland_surface->surface;

    xview->view.map.notify = handle_view_map;
    wl_signal_add(&surface->events.map, &xview->view.map);
    xview->view.unmap.notify = handle_view_unmap;
    wl_signal_add(&surface->events.unmap, &xview->view.unmap);
    xview->associated = true;

    // An xview can be associated, dissociated and associated again, so the tree
    // needs to be built only once.
    if (!xview->initialized) {
        hrt_view_init(&xview->view, xview->server->scene_root->normal);
        xview->initialized = true;
    } else {
        struct wlr_scene_tree *surface_tree =
            wlr_scene_subsurface_tree_create(xview->view.scene_tree, surface);
        if (surface_tree) {
            surface_tree->node.data = &xview->view;
            xview->view.xdg_scene   = surface_tree;
        }
    }

    if (!xview->announced) {
        xview->announced = true;
        xview->view.callbacks->new_view(&xview->view);
    }
}

static void handle_view_dissociate(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_view *xview = wl_container_of(listener, xview,
                                                      dissociate);
    if (!xview->associated) {
        return;
    }
    wl_list_remove(&xview->view.map.link);
    wl_list_remove(&xview->view.unmap.link);
    xview->associated = false;
}

static void handle_view_destroy(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_view *xview = wl_container_of(listener, xview,
                                                      view.destroy);
    wlr_log(WLR_DEBUG, "Xwayland surface destroyed");

    if (xview->announced) {
        xview->view.callbacks->view_destroyed(&xview->view);
    }
    if (xview->associated) {
        wl_list_remove(&xview->view.map.link);
        wl_list_remove(&xview->view.unmap.link);
    }

    wl_list_remove(&xview->associate.link);
    wl_list_remove(&xview->dissociate.link);
    wl_list_remove(&xview->view.destroy.link);
    wl_list_remove(&xview->request_configure.link);
    wl_list_remove(&xview->view.request_fullscreen.link);
    wl_list_remove(&xview->view.request_maximize.link);
    wl_list_remove(&xview->view.request_minimize.link);

    if (xview->initialized) {
        hrt_view_cleanup(&xview->view);
    }
    free(xview);
}

static void create_view(struct hrt_server *server,
                        struct wlr_xwayland_surface *xsurface) {
    struct hrt_xwayland_view *xview =
        calloc(1, sizeof(struct hrt_xwayland_view));
    if (!xview) {
        wlr_log(WLR_ERROR,
                "Failed to allocate hrt_xwayland_view object for surface %p",
                xsurface);
        return;
    }

    xview->server                 = server;
    xview->view.type              = HRT_VIEW_XWAYLAND;
    xview->view.xwayland_surface  = xsurface;
    xview->view.callbacks         = server->view_callbacks;
    xsurface->data                = &xview->view;

    xview->associate.notify = handle_view_associate;
    wl_signal_add(&xsurface->events.associate, &xview->associate);
    xview->dissociate.notify = handle_view_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &xview->dissociate);
    xview->view.destroy.notify = handle_view_destroy;
    wl_signal_add(&xsurface->events.destroy, &xview->view.destroy);

    xview->request_configure.notify = handle_view_request_configure;
    wl_signal_add(&xsurface->events.request_configure,
                  &xview->request_configure);

    xview->view.request_maximize.notify = handle_view_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize,
                  &xview->view.request_maximize);
    xview->view.request_minimize.notify = handle_view_request_minimize;
    wl_signal_add(&xsurface->events.request_minimize,
                  &xview->view.request_minimize);
    xview->view.request_fullscreen.notify = handle_view_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen,
                  &xview->view.request_fullscreen);
}

static void handle_unmanaged_map(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_unmanaged *unmanaged = wl_container_of(listener,
                                                               unmanaged, map);
    struct wlr_xwayland_surface *xsurface = unmanaged->xsurface;

    // Above the tiled windows, below the layer-shell overlay.
    unmanaged->tree = wlr_scene_subsurface_tree_create(
        unmanaged->server->scene_root->top, xsurface->surface);
    if (!unmanaged->tree) {
        wlr_log(WLR_ERROR, "Could not create a scene tree for an unmanaged "
                           "xwayland surface");
        return;
    }
    wlr_scene_node_set_position(&unmanaged->tree->node, xsurface->x,
                                xsurface->y);
}

static void handle_unmanaged_unmap(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, unmap);
    if (unmanaged->tree) {
        wlr_scene_node_destroy(&unmanaged->tree->node);
        unmanaged->tree = NULL;
    }
}

static void handle_unmanaged_set_geometry(struct wl_listener *listener,
                                          void *data) {
    struct hrt_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, set_geometry);
    if (unmanaged->tree) {
        wlr_scene_node_set_position(&unmanaged->tree->node,
                                    unmanaged->xsurface->x,
                                    unmanaged->xsurface->y);
    }
}

static void handle_unmanaged_associate(struct wl_listener *listener,
                                       void *data) {
    struct hrt_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, associate);
    struct wlr_surface *surface = unmanaged->xsurface->surface;

    unmanaged->map.notify = handle_unmanaged_map;
    wl_signal_add(&surface->events.map, &unmanaged->map);
    unmanaged->unmap.notify = handle_unmanaged_unmap;
    wl_signal_add(&surface->events.unmap, &unmanaged->unmap);
}

static void handle_unmanaged_dissociate(struct wl_listener *listener,
                                        void *data) {
    struct hrt_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, dissociate);
    wl_list_remove(&unmanaged->map.link);
    wl_list_remove(&unmanaged->unmap.link);
    wl_list_init(&unmanaged->map.link);
    wl_list_init(&unmanaged->unmap.link);
}

static void handle_unmanaged_destroy(struct wl_listener *listener, void *data) {
    struct hrt_xwayland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, destroy);

    if (unmanaged->tree) {
        wlr_scene_node_destroy(&unmanaged->tree->node);
    }
    wl_list_remove(&unmanaged->map.link);
    wl_list_remove(&unmanaged->unmap.link);
    wl_list_remove(&unmanaged->associate.link);
    wl_list_remove(&unmanaged->dissociate.link);
    wl_list_remove(&unmanaged->set_geometry.link);
    wl_list_remove(&unmanaged->destroy.link);
    free(unmanaged);
}

static void create_unmanaged(struct hrt_server *server,
                             struct wlr_xwayland_surface *xsurface) {
    struct hrt_xwayland_unmanaged *unmanaged =
        calloc(1, sizeof(struct hrt_xwayland_unmanaged));
    if (!unmanaged) {
        wlr_log(WLR_ERROR,
                "Failed to allocate hrt_xwayland_unmanaged object for "
                "surface %p",
                xsurface);
        return;
    }

    unmanaged->xsurface = xsurface;
    unmanaged->server   = server;

    // Not associated yet, but dissociate and destroy both remove these.
    wl_list_init(&unmanaged->map.link);
    wl_list_init(&unmanaged->unmap.link);

    unmanaged->associate.notify = handle_unmanaged_associate;
    wl_signal_add(&xsurface->events.associate, &unmanaged->associate);
    unmanaged->dissociate.notify = handle_unmanaged_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &unmanaged->dissociate);
    unmanaged->set_geometry.notify = handle_unmanaged_set_geometry;
    wl_signal_add(&xsurface->events.set_geometry, &unmanaged->set_geometry);
    unmanaged->destroy.notify = handle_unmanaged_destroy;
    wl_signal_add(&xsurface->events.destroy, &unmanaged->destroy);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
    struct hrt_xwayland *xwayland = wl_container_of(listener, xwayland,
                                                    new_surface);
    struct wlr_xwayland_surface *xsurface = data;

    wlr_log(WLR_DEBUG, "New xwayland surface: class=%s override_redirect=%d",
            xsurface->class ? xsurface->class : "(null)",
            xsurface->override_redirect);

    // A window can go from managed to override-redirect and back again, that goes unhandled,
    // we handle it once from here.
    if (xsurface->override_redirect) {
        create_unmanaged(xwayland->server, xsurface);
    } else {
        create_view(xwayland->server, xsurface);
    }
}

static void handle_ready(struct wl_listener *listener, void *data) {
    struct hrt_xwayland *xwayland = wl_container_of(listener, xwayland, ready);
    wlr_log(WLR_INFO, "Xwayland is ready on DISPLAY=%s",
            xwayland->wlr_xwayland->display_name);

    // Reapplies the seat on every start, not just the first. Xwayland is lazy,
    // so it exits when the last X client does and a fresh xwm is built for the
    // next one. wlroots only carries the seat across that if xwayland->seat is
    // still set, and a session that loses it goes quiet.
    // This is idempotent xwm_set_seat drops the previous listeners first.
    wlr_xwayland_set_seat(xwayland->wlr_xwayland, xwayland->server->seat.seat);
}

static void handle_xwayland_destroy(struct wl_listener *listener, void *data) {
    struct hrt_xwayland *xwayland = wl_container_of(listener, xwayland,
                                                    destroy);
    wl_list_remove(&xwayland->ready.link);
    wl_list_remove(&xwayland->new_surface.link);
    wl_list_remove(&xwayland->destroy.link);
    if (server_xwayland == xwayland) {
        server_xwayland = NULL;
    }
    free(xwayland);
}

void hrt_xwayland_finish(struct hrt_server *server) {
    if (server_xwayland == NULL) {
        return;
    }
    wlr_xwayland_destroy(server_xwayland->wlr_xwayland);
}

bool hrt_xwayland_init(struct hrt_server *server) {
    struct hrt_xwayland *xwayland = calloc(1, sizeof(struct hrt_xwayland));
    if (!xwayland) {
        wlr_log(WLR_ERROR, "Failed to allocate hrt_xwayland object");
        return false;
    }
    xwayland->server = server;

    // Xwayland starts lazily, theres only a socket and a display name,
    // Xwayland starts when the first X11 client connects.
    xwayland->wlr_xwayland =
        wlr_xwayland_create(server->wl_display, server->compositor, true);
    if (!xwayland->wlr_xwayland) {
        wlr_log(WLR_ERROR, "Could not start the Xwayland server");
        free(xwayland);
        return false;
    }

    // wlroots re-applies this to each new xwm in xwayland_mark_ready, but only
    // `if (xwayland->seat)`.
    if (server->seat.seat == NULL) {
        wlr_log(WLR_ERROR, "Xwayland: no seat at init, X11 clipboard disabled");
    }
    wlr_xwayland_set_seat(xwayland->wlr_xwayland, server->seat.seat);

    xwayland->ready.notify = handle_ready;
    wl_signal_add(&xwayland->wlr_xwayland->events.ready, &xwayland->ready);
    xwayland->new_surface.notify = handle_new_surface;
    wl_signal_add(&xwayland->wlr_xwayland->events.new_surface,
                  &xwayland->new_surface);
    xwayland->destroy.notify = handle_xwayland_destroy;
    wl_signal_add(&xwayland->wlr_xwayland->events.destroy, &xwayland->destroy);

    setenv("DISPLAY", xwayland->wlr_xwayland->display_name, true);
    wlr_log(WLR_INFO, "Xwayland listening on DISPLAY=%s",
            xwayland->wlr_xwayland->display_name);

    server_xwayland = xwayland;
    return true;
}
