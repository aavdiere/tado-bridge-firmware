#ifndef SESSION_H
#define SESSION_H

#include "lwip/ip_addr.h"

#include <stdbool.h>
#include <stdint.h>

/* Server session (#15): WebSocket to tanoclo on /hw/v2, then the bridge's
 * own requests, POST auth/key, POST auth/token and GET time
 * (docs/server-api.md "Bridge sends"), with reconnect and backoff. Drives
 * the cloud LED: off without a connection, 2 Hz while connecting and
 * authenticating, on once authenticated with the time set. Once ready it also relays between
 * the server and the radio task's queues (src/link.h): frames for a device
 * address go to the radio, uplinks from devices to the server. Net task only. */

/* Call on every net-task pass. net_up: link, DHCP address and the server's
 * address are all there. *busy: call again without waiting. */
void session_poll(bool net_up, const ip_addr_t *server, bool *busy);

/* One log line with the relay counters of the last minute (#17). */
void session_log_minute(void);

/* The session is ready: authenticated, time set (for the update's
 * confirmation, #24). Any task; a snapshot. */
bool session_ready(void);

/* Unix time from the server, if it has sent one since boot. */
bool session_unix_time(uint32_t *now);

#endif
