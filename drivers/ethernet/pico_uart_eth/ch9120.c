#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(eth_ch9120, LOG_LEVEL_INF);

#define DT_DRV_COMPAT wch_ch9120
#define CH9120_NODE DT_INST(0, wch_ch9120)
#define CH9120_UART_PRE_DELAY		30
#define CH9120_BAUD_CONFIG			9600
#define ETH_CH9120_RX_BUF_SIZE		CONFIG_ETH_CH9120_RX_BUF_SIZE
#define CH9120_CONNECT_TIMEOUT_MS   5000
#define CH9120_CONNECT_POLL_MS      10

/* header bytes */
#define CH9120_HDR_0			0x57
#define CH9120_HDR_1			0xAB

/* command codes */
#define CH9120_CMD_SET_MODE			0x10
#define CH9120_CMD_SET_TARGET_IP	0x15
#define CH9120_CMD_SET_TARGET_PORT	0x16
#define CH9120_CMD_SET_DHCP			0x33
#define CH9120_CMD_GET_MAC			0x81

/* exit config mode sequence */
#define CH9120_CMD_SAVE			0x0d
#define CH9120_CMD_RESET		0x0e

/* mode values */
#define CH9120_MODE_TCP_SERVER	0x00
#define CH9120_MODE_TCP_CLIENT	0x01

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
	struct net_sockaddr dst;

	struct sockaddr remote_addr;
	struct ring_buf rx_buf;
	uint8_t rx_buf_data[ETH_CH9120_RX_BUF_SIZE];
	struct k_sem rx_sem;

	struct k_mutex lock;

	bool is_nonblocking;
};

struct ch9120_runtime {

	struct net_if *iface;
	uint8_t mac_addr[6];

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

static const struct socket_op_vtable ch9120_socket_fd_op_vtable;

static bool ch9120_is_tcp_connected(const struct ch9120_config *cfg)
{
    return gpio_pin_get_dt(&cfg->tcp_gpio) == 1;
}

static void ch9120_uart_flush(const struct device *uart_dev)
{
	uint8_t c;

	while (uart_fifo_read(uart_dev, &c, 1) > 0) {
		/* discard */
	}
}

static int ch9120_send_cmd_wait(const struct ch9120_config *cfg,
				uint8_t cmd, const uint8_t *data, size_t len,
				const uint16_t pre_delay, const uint16_t timeout)
{
	uint8_t header[3] = { CH9120_HDR_0, CH9120_HDR_1, cmd };
	const struct device *uart_dev = cfg->uart_dev;
	uint8_t ack;
	int ret;

	/* Enter config mode */
	gpio_pin_set_dt(&cfg->cfg_gpio, 1);

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
				gpio_pin_set_dt(&cfg->cfg_gpio, 0);
				return 0;
			}

			if (ack == 0xEE) {
				LOG_ERR("CH9120 rejected command 0x%02x", cmd);
				gpio_pin_set_dt(&cfg->cfg_gpio, 0);
				return -EPROTO;
			}
		}
		k_msleep(10);
	}

	LOG_ERR("Timeout waiting for ACK on cmd: 0x%02x", cmd);
	gpio_pin_set_dt(&cfg->cfg_gpio, 0);

	return -EIO;
}

static int ch9120_send_cmd_read(const struct ch9120_config *cfg,
				uint8_t cmd, const uint8_t *data, size_t len,
				uint8_t *read_buf, size_t read_len,
				const uint16_t pre_delay, const uint16_t timeout)
{
	uint8_t header[3] = { CH9120_HDR_0, CH9120_HDR_1, cmd };
	const struct device *uart_dev = cfg->uart_dev;
	size_t indx;

	/* Enter config mode */
	gpio_pin_set_dt(&cfg->cfg_gpio, 1);

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

	indx = 0;
	for (int t = 0; t < timeout; t++) {
		while (uart_poll_in(uart_dev, &read_buf[indx]) == 0) {
			indx++;
			if (indx >= read_len) {
				gpio_pin_set_dt(&cfg->cfg_gpio, 0);
				return 0;
			}
		}
		k_msleep(10);
	}

	LOG_ERR("Timeout waiting for ACK on cmd: 0x%02x", cmd);
	gpio_pin_set_dt(&cfg->cfg_gpio, 0);

	return -EIO;
}

static int ch9120_wait_for_connection(const struct ch9120_config *cfg,
				uint32_t timeout_ms)
{
    uint32_t elapsed = 0;

    while (elapsed < timeout_ms) {
        if (ch9120_is_tcp_connected(cfg)) {
            return 0;
        }
        k_msleep(CH9120_CONNECT_POLL_MS);
        elapsed += CH9120_CONNECT_POLL_MS;
    }

    return -ETIMEDOUT;
}

