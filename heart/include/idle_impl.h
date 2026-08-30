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

#endif
