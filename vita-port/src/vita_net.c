/*
 * vita_net.c - Real BSD-socket network layer for phoneME MIDP on PS Vita
 *
 * v01.33: non-blocking mode switched from fcntl(O_NONBLOCK) (a NO-OP on
 * sceNet descriptors) to sceNetSetsockopt(SCE_NET_SO_NBIO) - see
 * vita_set_nbio() below for the 27s-freeze incident details.
 *
 * Replaces the pcsl_socket/pcsl_network stubs in vita_pcsl.c. HTTP/HTTPS
 * (JSR-118) is pure Java over socket://, so implementing the TCP client
 * layer below unlocks HttpConnection for MIDlets.
 *
 * Threading model (matches phoneME's mastermode event pump):
 *   - Java thread calls pcsl_socket_*_start(); on PCSL_NET_WOULDBLOCK it
 *     blocks via midp_thread_wait(NETWORK_WRITE/READ_SIGNAL, fd, ctx).
 *   - midp_check_events() periodically calls checkForSystemSignal()
 *     (vita_input.c). We expose vita_net_poll() from there: it select()s
 *     all registered fds and wakes blocked threads through
 *     midp_thread_signal(NETWORK_*_SIGNAL, fd, PCSL_NET_SUCCESS).
 *   - The corresponding *_finish() then re-issues the syscall, which now
 *     completes. Non-blocking throughout; no extra Vita threads needed.
 *
 * BSD sockets come from VitaSDK newlib (socket/connect/fcntl/select are
 * thin wrappers over sceNet). Net stack must be up before the first
 * socket call: sceSysmoduleLoadModule(SCE_SYSMODULE_NET) + sceNetInit,
 * done lazily on first use in a non-VM thread is unsafe, so vita_main.c
 * calls vita_net_early_init() before starting the VM.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include <pcsl_network.h>
#include <pcsl_socket.h>
#include <pcsl_serversocket.h>
#include <pcsl_datagram.h>

#include <midpServices.h> /* NETWORK_*_SIGNAL for vita_net_poll() */

/* ---- logging: stderr (midp_stderr.log) + ux0:/data/net_log.txt ----
 * Dedicated append-only file: midp_stderr.log is truncated per launch
 * and shared with VM/AMS spew; net_log.txt survives across launches so
 * "no log at all" (crash before freopen) is distinguishable from "net
 * never called". Per-line fopen/fclose - safe against force-kill
 * (the 8-31 empty-log incident). */
static void vnet_log(const char *fmt, ...)
{
    va_list ap;
    char buf[256];
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s", buf);
    fflush(stderr);
    f = fopen("ux0:/data/net_log.txt", "a");
    if (f != NULL) {
        fprintf(f, "%s", buf);
        fclose(f);
    }
}
/* Max fds tracked for the select() scan in vita_net_poll(). */
#define VITA_NET_MAX_FDS 32
#define VITA_NET_MEM_SIZE (1 * 1024 * 1024) /* 1MB: DNS resolver + socket
                                             * buffers live in this pool;
                                             * 64KB starved them (official
                                             * samples use 1MB) */
static char g_net_mem[VITA_NET_MEM_SIZE];

/* FIONREAD is not exposed by newlib's sys/ioctl.h on Vita; available()
 * is implemented with a 1-byte MSG_PEEK recv + socket buffer query. */
static int vita_socket_available(int fd, int *pBytesAvailable) {
    /* Peek one byte non-blocking; if a byte exists data is pending. The
     * exact count is not obtainable cheaply on Vita; available() is a hint
     * in MIDP (InputStream.available()), exactness is not required.
     * The socket is already NBIO (vita_set_nbio at open time), so the
     * MSG_PEEK recv cannot block. */
    unsigned char probe;
    int n = recv(fd, (char *)&probe, 1, MSG_PEEK);

    if (n < 0) {
        return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
    }
    *pBytesAvailable = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Small handle: just carries the fd. na_* "notification adapter" of   */
/* the upstream BSD port is replaced by the select() loop in           */
/* vita_net_poll() — phoneME's SNI-based blocking does not need a      */
/* persistent notifier thread.                                         */
/* ------------------------------------------------------------------ */

typedef struct NetHandle {
    int fd;
} NetHandle;

static NetHandle g_handles[VITA_NET_MAX_FDS] = {
    { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 },
    { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 },
    { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 },
    { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }
};
static int g_net_up = 0;

/* fcntl(O_NONBLOCK) is a NO-OP on sceNet descriptors: newlib's socket fd
 * is the raw sceNetSocket id, and non-blocking mode on Vita is a socket
 * option (SCE_NET_SO_NBIO), not a file status flag. With the old fcntl
 * attempt connect() blocked inside the syscall - observed as a 27s VM
 * freeze ending in ETIMEDOUT (errno 116) under Vita3K. Must be called
 * before connect()/accept()/recv() on every socket we create. */
static void vita_set_nbio(int fd) {
    int on = 1;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                     &on, sizeof(on));
}

