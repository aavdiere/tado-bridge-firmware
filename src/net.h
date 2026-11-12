#ifndef NET_H
#define NET_H

/* Net task body (#12): owns SPI2 and the ENC28J60 (docs/design.md "Tasks").
 * Brings the chip up (retrying every 10 s), shows the link on the router LED,
 * services INT and the receive buffer, logs counters every 60 s, keeps the
 * power LED heartbeat and the watchdog check-in. lwIP (#13) attaches here.
 * Never returns. */
void net_task_run(void);

#endif
