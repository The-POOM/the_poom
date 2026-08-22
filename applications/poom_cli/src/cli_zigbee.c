#include "cli_zigbee.h"

#include "zb_cli.h"

static bool s_cli_poom_zigbee_running = false;

void cli_poom_zigbee_begin(void)
{
    if (s_cli_poom_zigbee_running)
    {
        return;
    }

    zb_cli_begin();
    s_cli_poom_zigbee_running = true;
}

void cli_poom_zigbee_stop(void)
{
    if (!s_cli_poom_zigbee_running)
    {
        return;
    }

    zb_cli_stop();
    s_cli_poom_zigbee_running = false;
}

bool cli_poom_zigbee_is_running(void)
{
    return s_cli_poom_zigbee_running;
}