int pcsl_lastNetworkError = 0; /* mirrors upstream `lastError` */

/* ---- called from vita_main.c before the VM starts ---- */
void vita_net_early_init(void) {
    SceNetInitParam param;
    int rc;

    if (g_net_up) {
        return;
    }

    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    vnet_log("[NET] sysmodule load: 0x%08X\n", rc);

    memset(&param, 0, sizeof(param));
    param.memory = g_net_mem;
    param.size = sizeof(g_net_mem);
    param.flags = 0;
    rc = sceNetInit(&param);
    vnet_log("[NET] sceNetInit: 0x%08X\n", rc);

    rc = sceNetCtlInit();
    vnet_log("[NET] sceNetCtlInit: 0x%08X\n", rc);

    g_net_up = 1;
}

static NetHandle *na_create(int fd) {
    int i;
    for (i = 0; i < VITA_NET_MAX_FDS; i++) {
        if (g_handles[i].fd == -1) {
            g_handles[i].fd = fd;
            return &g_handles[i];
        }
    }
    return NULL; /* table full — practically unreachable for a MIDlet */
}

static int na_get_fd(void *handle) {
    return handle ? ((NetHandle *)handle)->fd : -1;
}

static void na_destroy(void *handle) {
    if (handle) {
        ((NetHandle *)handle)->fd = -1;
    }
}

/* ================================================================== */
/*.poll: called from checkForSystemSignal() (vita_input.c). Scans all  */
/* registered fds with select(); wakes Java threads blocked on         */
/* NETWORK_READ / NETWORK_WRITE / NETWORK_EXCEPTION signals.           */
/* ================================================================== */
void vita_net_poll(void) {
    fd_set rfds, wfds, efds;
    int maxfd = -1;
    int i;
    struct timeval tv;
    int n;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    for (i = 0; i < VITA_NET_MAX_FDS; i++) {
        int fd = g_handles[i].fd;
        if (fd >= 0 && fd < FD_SETSIZE) {
            FD_SET(fd, &rfds);
            FD_SET(fd, &wfds);
            FD_SET(fd, &efds);
            if (fd > maxfd) {
                maxfd = fd;
            }
        }
    }

    if (maxfd < 0) {
        return; /* nothing registered */
    }

    memset(&tv, 0, sizeof(tv)); /* pure poll, never block */
    n = select(maxfd + 1, &rfds, &wfds, &efds, &tv);
    if (n <= 0) {
        return; /* nothing ready (or select missing on Vita3K; harmless) */
    }

    /* Ready fds: the *_finish() calls happen back in Java threads; here we
     * only need to know *whether* threads are blocked on those fds. That
     * bookkeeping lives in midp_thread_signal(), so dispatch a signal for
     * each ready fd and let the core match it against blocked threads.
     * A signal for a fd nobody waits on is a harmless no-op.
     * NOTE: with no per-handle direction tracking we conservatively wake
     * both READ and WRITE waiters; phoneME matches descriptor+signal type,
     * and a thread waiting on the other direction will just re-block. */
    for (i = 0; i < VITA_NET_MAX_FDS; i++) {
        int fd = g_handles[i].fd;
        if (fd < 0 || fd >= FD_SETSIZE) {
            continue;
        }
        if (FD_ISSET(fd, &rfds)) {
            midp_thread_signal(NETWORK_READ_SIGNAL, fd, PCSL_NET_SUCCESS);
        }
        if (FD_ISSET(fd, &wfds)) {
            midp_thread_signal(NETWORK_WRITE_SIGNAL, fd, PCSL_NET_SUCCESS);
        }
        if (FD_ISSET(fd, &efds)) {
            midp_thread_signal(NETWORK_EXCEPTION_SIGNAL, fd,
                               PCSL_NET_IOERROR);
        }
    }
}

