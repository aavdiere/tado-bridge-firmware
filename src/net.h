#ifndef NET_H
#define NET_H

/* Net task body (#12, #13): owns SPI2, the ENC28J60 and lwIP (docs/design.md
 * "Tasks"). Brings the chip up (retrying every 10 s), runs lwIP with DHCP and
 * resolves the server host (NET_SERVER_HOST) once per lease, shows link and
 * address on the router LED (off: no link, blinking: link without address,
 * on: DHCP lease), logs counters every 60 s, keeps the power LED heartbeat
 * and the watchdog check-in. Never returns. */
void net_task_run(void);

#endif
