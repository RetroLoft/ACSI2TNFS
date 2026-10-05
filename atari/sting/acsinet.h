/*
 * ACSI transport for ACSI_NET.STX: vendor commands to ACSI2TNFS
 * (see ACSI_NET-ontwerp.md, section D). Compiled with -mshort.
 */
#ifndef ACSINET_H
#define ACSINET_H

#define NET_INFO    0x20
#define NET_CTRL    0x21
#define NET_TX      0x22
#define NET_RX      0x23

#define ST_OK       0L
#define ST_BUSY     8L          /* NET_TX: ring full, try again later   */
#define ST_TIMEOUT  (-1L)
#define ST_LOCKED   (-2L)       /* flock taken by someone else: skip    */

/* find the adapter (vendor sub 8 answers "ATL" on every firmware).
   Supervisor mode. Returns the ACSI id or -1. */
int acsi_find(void);

/* one vendor command to the adapter. Supervisor mode. 'wait': in process
   context wait up to 1 s for flock; 0 (STinG poll): give up at once.
   Returns the status byte, ST_TIMEOUT or ST_LOCKED. */
long acsi_net(unsigned char sub, unsigned char arg, void *buf, int sectors, int write, int wait);

#endif
