#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#define DT_DRV_COMPAT wch_ch9120
#define CH9120_NODE DT_INST(0, wch_ch9120)
#define CH9120_RX_BUF_SIZE 1024

LOG_MODULE_REGISTER(eth_ch9120, CONFIG_LOG_DEFAULT_LEVEL);

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

struct ch9120_config {
    struct gpio_dt_spec cfg_gpio;
    struct gpio_dt_spec rst_gpio;
    struct gpio_dt_spec tcp_gpio;

    const struct device *uart_dev;
};

static struct ch9120_runtime ch9120_runtime_data;
static const struct ch9120_config ch9120_config_data = {
    .uart_dev  = DEVICE_DT_GET(DT_PROP(CH9120_NODE, uart)),
    .cfg_gpio  = GPIO_DT_SPEC_INST_GET(0, config_gpios),
    .rst_gpio  = GPIO_DT_SPEC_INST_GET(0, reset_gpios),
    .tcp_gpio  = GPIO_DT_SPEC_INST_GET(0, tcpcs_gpios),
};

/*---- Socket ----*/
static const struct socket_op_vtable ch9120_socket_fd_op_vtable;

static ssize_t ch9120_read(void *obj, void *buf, size_t sz)
{
	return 0;
}

static ssize_t ch9120_write(void *obj, const void *buf, size_t sz)
{
	return 0;
}

static int ch9120_close(void *obj)
{
    return 0;
}

static int ch9120_ioctl(void *obj, unsigned int request, va_list args)
{
    return 0;
}

static int ch9120_bind(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
    return 0;
}

static int ch9120_connect(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	return 0;
}

static int ch9120_listen(void *obj, int backlog)
{
	return 0;
}

