#ifndef NET_H
#define NET_H

int net_init();
void net_cleanup();

int start_server(int port);
int start_client(const char *addr, int port);

#endif /*NET_H*/
