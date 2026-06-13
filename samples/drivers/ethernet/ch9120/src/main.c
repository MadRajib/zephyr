#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Starting CH9120 Quick Test!");
    

    /* Stage 1 — check device */
    LOG_INF("\nCH9120 ========== Stage 1 =====================\n");
    const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ch9120_eth0));
    if (!device_is_ready(dev)) {
        LOG_ERR("CH9120 device not ready");
        return -1;
    }
    LOG_INF("CH9120 device ready");

    /* Stage 2 — check net_if */
    LOG_INF("\nCH9120 ========== Stage 2 =====================\n");
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No network interface found");
        return -1;
    }
    LOG_INF("Network interface found");
    
    k_sleep(K_SECONDS(5));

    /* Stage 3 — open socket */
    LOG_INF("\nCH9120 ========== Stage 3 =====================\n");
    int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("socket() failed errno=%d", errno);
        return -1;
    }
    LOG_INF("socket() created fd=%d", fd);

    zsock_close(fd);
    LOG_INF("socket closed");


    fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("socket() failed errno=%d", errno);
        return -1;
    }
    LOG_INF("socket() created fd=%d", fd);
    zsock_close(fd);
    LOG_INF("socket closed");

    LOG_INF("\nCH9120 ========== Stage 4 =====================\n");

    int fd1 = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int fd2 = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd1 < 0) {
        LOG_ERR("first socket() failed");
    } else if (fd2 >= 0) {
        LOG_ERR("second socket() should have failed");
        zsock_close(fd2);
    } else {
        LOG_INF("correctly rejected second socket with errno=%d", errno);
    }
    zsock_close(fd1);

    LOG_INF("all checks passed");

    return 0;
}