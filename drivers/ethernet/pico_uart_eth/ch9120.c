#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "ch9120.h"

#define DT_DRV_COMPAT wch_ch9120
#define CH9120_NODE DT_INST(0, wch_ch9120)

LOG_MODULE_REGISTER(eth_ch9120, CONFIG_LOG_DEFAULT_LEVEL);

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

static int ch9120_init(const struct device *dev)
{
    int ret;
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

static const struct ethernet_api ch9120_if_apis = {
	.iface_api.init = ch9120_iface_init,
	.get_capabilities = ch9120_get_capabilities,
	.set_config = ch9120_set_config,
	.start = ch9120_start,
	.stop = ch9120_stop,
	.send = ch9120_send,
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