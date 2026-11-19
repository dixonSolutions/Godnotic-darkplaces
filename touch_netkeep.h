#ifndef TOUCH_NETKEEP_H
#define TOUCH_NETKEEP_H

// Godnotic: the connection to a server outlives one long frame
// (touch_netkeep.c, docs/NETWORK.md "Staying connected").

struct netconn_s;

void TouchNetKeep_Init(void);
// end of every client frame (touch_net.c TouchNet_Frame): the main thread is alive
void TouchNetKeep_Frame(void);
// a connection is closed (NetConn_Close via TouchNet_Close): the watcher lets go of it
void TouchNetKeep_Closing(struct netconn_s *conn);
// the client's sockets are closed (NetConn_CloseClientPorts)
void TouchNetKeep_PortsClosing(void);

#endif
