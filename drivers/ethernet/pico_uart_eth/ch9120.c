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

LOG_MODULE_REGISTER(eth_ch9120, LOG_LEVEL_INF);

/* CH9120 binary protocol header bytes */
#define CH9120_HDR_0            0x57
#define CH9120_HDR_1            0xAB

/* command codes */
#define CH9120_CMD_MODE         0x10  /* set mode: TCP_SERVER/CLIENT, UDP_SERVER/CLIENT */
#define CH9120_CMD_LOCAL_IP     0x11  /* set local IP address */
#define CH9120_CMD_SUBNET_MASK  0x12  /* set subnet mask */
#define CH9120_CMD_GATEWAY      0x13  /* set gateway */
#define CH9120_CMD_LOCAL_PORT   0x14  /* set local port */
#define CH9120_CMD_TARGET_IP    0x15  /* set target IP */
#define CH9120_CMD_TARGET_PORT  0x16  /* set target port */
#define CH9120_CMD_PORT_RANDOM  0x17  /* enable random local port */
#define CH9120_CMD_BAUD         0x21  /* set UART baud rate */
#define CH9120_CMD_DHCP         0x33  /* enable/disable DHCP */

/* exit config mode sequence */
#define CH9120_CMD_SAVE         0x0d  /* save config to flash */
#define CH9120_CMD_RESET        0x0e  /* reset chip */
#define CH9120_CMD_EXIT         0x5e  /* exit config mode */

/* mode values */
#define CH9120_MODE_TCP_SERVER  0x00
#define CH9120_MODE_TCP_CLIENT  0x01
#define CH9120_MODE_UDP_SERVER  0x02
#define CH9120_MODE_UDP_CLIENT  0x03

/* baud rates */
#define CH9120_BAUD_CONFIG      9600
#define CH9120_BAUD_DATA        115200

#define CH9120_UART_PRE_DELAY   30
#define CH9120_UART_PRE_DELAY   50

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

static void ch9130_uart_cb(const struct device *dev_uart, void *user_data)
{

}

// static void ch9120_send_cmd(const struct device *uart_dev,
//                              uint8_t cmd, const uint8_t *data, size_t len)
// {
//     uint8_t header[3] = { CH9120_HDR_0, CH9120_HDR_1, cmd };

//     /* send header */
//     for (int i = 0; i < 3; i++) {
//         uart_poll_out(uart_dev, header[i]);
//     }

//     /* send data bytes */
//     for (size_t i = 0; i < len; i++) {
//         uart_poll_out(uart_dev, data[i]);
//     }

//     k_msleep(10);
// }

static void ch9120_uart_flush(const struct device *uart_dev)
{
    uint8_t c;

    while (uart_fifo_read(uart_dev, &c, 1) > 0) {
        /* discard */
    }
}

static int ch9120_send_cmd_and_wait_ack(const struct device *uart_dev, 
                                        uint8_t cmd, const uint8_t *data, size_t len,
                                        const uint16_t pre_delay, const uint16_t timeout)
{
    uint8_t header[3] = { CH9120_HDR_0, CH9120_HDR_1, cmd };
    uint8_t ack;
    int ret;

    ch9120_uart_flush(uart_dev);

    for (int i = 0; i < 3; i++) {
        uart_poll_out(uart_dev, header[i]);
    }

    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uart_dev, data[i]);
    }

    if (pre_delay) {
        k_msleep(pre_delay);
    }

    /* Wait for 0xAA Acknowledgment from CH9120 */
    for (int t = 0; t < timeout; t++) {
        ret = uart_poll_in(uart_dev, &ack);
        if (ret == 0) {
            if (ack == 0xAA) {
                return 0;   /* Success! Command acknowledged. */
            }

            if (ack == 0xEE) {
                LOG_ERR("CH9120 rejected command 0x%02x", cmd);
                return -EPROTO;
            }
        }
        k_msleep(10);
    }

    LOG_ERR("Timeout waiting for ACK on cmd: 0x%02x", cmd);
    return -EIO;
}

// static void ch9120_config_mode_enter(const struct ch9120_config *cfg)
// {
//     gpio_pin_set_dt(&cfg->rst_gpio, 1);
//     gpio_pin_set_dt(&cfg->cfg_gpio, 0);
// }

// static void ch9120_config_mode_exit(const struct ch9120_config *cfg)
// {
//     uint8_t save_cmd  = 0x0d;
//     uint8_t reset_cmd = 0x0e;
//     uint8_t exit_cmd  = 0x5e;

//     ch9120_send_cmd(cfg->uart_dev, save_cmd,  NULL, 0);
//     k_msleep(200);
//     ch9120_send_cmd(cfg->uart_dev, reset_cmd, NULL, 0);
//     k_msleep(200);
//     ch9120_send_cmd(cfg->uart_dev, exit_cmd,  NULL, 0);
//     k_msleep(200);