/* ================================================================== */
/* TCP client socket (pcsl_socket.h)                                   */
/* ================================================================== */

int pcsl_socket_open_start(unsigned char *ipBytes, int port,
                           void **pHandle, void **pContext) {
    int fd;
    int one = 1;
    struct sockaddr_in addr;
    NetHandle *h;

    (void)pContext;

    vita_net_early_init();
    vnet_log("[NET] socket_open ip=%u.%u.%u.%u port=%d\n",
             ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3], port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    pcsl_lastNetworkError = errno;
    if (fd < 0) {
        vnet_log("[NET] socket() FAILED errno=%d\n", errno);
        return PCSL_NET_IOERROR;
    }

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    vita_set_nbio(fd); /* non-blocking connect: EINPROGRESS => wait on WRITE */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr.s_addr, ipBytes, 4);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        h = na_create(fd);
        if (h == NULL) { close(fd); return PCSL_NET_IOERROR; }
        *pHandle = h;
        vnet_log("[NET] connect immediate OK fd=%d\n", fd);
        return PCSL_NET_SUCCESS;
    }

    if (errno == EINPROGRESS || errno == EALREADY) {
        h = na_create(fd);
        if (h == NULL) { close(fd); return PCSL_NET_IOERROR; }
        *pHandle = h;
        *pContext = NULL;
        vnet_log("[NET] connect EINPROGRESS fd=%d (thread will block)\n", fd);
        return PCSL_NET_WOULDBLOCK;
    }

    pcsl_lastNetworkError = errno;
    vnet_log("[NET] connect FAILED errno=%d\n", errno);
    close(fd);
    return PCSL_NET_CONNECTION_NOTFOUND;
}

int pcsl_socket_open_finish(void *handle, void *context) {
    int err = 0;
    socklen_t errlen = sizeof(err);
    int fd = na_get_fd(handle);

    (void)context;
    if (fd < 0) {
        return PCSL_NET_IOERROR;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) < 0) {
        pcsl_lastNetworkError = errno;
        vnet_log("[NET] open_finish getsockopt FAILED errno=%d\n", errno);
    } else {
        pcsl_lastNetworkError = err;
    }

    if (err == 0) {
        vnet_log("[NET] connect DONE fd=%d\n", fd);
        return PCSL_NET_SUCCESS;
    }
    vnet_log("[NET] connect FAILED fd=%d SO_ERROR=%d\n", fd, err);
    na_destroy(handle);
    close(fd);
    return PCSL_NET_IOERROR;
}

static int socket_read_common(void *handle, unsigned char *pData, int len,
                              int *pBytesRead) {
    int n;
    int fd = na_get_fd(handle);
    if (fd < 0) {
        return PCSL_NET_INTERRUPTED;
    }

    n = recv(fd, (char *)pData, len, 0);
    pcsl_lastNetworkError = errno;

    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS) {
            return PCSL_NET_WOULDBLOCK;
        }
        if (errno == EINTR) {
            return PCSL_NET_INTERRUPTED;
        }
        vnet_log("[NET] recv FAILED fd=%d errno=%d\n", fd, errno);
        return PCSL_NET_IOERROR;
    }
    if (n == 0) {
        vnet_log("[NET] recv EOF fd=%d\n", fd);
    }
    /* n == 0 means orderly EOF; report as success with 0 bytes so the
     * Java layer sees stream end (that is how the BSD port behaves). */
    *pBytesRead = n;
    return PCSL_NET_SUCCESS;
}

int pcsl_socket_read_start(void *handle, unsigned char *pData, int len,
                           int *pBytesRead, void **pContext) {
    (void)pContext;
    return socket_read_common(handle, pData, len, pBytesRead);
}

int pcsl_socket_read_finish(void *handle, unsigned char *pData, int len,
                            int *pBytesRead, void *context) {
    (void)context;
    return socket_read_common(handle, pData, len, pBytesRead);
}

