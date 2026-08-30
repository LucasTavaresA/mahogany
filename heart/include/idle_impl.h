#ifndef IDLE_IMPL_H
#define IDLE_IMPL_H

#include <stdbool.h>

struct hrt_seat;
struct hrt_server;

bool hrt_idle_init(struct hrt_server *server);

/**
 * Report user activity on a seat, restarting the timeouts a client is
 * waiting on.
 *
 * this has to be called from every input handler that should count as the user activity.
 */
void hrt_idle_notify_activity(struct hrt_seat *seat);

/**
 * Updates the status of the inhibit notifier by working out if any
 * clients are asking for inhibit, that only counts for a surface is
 * on the screen, so it must be called by both things that change what is on
 * the screen and from protocol handlers.
 */
void hrt_idle_inhibit_update(void);

#endif