static int ch9120_accept(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

static ssize_t ch9120_sendto(void *obj, const void *buf, size_t len, int flags,
			   const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	return 0;
}

static ssize_t ch9120_sendmsg(void *obj, const struct net_msghdr *msg, int flags)
{
	return 0;
}

static ssize_t ch9120_recvfrom(void *obj, void *buf, size_t len, int flags,
			     struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

static ssize_t ch9120_recvmsg(void *obj, struct net_msghdr *msg, int flags)
{
	return 0;
}

static int ch9120_getsockopt(void *obj, int level, int optname,
			   void *optval, net_socklen_t *optlen)
{
	return 0;
}

static int ch9120_setsockopt(void *obj, int level, int optname,
			   const void *optval, net_socklen_t optlen)
{
	return 0;
}

static int ch9120_getpeername(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

static int ch9120_getsockname(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

// Utils
static int socket_family_is_supported(int family)
{
	switch (family) {
	case NET_AF_INET:
		break;
	default:
		return -1;
	}

	return 0;
}

static int socket_type_is_supported(int type)
{
	switch (type) {
	case NET_SOCK_STREAM:
		break;
	case NET_SOCK_DGRAM:
		break;
	default:
		return -1;
	}

	return 0;
}

static int socket_proto_is_supported(int proto)
{
	switch (proto) {
	case NET_IPPROTO_TCP:
		break;
	case NET_IPPROTO_UDP:
		break;
	default:
		return -1;
	}

	return 0;
}
// Utils End

static bool ch9120_socket_is_supported(int family, int type, int proto)
{
    int ret;
    
    ret = socket_family_is_supported(family);
    if (ret < 0) {
        return false;
    }

    ret = socket_type_is_supported(type);
    if (ret < 0) {
        return false;
    }

    ret = socket_proto_is_supported(proto);
    if (ret < 0) {
        return false;
    }

	return true;
}

int ch9120_socket_create(int family, int type, int proto)
{
    int fd;
    struct ch9120_socket *sck = &ch9120_runtime_data.sock;
    //TODO: check sck ?

    k_mutex_lock(&ch9120_runtime_data.drv_lock, K_FOREVER);

    if (sck->in_use) {
        k_mutex_unlock(&ch9120_runtime_data.drv_lock);
        LOG_ERR("Failed to create socket, already in use");
        return -1;
    }

    sck->in_use = true;

    k_mutex_unlock(&ch9120_runtime_data.drv_lock);

    sck->family = family;
    sck->type = type;
    sck->proto = proto;
    sck->state = CH9120_SOCK_OPEN;
    sck->is_nonblocking = false;
    
    k_mutex_init(&sck->lock);
    k_sem_init(&sck->rx_sem, 0, 1);
    k_sem_init(&sck->connect_sem, 0, 1);
    ring_buf_init(&sck->rx_buf, sizeof(sck->rx_buf_data), sck->rx_buf_data);

    fd = zvfs_reserve_fd();
	if (fd < 0) {
        sck->in_use = false;
		return -1;
	}

    zvfs_finalize_typed_fd(fd, sck,
                            (const struct fd_op_vtable *)&ch9120_socket_fd_op_vtable.fd_vtable,
                            ZVFS_MODE_IFSOCK);

	return fd;
}

/*---- Socket END ----*/

/*---- Device Instance ----*/
static int ch9120_init(const struct device *dev)
{
    int ret;
    struct ch9120_runtime *data = dev->data;
    const struct ch9120_config *cfg = dev->config;

    struct uart_config uart_cfg = {
		.baudrate = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

    if (!device_is_ready(cfg->uart_dev)) {
        return -ENODEV;
    }

    ret = uart_configure(cfg->uart_dev, &uart_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to set UART : %d", ret);
		return ret;
	}

    data->sock.in_use = false;
    k_mutex_init(&data->drv_lock);

    return 0;
}

static void ch9120_iface_init(struct net_if *iface)
{
    net_if_socket_offload_set(iface, ch9120_socket_create);
}

static enum ethernet_hw_caps ch9120_get_capabilities(const struct device *dev __unused,
						    struct net_if *iface __unused)
{
    return 0;
}

static int ch9120_set_config(const struct device *dev,
				    struct net_if *iface __unused,
				    enum ethernet_config_type type,
				    const struct ethernet_config *config)
{
    return 0;
}

static int ch9120_start(const struct device *dev, struct net_if *iface)
{
    return 0;
}

static int ch9120_stop(const struct device *dev, struct net_if *iface)
{
    return 0;
}

static int ch9120_send(const struct device *dev, struct net_pkt *pkt)
{
    return 0;
}

/*---- Device Instance END ----*/

static const struct ethernet_api ch9120_if_apis = {
	.iface_api.init = ch9120_iface_init,
	.get_capabilities = ch9120_get_capabilities,
	.set_config = ch9120_set_config,
	.start = ch9120_start,
	.stop = ch9120_stop,
	.send = ch9120_send,
};

static const struct socket_op_vtable ch9120_socket_fd_op_vtable = {
	.fd_vtable = {
		.read = ch9120_read,
		.write = ch9120_write,
		.close = ch9120_close,
		.ioctl = ch9120_ioctl,
	},
	.bind = ch9120_bind,
	.connect = ch9120_connect,
	.listen = ch9120_listen,
	.accept = ch9120_accept,
	.sendto = ch9120_sendto,
	.sendmsg = ch9120_sendmsg,
	.recvfrom = ch9120_recvfrom,
	.recvmsg = ch9120_recvmsg,
	.getsockopt = ch9120_getsockopt,
	.setsockopt = ch9120_setsockopt,
	.getpeername = ch9120_getpeername,
	.getsockname = ch9120_getsockname,
};

NET_DEVICE_DT_INST_OFFLOAD_DEFINE(
    0,
    ch9120_init,
    NULL,
    &ch9120_runtime_data,
    &ch9120_config_data,
    CONFIG_ETH_INIT_PRIORITY,
    &ch9120_if_apis,
    NET_ETH_MTU
);

NET_SOCKET_OFFLOAD_REGISTER(
    ch9120,
    CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY,
    AF_UNSPEC,
	ch9120_socket_is_supported,
    ch9120_socket_create);