static int socket_write_common(void *handle, char *pData, int len,
                               int *pBytesWritten) {
    int n;
    int fd = na_get_fd(handle);
    if (fd < 0) {
        return PCSL_NET_INTERRUPTED;
    }

    n = send(fd, pData, len, 0);
    pcsl_lastNetworkError = errno;

    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS) {
            return PCSL_NET_WOULDBLOCK;
        }
        if (errno == EINTR) {
            return PCSL_NET_INTERRUPTED;
        }
        vnet_log("[NET] send FAILED fd=%d errno=%d\n", fd, errno);
        return PCSL_NET_IOERROR;
    }
    *pBytesWritten = n;
    return PCSL_NET_SUCCESS;
}

int pcsl_socket_write_start(void *handle, char *pData, int len,
                            int *pBytesWritten, void **pContext) {
    (void)pContext;
    return socket_write_common(handle, pData, len, pBytesWritten);
}

int pcsl_socket_write_finish(void *handle, char *pData, int len,
                             int *pBytesWritten, void *context) {
    (void)context;
    return socket_write_common(handle, pData, len, pBytesWritten);
}

int pcsl_socket_available(void *handle, int *pBytesAvailable) {
    int fd = na_get_fd(handle);
    if (fd < 0 || vita_socket_available(fd, pBytesAvailable) < 0) {
        pcsl_lastNetworkError = errno;
        return PCSL_NET_IOERROR;
    }
    return PCSL_NET_SUCCESS;
}

int pcsl_socket_shutdown_output(void *handle) {
    int fd = na_get_fd(handle);
    if (fd >= 0) {
        shutdown(fd, SHUT_WR); /* 1 == SHUT_WR */
    }
    return PCSL_NET_SUCCESS;
}

int pcsl_socket_close_start(void *handle, void **pContext) {
    int fd = na_get_fd(handle);
    (void)pContext;
    na_destroy(handle);
    if (fd >= 0) {
        close(fd);
    }
    return PCSL_NET_SUCCESS;
}

int pcsl_socket_close_finish(void *handle, void *context) {
    (void)handle; (void)context;
    return PCSL_NET_INVALID;
}

int pcsl_socket_getlocaladdr(void *handle, char *pAddress) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = na_get_fd(handle);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        return PCSL_NET_IOERROR;
    }
    strcpy(pAddress, inet_ntoa(sa.sin_addr));
    return PCSL_NET_SUCCESS;
}

int pcsl_socket_getremoteaddr(void *handle, char *pAddress) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = na_get_fd(handle);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&sa, &sl) < 0) {
        return PCSL_NET_IOERROR;
    }
    strcpy(pAddress, inet_ntoa(sa.sin_addr));
    return PCSL_NET_SUCCESS;
}

/* ================================================================== */
/* Server sockets (pcsl_serversocket.h) — limited use, but cheap       */
/* ================================================================== */

int pcsl_server_socket_open_start(int port, void **pHandle, void **pContext) {
    int fd;
    int one = 1;
    struct sockaddr_in addr;
    NetHandle *h;

    (void)pContext;
    vita_net_early_init();

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        pcsl_lastNetworkError = errno;
        return PCSL_NET_IOERROR;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
    vita_set_nbio(fd); /* accept() must return EWOULDBLOCK, not block */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        pcsl_lastNetworkError = errno;
        close(fd);
        return PCSL_NET_IOERROR;
    }

    h = na_create(fd);
    if (h == NULL) { close(fd); return PCSL_NET_IOERROR; }
    *pHandle = h;
    return PCSL_NET_SUCCESS;
}

int pcsl_server_socket_open_finish(void **handle, void **context) {
    (void)handle; (void)context;
    return PCSL_NET_SUCCESS; /* open was synchronous */
}

int pcsl_serversocket_accept_start(void *handle, void **pHandleClient,
                                   void **pContext) {
    int client;
    NetHandle *h;
    int fd = na_get_fd(handle);

    (void)pContext;
    if (fd < 0) {
        return PCSL_NET_IOERROR;
    }
    client = accept(fd, NULL, NULL);
    pcsl_lastNetworkError = errno;
    if (client < 0) {
        return (errno == EWOULDBLOCK || errno == EAGAIN)
               ? PCSL_NET_WOULDBLOCK : PCSL_NET_IOERROR;
    }
    h = na_create(client);
    if (h == NULL) { close(client); return PCSL_NET_IOERROR; }
    *pHandleClient = h;
    return PCSL_NET_SUCCESS;
}

