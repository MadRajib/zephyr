#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);
#define SERVER_IP    "192.168.1.19"
#define SERVER_PORT  8080

int main(void)
{
    int ret;
    int fd;
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

    /* Stage 3 — open socket */
    // LOG_INF("\nCH9120 ========== Stage 3 =====================\n");
    // int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // if (fd < 0) {
    //     LOG_ERR("socket() failed errno=%d", errno);
    //     return -1;
    // }
    // LOG_INF("socket() created fd=%d", fd);

    // zsock_close(fd);
    // LOG_INF("socket closed");


    // fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // if (fd < 0) {
    //     LOG_ERR("socket() failed errno=%d", errno);
    //     return -1;
    // }
    // LOG_INF("socket() created fd=%d", fd);
    // zsock_close(fd);
    // LOG_INF("socket closed");

    // LOG_INF("\nCH9120 ========== Stage 4 =====================\n");

    // int fd1 = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // int fd2 = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    // if (fd1 < 0) {
    //     LOG_ERR("first socket() failed");
    // } else if (fd2 >= 0) {
    //     LOG_ERR("second socket() should have failed");
    //     zsock_close(fd2);
    // } else {
    //     LOG_INF("correctly rejected second socket with errno=%d", errno);
    // }
    // zsock_close(fd1);

    // Stage 5
    LOG_INF("\nCH9120 ========== Stage 5 =====================\n");

    fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("socket() failed errno=%d", errno);
        return -1;
    }
    LOG_INF("socket created fd=%d", fd);
    

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    ret = zsock_inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
    if (ret != 1) {
        LOG_ERR("invalid IP address");
        zsock_close(fd);
        return -1;
    }

    /* connect */
    LOG_INF("connecting to %s:%d ...", SERVER_IP, SERVER_PORT);
    ret = zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        LOG_ERR("connect() failed errno=%d", errno);
        zsock_close(fd);
        return -1;
    }
    LOG_INF("connected");

    const char *msg = "hello from CH9120\n";
    ret = zsock_send(fd, msg, strlen(msg), 0);
    if (ret < 0) {
        LOG_ERR("send() failed errno=%d", errno);
    } else {
        LOG_INF("sent %d bytes", ret);
    }

    char rx_buf[64];

    LOG_INF("waiting for data from server...");

    while (1) {
        memset(rx_buf, 0, sizeof(rx_buf));

        ret = zsock_recv(fd, rx_buf, sizeof(rx_buf) - 1, 0);
        if (ret < 0) {
            LOG_ERR("recv() failed errno=%d", errno);
            break;
        }

        if (ret == 0) {
            /* server closed connection */
            LOG_INF("server disconnected");
            break;
        }

        LOG_INF("received %d bytes: %s", ret, rx_buf);
    }

    zsock_close(fd);
    LOG_INF("socket closed");

    LOG_INF("all checks passed");

    return 0;
}