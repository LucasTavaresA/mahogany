#ifndef XWAYLAND_IMPL_H
#define XWAYLAND_IMPL_H

#include "hrt/hrt_server.h"
#include <stdbool.h>

/**
 * Start the Xwayland server, X window manager and set DISPLAY.
 *
 * XWM needs a seat, so this must be called after hrt_seat_init,
 * since it sets an environment variable it needs to be called
 * before loading the config file.
 */
bool hrt_xwayland_init(struct hrt_server *server);

void hrt_xwayland_finish(struct hrt_server *server);

#endif