int pcsl_serversocket_accept_finish(void *handle, void **pHandleClient,
                                    void *context) {
    (void)handle; (void)pHandleClient; (void)context;
    return PCSL_NET_INVALID;
}

void pcsl_serversocket_close_start(void *handle, void **pContext) {
    int fd = na_get_fd(handle);
    (void)pContext;
    na_destroy(handle);
    if (fd >= 0) {
        close(fd);
    }
}

void pcsl_serversocket_close_finish(void *handle, void *context) {
    (void)handle; (void)context;
}

/* ================================================================== */
/* Network housekeeping (pcsl_network.h)                               */
/* ================================================================== */

int pcsl_network_init(void) {
    vita_net_early_init();
    return PCSL_NET_SUCCESS;
}

int pcsl_network_init_start(PCSL_NET_CALLBACK cb) {
    (void)cb;
    return pcsl_network_init();
}

int pcsl_network_init_finish(void) {
    return PCSL_NET_SUCCESS;
}

int pcsl_network_finalize_start(PCSL_NET_CALLBACK cb) {
    (void)cb;
    return PCSL_NET_SUCCESS;
}

int pcsl_network_finalize_finish(void) {
    return PCSL_NET_SUCCESS;
}

int pcsl_network_error(void *handle) {
    (void)handle;
    return pcsl_lastNetworkError;
}

int pcsl_network_getLocalHostName(char *pLocalHost) {
    /* No meaningful hostname on Vita; a fixed id avoids surprises. */
    strcpy(pLocalHost, "psvita");
    return PCSL_NET_SUCCESS;
}

int pcsl_network_getLocalIPAddressAsString(char *pLocalIPAddress) {
    SceNetCtlInfo info;
    vita_net_early_init();
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) {
        vnet_log("[NET] getLocalIP: NetCtl INFO FAILED (wifi down?)\n");
        strcpy(pLocalIPAddress, "0.0.0.0");
        return PCSL_NET_SUCCESS;
    }
    vnet_log("[NET] getLocalIP: %s\n", info.ip_address);
    strncpy(pLocalIPAddress, info.ip_address, 15);
    pLocalIPAddress[15] = '\0';
    return PCSL_NET_SUCCESS;
}

int pcsl_network_gethostbyname_start(char *hostname, unsigned char *pAddress,
                                     int maxLen, int *pLen,
                                     void **pHandle, void **pContext) {
    struct hostent *hp;
    int realLen;

    (void)pHandle; (void)pContext;
    vita_net_early_init();

    hp = gethostbyname(hostname);
    pcsl_lastNetworkError = errno;
    if (hp == NULL || hp->h_addrtype != AF_INET) {
        vnet_log("[NET] gethostbyname('%s') FAILED h_errno/errno=%d\n",
                 hostname, errno);
        return PCSL_NET_IOERROR;
    }
    realLen = 4;
    if (realLen > maxLen) {
        return PCSL_NET_INVALID;
    }
    memcpy(pAddress, hp->h_addr_list[0], realLen);
    *pLen = realLen;
    vnet_log("[NET] gethostbyname('%s') = %u.%u.%u.%u\n",
             hostname, pAddress[0], pAddress[1], pAddress[2], pAddress[3]);
    return PCSL_NET_SUCCESS;
}

int pcsl_network_gethostbyname_finish(unsigned char *pAddress, int maxLen,
                                      int *pLen, void *handle, void *context) {
    (void)pAddress; (void)maxLen; (void)pLen; (void)handle; (void)context;
    return PCSL_NET_INVALID;
}

int pcsl_network_getHostByAddr_start(int ipn, char *host,
                                     void **pHandle, void **pContext) {
    struct in_addr a;
    (void)pHandle; (void)pContext;
    a.s_addr = ipn;
    strcpy(host, inet_ntoa(a));
    return PCSL_NET_SUCCESS;
}

int pcsl_network_getHostByAddr_finish(int ipn, char *host,
                                      void **pHandle, void *context) {
    (void)ipn; (void)host; (void)pHandle; (void)context;
    return PCSL_NET_INVALID;
}