//     gpio_pin_set_dt(&cfg->cfg_gpio, 1);
// }

// static void parse_ip(const char *str, uint8_t ip[4])
// {
//     sscanf(str, "%hhu.%hhu.%hhu.%hhu",
//            &ip[0], &ip[1], &ip[2], &ip[3]);
// }

static int ch9120_init(const struct device *dev)
{
    int ret;
    struct ch9120_runtime *data = dev->data;
    const struct ch9120_config *cfg = dev->config;

    struct uart_config uart_cfg = {
		.baudrate = CH9120_BAUD_CONFIG,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

    k_msleep(1000);

    /* Initialise UART*/
    if (!device_is_ready(cfg->uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    LOG_INF("uart: device is ready");

    ret = uart_configure(cfg->uart_dev, &uart_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure UART : %d", ret);
		return ret;
	}

    LOG_INF("uart: configured ");

    /* Initialise gpios */
    if (!gpio_is_ready_dt(&cfg->rst_gpio) ||
        !gpio_is_ready_dt(&cfg->cfg_gpio) ||
        !gpio_is_ready_dt(&cfg->tcp_gpio)) {
        LOG_ERR("GPIOs not ready");
        return -1;
    }

    gpio_pin_configure_dt(&cfg->rst_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&cfg->cfg_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&cfg->tcp_gpio, GPIO_INPUT);

    /* Enter config mode */
    gpio_pin_set_dt(&cfg->rst_gpio, 0);
    gpio_pin_set_dt(&cfg->cfg_gpio, 1);

    k_msleep(500);

    uint8_t dhcp_enable = 0x01;
    ret = ch9120_send_cmd_and_wait_ack(cfg->uart_dev, CH9120_CMD_DHCP,
                                        &dhcp_enable, 1,
                                        CH9120_UART_PRE_DELAY, 1000);
    if (ret < 0) {
        LOG_ERR("Failed to set dhcp:%d", ret);
        return -EIO;
    }

    uint32_t baud = CH9120_BAUD_DATA;
    uint8_t  baud_bytes[4] = {
        baud & 0xff,
        (baud >> 8)  & 0xff,
        (baud >> 16) & 0xff,
        (baud >> 24) & 0xff,
    };

    ret = ch9120_send_cmd_and_wait_ack(cfg->uart_dev, CH9120_CMD_BAUD,
                                        baud_bytes, 4,
                                        CH9120_UART_PRE_DELAY, 1000);
    if (ret < 0) {
        LOG_ERR("Failed to set BAUD Rate :%d", ret);
        return -EIO;
    }

    ret = ch9120_send_cmd_and_wait_ack(cfg->uart_dev, CH9120_CMD_SAVE,
                                NULL, 0,
                                CH9120_UART_PRE_DELAY, 1000);
    if (ret < 0) {
        LOG_ERR("Failed to save config :%d", ret);
        return -EIO;
    }

    /* Exit config mode */
    gpio_pin_set_dt(&cfg->cfg_gpio, 0);

    /* Hardware reset the CH9120 to apply new baud rate and DHCP */
    gpio_pin_set_dt(&cfg->rst_gpio, 1);
    k_msleep(50);
    gpio_pin_set_dt(&cfg->rst_gpio, 0);

    /* Wait for the chip to boot up */
    k_msleep(500);

    /* Reconfigure UART for DATA Mode */
    uart_cfg.baudrate = CH9120_BAUD_DATA;
    ret = uart_configure(cfg->uart_dev, &uart_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure UART for data mode : %d", ret);
		return ret;
	}

    ch9120_uart_flush(cfg->uart_dev);

    ret = uart_irq_callback_user_data_set(cfg->uart_dev, ch9130_uart_cb, (void *)dev);
	if (ret < 0) {
		LOG_ERR("Couldn't set UART callback");
		return ret;
	}
	uart_irq_rx_enable(cfg->uart_dev);

    /* Initialise driver state */
    data->sock.in_use = false;
    k_mutex_init(&data->drv_lock);

    LOG_INF("CH9120 Initialized Successfully with DHCP");

    return 0;
}

static void ch9120_iface_init(struct net_if *iface)
{
    const struct device *dev = net_if_get_device(iface);
    struct ch9120_runtime *data = dev->data;

    data->iface = iface;

    net_if_socket_offload_set(iface, ch9120_socket_create);
    net_if_flag_set(iface, NET_IF_NO_AUTO_START);
}

/*---- Device Instance END ----*/

static struct offloaded_if_api ch9120_if_apis = {
	.iface_api.init = ch9120_iface_init,
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