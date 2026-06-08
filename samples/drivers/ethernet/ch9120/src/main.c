#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Starting CH9120 Quick Test!");
    LOG_INF("Waiting for network interface...");
    k_sleep(K_SECONDS(5));
    LOG_INF("Done. If the driver init didn't crash, you are good to go!");
    return 0;
}