int pcsl_network_addrToString(unsigned char *ipBytes,
                              unsigned short **pResult, int *pResultLen) {
    struct in_addr a;
    char tmp[80];
    int n, i;

    memcpy(&a.s_addr, ipBytes, 4);
    strncpy(tmp, inet_ntoa(a), sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    n = strlen(tmp);

    /* plain malloc: the only dynamic allocation here; freed by the caller
     * (Java address-string helper) with the matching free */
    *pResult = (unsigned short *)malloc((n + 1) * sizeof(unsigned short));
    if (*pResult == NULL) {
        return PCSL_NET_IOERROR;
    }
    for (i = 0; i < n; i++) {
        (*pResult)[i] = (unsigned short)tmp[i];
    }
    (*pResult)[n] = 0;
    *pResultLen = n;
    return PCSL_NET_SUCCESS;
}

int pcsl_network_getlocalport(void *handle, int *pPortNumber) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = na_get_fd(handle);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        return PCSL_NET_IOERROR;
    }
    *pPortNumber = ntohs(sa.sin_port);
    return PCSL_NET_SUCCESS;
}

int pcsl_network_getremoteport(void *handle, int *pPortNumber) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = na_get_fd(handle);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&sa, &sl) < 0) {
        return PCSL_NET_IOERROR;
    }
    *pPortNumber = ntohs(sa.sin_port);
    return PCSL_NET_SUCCESS;
}

int pcsl_network_getsockopt(void *handle, int flag, int *pOptval) {
    int fd = na_get_fd(handle);
    int optname;
    struct linger lg;
    socklen_t sz;

    if (fd < 0) {
        return PCSL_NET_IOERROR;
    }
    switch (flag) {
    case 0:  optname = TCP_NODELAY;  sz = sizeof(*pOptval); goto plain;
    case 1:  optname = SO_LINGER;    lg.l_onoff = lg.l_linger = 0;
             sz = sizeof(lg);
             if (getsockopt(fd, SOL_SOCKET, optname,
                            (char *)&lg, &sz) < 0) {
                 return PCSL_NET_IOERROR;
             }
             *pOptval = (lg.l_onoff == 0) ? 0 : lg.l_linger;
             return PCSL_NET_SUCCESS;
    case 2:  optname = SO_KEEPALIVE; sz = sizeof(*pOptval); goto plain;
    case 3:  optname = SO_RCVBUF;    sz = sizeof(*pOptval); goto plain;
    case 4:  optname = SO_SNDBUF;    sz = sizeof(*pOptval); goto plain;
    default: return PCSL_NET_INVALID;
    }
plain:
    if (getsockopt(fd, (optname == TCP_NODELAY) ? IPPROTO_TCP : SOL_SOCKET,
                   optname, (char *)pOptval, &sz) < 0) {
        return PCSL_NET_IOERROR;
    }
    return PCSL_NET_SUCCESS;
}

int pcsl_network_setsockopt(void *handle, int flag, int optval) {
    int fd = na_get_fd(handle);
    int one = 1;
    struct linger lg;

    if (fd < 0) {
        return PCSL_NET_IOERROR;
    }
    switch (flag) {
    case 0: return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                              (char *)&one, sizeof(one)) == 0
                    ? PCSL_NET_SUCCESS : PCSL_NET_IOERROR;
    case 1: lg.l_onoff = (optval != 0);
            lg.l_linger = optval;
            return setsockopt(fd, SOL_SOCKET, SO_LINGER,
                              (char *)&lg, sizeof(lg)) == 0
                    ? PCSL_NET_SUCCESS : PCSL_NET_IOERROR;
    case 2: return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                              (char *)&one, sizeof(one)) == 0
                    ? PCSL_NET_SUCCESS : PCSL_NET_IOERROR;
    case 3:
    case 4: {
        int sz = optval;
        int name = (flag == 3) ? SO_RCVBUF : SO_SNDBUF;
        return setsockopt(fd, SOL_SOCKET, name,
                          (char *)&sz, sizeof(sz)) == 0
                    ? PCSL_NET_SUCCESS : PCSL_NET_IOERROR;
    }
    default: return PCSL_NET_INVALID;
    }
}

int pcsl_network_getRawIpNumber(unsigned char *ipBytes) {
    struct in_addr a;
    memcpy(&a.s_addr, ipBytes, 4);
    return a.s_addr;
}

unsigned int pcsl_network_htonl(unsigned int v) { return htonl(v); }
unsigned int pcsl_network_ntohl(unsigned int v) { return ntohl(v); }
unsigned short pcsl_network_htons(unsigned short v) { return htons(v); }
unsigned short pcsl_network_ntohs(unsigned short v) { return ntohs(v); }

