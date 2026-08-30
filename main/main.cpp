#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "poom_sbus.h"
#include "button_driver.h"
#include "nvs_flash.h"
#include "poom_boot_policy.h"
#include "poom_menu.h"
#if CONFIG_ZB_ENABLED
#include "zb_cli.h"
#endif
//#include "app_nfc.h"
#include "cli_nfc.h"
static char** nodes_arr = NULL;

#if CONFIG_ZB_ENABLED
__attribute__((weak))
void esp_zb_app_signal_handler(esp_zb_app_signal_t* signal_struct)
{
    (void)signal_struct;
    printf("fake \n\r");
}
#endif



extern "C" void app_main(void)
{
    esp_err_t ret;
    uint8_t* found_addresses = NULL;
    size_t num_addresses = 0;
	const TickType_t xDelay = 500 / portTICK_PERIOD_MS;
    (void)poom_boot_policy_init();
    (void)poom_boot_policy_apply_startup_policy();
    i2c_init();
    i2c_unlock(); 
    poom_sbus_init(); 
    poom_sbus_start(4, 4096);
    button_module_begin();
    app_poom_menu_principal();
    while(1){
		vTaskDelay( xDelay );

	}

}
