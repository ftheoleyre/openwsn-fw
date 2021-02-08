#include "config.h"

#if OPENWSN_UECHO_C

#include "opendefs.h"
#include "uecho.h"
#include "sock.h"
#include "async.h"
#include "openqueue.h"
#include "openserial.h"
#include "packetfunctions.h"

//=========================== variables =======================================

sock_udp_t uecho_sock;

//=========================== prototypes ======================================

void uecho_handler(sock_udp_t *sock, sock_async_flags_t type, void *arg);

//=========================== public ==========================================

void uecho_init(void) {
    // clear local variables
    memset(&uecho_sock, 0, sizeof(sock_udp_t));

    sock_udp_ep_t local;

    local.port = WKP_UDP_ECHO;

    if (sock_udp_create(&uecho_sock, &local, NULL, 0) < 0) {
        openserial_printf("Could not create socket\n");
        return;
    }

    openserial_printf("Created a UDP socket\n");

    sock_udp_set_cb(&uecho_sock, uecho_handler, NULL);
}

void uecho_handler(sock_udp_t *sock, sock_async_flags_t type, void *arg) {
    (void) arg;

    char buf[50];

    if (type & SOCK_ASYNC_MSG_RECV) {
        sock_udp_ep_t remote;
        int16_t res;

        if ((res = sock_udp_recv(sock, buf, sizeof(buf), 0, &remote)) >= 0) {
           openserial_printf("Msg Received (%d bytes, from remote endpoint port:%d, addr: %x%x:%x%x:%x%x:%x%x:%x%x:%x%x:%x%x:%x%x\n",
                                 res,
                                 remote.port,
                                 remote.addr.ipv6[0],remote.addr.ipv6[1],
                                 remote.addr.ipv6[2],remote.addr.ipv6[3],
                                 remote.addr.ipv6[4],remote.addr.ipv6[5],
                                 remote.addr.ipv6[6],remote.addr.ipv6[7],
                                 remote.addr.ipv6[8],remote.addr.ipv6[9],
                                 remote.addr.ipv6[10],remote.addr.ipv6[11],
                                 remote.addr.ipv6[12],remote.addr.ipv6[13],
                                 remote.addr.ipv6[14],remote.addr.ipv6[15]
                              );


 
            if (sock_udp_send(sock, buf, res, &remote) < 0) {
                openserial_printf("Error sending reply\n");
            }
        }
    }
}
//=========================== private =========================================

#endif /* OPENWSN_UECHO_C */
