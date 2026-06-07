#ifndef __DRIVERS_NET_CH9120_H__
#define __DRIVERS_NET_CH9120_H__

#include <zephyr/net/socket.h>

#define CH9120_RX_BUF_SIZE 1024

enum ch9120_sock_state {
    CH9120_SOCK_CLOSED,
    CH9120_SOCK_OPEN,
    CH9120_SOCK_CONNECTING,
    CH9120_SOCK_CONNECTED,
    CH9120_SOCK_ERROR,
};

struct ch9120_socket {
    bool in_use;

    int type;
    int proto;
    int family;
    enum ch9120_sock_state state;

    struct sockaddr remote_addr;
    struct ring_buf rx_buf;
    uint8_t rx_buf_data[CH9120_RX_BUF_SIZE];
    struct k_sem rx_sem;

    struct k_mutex lock;
    struct k_sem connect_sem;

    bool is_nonblocking;
};

struct ch9120_runtime {

    struct net_if *iface;

    struct ch9120_socket sock;
    struct k_mutex drv_lock;
    struct k_thread rx_thread;

    k_thread_stack_t *rx_stack;
};

int ch9120_socket_create(int family, int type, int proto);

#endif /* __DRIVERS_NET_CH9120_H__ */