char *pcsl_inet_ntoa(void *ipBytes) {
    struct in_addr a;
    memcpy(&a.s_addr, ipBytes, 4);
    return inet_ntoa(a);
}

void pcsl_add_network_notifier(void *handle, int event) {
    (void)handle; (void)event; /* select()-loop model: notifier not used */
}

void pcsl_remove_network_notifier(void *handle, int event) {
    (void)handle; (void)event;
}

/* ================================================================== */
/* UDP datagrams (pcsl_datagram.h) — full implementation, same model   */
/* ================================================================== */

int pcsl_datagram_open_start(int port, void **pHandle, void **pContext) {
    int fd;
    int one = 1;
    NetHandle *h;
    struct sockaddr_in addr;

    (void)pContext;
    vita_net_early_init();

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        pcsl_lastNetworkError = errno;
        return PCSL_NET_IOERROR;
    }
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof(one));
    vita_set_nbio(fd); /* recvfrom must EWOULDBLOCK, never block */

    if (port != 0) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            pcsl_lastNetworkError = errno;
            close(fd);
            return PCSL_NET_IOERROR;
        }
    }

    h = na_create(fd);
    if (h == NULL) { close(fd); return PCSL_NET_IOERROR; }
    *pHandle = h;
    return PCSL_NET_SUCCESS;
}

int pcsl_datagram_open_finish(void *handle, void *context) {
    (void)handle; (void)context;
    return PCSL_NET_SUCCESS;
}

int pcsl_datagram_read_start(void *handle, unsigned char *pAddress, int *port,
                             char *pBuffer, int bufferSize,
                             int *pBytesRead, void **pContext) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = na_get_fd(handle);
    int n;

    (void)pContext;
    if (fd < 0) {
        return PCSL_NET_INTERRUPTED;
    }
    n = recvfrom(fd, pBuffer, bufferSize, 0,
                 (struct sockaddr *)&sa, &sl);
    pcsl_lastNetworkError = errno;
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return PCSL_NET_WOULDBLOCK;
        }
        return (errno == EINTR) ? PCSL_NET_INTERRUPTED : PCSL_NET_IOERROR;
    }
    memcpy(pAddress, &sa.sin_addr.s_addr, 4);
    *port = ntohs(sa.sin_port);
    *pBytesRead = n;
    return PCSL_NET_SUCCESS;
}

int pcsl_datagram_read_finish(void *handle, unsigned char *pAddress, int *port,
                              char *pBuffer, int bufferSize,
                              int *pBytesRead, void *context) {
    (void)context;
    return pcsl_datagram_read_start(handle, pAddress, port, pBuffer,
                                    bufferSize, pBytesRead, NULL);
}

int pcsl_datagram_write_start(void *handle, unsigned char *pAddress, int port,
                              char *pBuffer, int bufferSize,
                              int *pBytesWritten, void **pContext) {
    struct sockaddr_in sa;
    int fd = na_get_fd(handle);
    int n;

    (void)pContext;
    if (fd < 0) {
        return PCSL_NET_INTERRUPTED;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr.s_addr, pAddress, 4);

    n = sendto(fd, pBuffer, bufferSize, 0,
               (struct sockaddr *)&sa, sizeof(sa));
    pcsl_lastNetworkError = errno;
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return PCSL_NET_WOULDBLOCK;
        }
        return (errno == EINTR) ? PCSL_NET_INTERRUPTED : PCSL_NET_IOERROR;
    }
    *pBytesWritten = n;
    return PCSL_NET_SUCCESS;
}

int pcsl_datagram_write_finish(void *handle, unsigned char *pAddress, int port,
                               char *pBuffer, int bufferSize,
                               int *pBytesWritten, void *context) {
    (void)context;
    return pcsl_datagram_write_start(handle, pAddress, port, pBuffer,
                                     bufferSize, pBytesWritten, NULL);
}

int pcsl_datagram_close_start(void *handle, void **pContext) {
    int fd = na_get_fd(handle);
    (void)pContext;
    na_destroy(handle);
    if (fd >= 0) {
        close(fd);
    }
    return PCSL_NET_SUCCESS;
}

int pcsl_datagram_close_finish(void *handle, void *context) {
    (void)handle; (void)context;
    return PCSL_NET_INVALID;
}
