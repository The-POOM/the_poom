#pragma once
#include "FreeRTOS.h"
void vTaskDelay(unsigned);
void vTaskDelete(void *);
unsigned uxTaskGetStackHighWaterMark(void *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);


typedef void *TaskHandle_t;