static int ch9120_close(void *obj)
{
	struct ch9120_socket *sck = (struct ch9120_socket *)obj;
	const struct ch9120_config *cfg = &ch9120_config_data;

	if (sck == NULL) {
		LOG_ERR("%s: invalid socket received", __func__);
		return -1;
	}

	k_mutex_lock(&ch9120_runtime_data.drv_lock, K_FOREVER);
	if (!sck->in_use) {
		k_mutex_unlock(&ch9120_runtime_data.drv_lock);
		return -1;
	}

	sck->in_use = false;
	sck->state = CH9120_SOCK_CLOSED;

	ring_buf_reset(&sck->rx_buf);
	k_sem_reset(&sck->rx_sem);

	k_mutex_unlock(&ch9120_runtime_data.drv_lock);

	uart_irq_rx_disable(cfg->uart_dev);
	LOG_DBG("socket closed");
	return 0;
}

static int ch9120_ioctl(void *obj, unsigned int request, va_list args)
{
	return 0;
}

static int ch9120_connect(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	int ret;
	uint8_t mode;
	uint16_t dst_port = 0U;
	uint8_t dst_ip[4];
	uint8_t port_bytes[2];
	struct ch9120_socket *sck = (struct ch9120_socket *)obj;
	const struct ch9120_config *cfg = &ch9120_config_data;

	if (sck == NULL) {
		LOG_ERR("%s: invalid socket received", __func__);
		return -EINVAL;
	}

	if (addr == NULL || addrlen < sizeof(struct sockaddr_in)) {
		return -EINVAL;
	}

	if (addr->sa_family != NET_AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	k_mutex_lock(&sck->lock, K_FOREVER);
	if (sck->state != CH9120_SOCK_OPEN) {
		k_mutex_unlock(&sck->lock);
		return -EISCONN;
	}

	sck->state = CH9120_SOCK_CONNECTING;
	k_mutex_unlock(&sck->lock);

	dst_port = net_ntohs(net_sin(addr)->sin_port);
	port_bytes[0] = dst_port & 0xff;
	port_bytes[1] = (dst_port >> 8) & 0xff;

	memcpy(dst_ip, &net_sin(addr)->sin_addr.s_addr, 4);

	ret = ch9120_send_cmd_wait(cfg, CH9120_CMD_SET_TARGET_IP,
						dst_ip, 4, CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("Failed to send destination IP :%d", ret);
		goto err;
	}

	ret = ch9120_send_cmd_wait(cfg, CH9120_CMD_SET_TARGET_PORT,
						port_bytes, 2, CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("failed to set dst port: %d", ret);
		goto err;
	}

	mode = CH9120_MODE_TCP_CLIENT;
	ret = ch9120_send_cmd_wait(cfg, CH9120_CMD_SET_MODE,
						&mode, 1, CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("failed to set tcp client: %d", ret);
		goto err;
	}

	ret = ch9120_wait_for_connection(cfg, CH9120_CONNECT_TIMEOUT_MS);
    if (ret < 0) {
        LOG_ERR("TCP connection timed out!");
        goto err;
    }

	k_mutex_lock(&sck->lock, K_FOREVER);
	sck->state = CH9120_SOCK_CONNECTED;
	k_mutex_unlock(&sck->lock);

	uart_irq_rx_enable(cfg->uart_dev);

	return 0;

err:
	k_mutex_lock(&sck->lock, K_FOREVER);
	sck->state = CH9120_SOCK_OPEN;
	k_mutex_unlock(&sck->lock);

	return ret;
}

static ssize_t ch9120_sendto(void *obj, const void *buf, size_t len, int flags,
			   const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	struct ch9120_socket *sck = (struct ch9120_socket *)obj;
	const struct ch9120_config *cfg = &ch9120_config_data;
	const uint8_t *data = buf;

	ARG_UNUSED(flags);
	ARG_UNUSED(addr);
	ARG_UNUSED(addrlen);

	if (!buf || len == 0) {
		errno = EINVAL;
		return -1;
	}

	if (sck->state != CH9120_SOCK_CONNECTED) {
		return -ENOTCONN;
	}

	if (len > ETH_CH9120_RX_BUF_SIZE) {
		len = ETH_CH9120_RX_BUF_SIZE;
	}

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(cfg->uart_dev, data[i]);
	}

	return len;
}

static ssize_t ch9120_recvfrom(void *obj, void *buf, size_t len, int flags,
				 struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct ch9120_socket *sck = (struct ch9120_socket *)obj;
	uint32_t read;
	int ret;

	ARG_UNUSED(flags);

	if (sck->state != CH9120_SOCK_CONNECTED) {
		return -ENOTCONN;
	}

	if (addr != NULL && addrlen != NULL) {
		*addrlen = sizeof(sck->dst);
		memcpy(addr, &sck->dst, *addrlen);
	}

	if (sck->is_nonblocking) {
		if (ring_buf_is_empty(&sck->rx_buf)) {
			return -EAGAIN;
		}
	} else {
		ret = k_sem_take(&sck->rx_sem, K_FOREVER);
		if (ret < 0) {
			return -EINTR;
		}

		if (sck->state != CH9120_SOCK_CONNECTED) {
			return -ENOTCONN;
		}
	}
	
	if (len >= ETH_CH9120_RX_BUF_SIZE) {
		len = ETH_CH9120_RX_BUF_SIZE;
	}

	read = ring_buf_get(&sck->rx_buf, buf, ETH_CH9120_RX_BUF_SIZE);
	if (read == 0) {
		return -EAGAIN;
	}

	return (ssize_t)read;
}

