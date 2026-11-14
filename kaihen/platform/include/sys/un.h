/* kaiHEN compat: Unix-domain socket addresses. The console has none, so a connect fails and the
 * caller (Discord presence) gives up as it would with no Discord running. */
#pragma once
#include <sys/socket.h>
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};
