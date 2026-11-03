#ifndef VALKEY_REST_H
#define VALKEY_REST_H

#include "server.h"

int isHttpRequest(const char *buf, size_t len);
void parseRestBuffer(client *c);
void processRestReply(client *c);
void initRestListener(void);
void cleanRestListener(void);

#endif /* VALKEY_REST_H */
