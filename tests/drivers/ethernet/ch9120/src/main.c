#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>

ZTEST(ch9120_init, test_device_ready)
{
    const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ch9120_eth0));

    zassert_not_null(dev, "CH9120 device not found in devicetree");
    zassert_true(device_is_ready(dev), "CH9120 device not ready");
}

ZTEST(ch9120_init, test_net_if_exists)
{
    struct net_if *iface = net_if_get_default();

    zassert_not_null(iface, "No default network interface found");
}

ZTEST_SUITE(ch9120_init, NULL, NULL, NULL, NULL, NULL);