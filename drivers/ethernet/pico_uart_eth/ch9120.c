#include <zephyr/net/net_ip.h>
#include <zephyr/drivers/uart.h>
#include "ch9120.h"

struct ch9120_config {
    struct gpio_dt_spec cfg_gpio;
    struct gpio_dt_spec rst_gpio;
    struct gpio_dt_spec tcp_gpio;
};

static struct ch9120_runtime ch9120_runtime_data;
static struct ch9120_config ch9120_config_data;

static int ch9120_init(const struct device *dev)
{
    int ret;
    struct ch9120_runtime *data = dev->data;

    struct uart_config uart_cfg = {
		.baudrate = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

    if (!device_is_ready(data->uart_dev)) {
        return -ENODEV;
    }

    ret = uart_configure(data->uart_dev, &uart_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to set UART baud rate to %d: %d", baudrate, ret);
		return ret;
	}

    return 0;
}

static void ch9120_iface_init(struct net_if *iface)
{
    net_if_socket_offload_set(iface, ch9120_socket_create);
}

static const struct ethernet_api ch9120_if_apis = {
	.iface_api.init = ch9120_iface_init,
	.get_capabilities = ch9120_get_capabilities,
	.set_config = ch9120_set_config,
	.start = ch9120_hw_start,
	.stop = ch9120_hw_stop,
	.send = ch9120_l2_tx,
};

ch9120_runtime_data.uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

NET_DEVICE_DT_INST_DEFINE(
    0,
    ch9120_init,
    NULL,
    &ch9120_runtime_data,
    &ch9120_config_data,
    CONFIG_ETH_INIT_PRIORITY,
    &ch9120_if_apis,
    OFFLOADED_NETDEV_L2,
    NET_L2_GET_CTX_TYPE(OFFLOADED_NETDEV_L2),
    NET_ETH_MTU
);