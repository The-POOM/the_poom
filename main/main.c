#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "i2c.h"
#include "poom_sbus.h"
#include "button_driver.h"
#include "nvs_flash.h"

void app_main(void)
{
	  const TickType_t xDelay = 500 / portTICK_PERIOD_MS;
    i2c_init();
    i2c_unlock(); 
    poom_sbus_init(); 
    poom_sbus_start(4, 4096);
    button_module_begin();
    while(1){
		  vTaskDelay( xDelay );
    }

}