static ssize_t ch9120_read(void *obj, void *buf, size_t sz)
{
	return ch9120_recvfrom(obj, buf, sz, 0, NULL, NULL);
}

static ssize_t ch9120_write(void *obj, const void *buf, size_t sz)
{
	return ch9120_sendto(obj, buf, sz, 0, NULL, 0);
}

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
	default:
		return -1;
	}

	return 0;
}

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
	/* TODO: check sck ? */

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

	fd = zvfs_reserve_fd();
	if (fd < 0) {
		k_mutex_lock(&ch9120_runtime_data.drv_lock, K_FOREVER);
		sck->in_use = false;
		k_mutex_unlock(&ch9120_runtime_data.drv_lock);
		return -1;
	}

	zvfs_finalize_typed_fd(fd, sck,
			(const struct fd_op_vtable *)&ch9120_socket_fd_op_vtable.fd_vtable,
			ZVFS_MODE_IFSOCK);

	LOG_DBG("socket created fd:%d", fd);
	return fd;
}

static void ch9120_uart_cb(const struct device *dev_uart, void *user_data)
{
	const struct device *dev = (const struct device *)user_data;
	const struct ch9120_config *cfg = dev->config;
	struct ch9120_socket *sck = &ch9120_runtime_data.sock;
	uint8_t buf[64];
	int read;

	if (!uart_irq_rx_ready(dev)) {
		return;
	}

	/* Check if TCP connection dropped */
    if (!ch9120_is_tcp_connected(cfg)) {
        if (sck->state == CH9120_SOCK_CONNECTED) {
            LOG_WRN("TCP connection lost");
            sck->state = CH9120_SOCK_OPEN;
            k_sem_give(&sck->rx_sem);
        }
        return;
    }

	read = uart_fifo_read(dev_uart, buf, sizeof(buf));
	if (read <= 0) {
		return;
	}

	/* only put data in buffer if socket is connected */
	if (sck->in_use && sck->state == CH9120_SOCK_CONNECTED) {
		ring_buf_put(&sck->rx_buf, buf, read);
		k_sem_give(&sck->rx_sem);
	}
}

static int ch9120_init(const struct device *dev)
{
	int ret;
	uint8_t enable_flag;
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
	gpio_pin_configure_dt(&cfg->tcp_gpio, GPIO_INPUT | GPIO_PULL_UP);

	/* Initialize the Chip */
	gpio_pin_set_dt(&cfg->rst_gpio, 1);
	k_msleep(10);
	gpio_pin_set_dt(&cfg->rst_gpio, 0);
	k_msleep(500);

	enable_flag = 0x01;
	ret = ch9120_send_cmd_wait(cfg, CH9120_CMD_SET_DHCP,
					&enable_flag, 1,
					CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("Failed to set dhcp:%d", ret);
		return -EIO;
	}
	k_msleep(500);

	ret = ch9120_send_cmd_wait(cfg, CH9120_CMD_SAVE,
				NULL, 0,
				CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("Failed to save config :%d", ret);
		return -EIO;
	}
	k_msleep(500);

	ret = ch9120_send_cmd_wait(cfg, CH9120_CMD_RESET,
				NULL, 0, CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("Failed to save config :%d", ret);
		return -EIO;
	}
	k_msleep(1000);

	ret = ch9120_send_cmd_read(cfg, CH9120_CMD_GET_MAC,
		NULL, 0, data->mac_addr, sizeof(data->mac_addr),
		CH9120_UART_PRE_DELAY, 1000);
	if (ret < 0) {
		LOG_ERR("Failed to read mac :%d", ret);
		return -EIO;
	}

	ret = uart_irq_callback_user_data_set(cfg->uart_dev, ch9120_uart_cb, (void *)dev);
	if (ret < 0) {
		LOG_ERR("Couldn't set UART callback");
		return ret;
	}

	/* Initialise driver state */
	data->sock.in_use = false;
	k_mutex_init(&data->drv_lock);

	k_mutex_init(&(data->sock.lock));
	k_sem_init(&(data->sock.rx_sem), 0, 1);
	ring_buf_init(&(data->sock.rx_buf), sizeof(data->sock.rx_buf_data), data->sock.rx_buf_data);

	LOG_INF("CH9120 Initialized Successfully with DHCP");

	return 0;
}

static void ch9120_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct ch9120_runtime *data = dev->data;

	net_if_set_link_addr(iface, data->mac_addr,
			sizeof(data->mac_addr), NET_LINK_ETHERNET);

	data->iface = iface;
	net_if_socket_offload_set(iface, ch9120_socket_create);
}

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
	.bind = NULL,
	.connect = ch9120_connect,
	.listen = NULL,
	.accept = NULL,
	.sendto = ch9120_sendto,
	.sendmsg = NULL,
	.recvfrom = ch9120_recvfrom,
	.recvmsg = NULL,
	.getsockopt = NULL,
	.setsockopt = NULL,
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
