#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t ts_ms; uint16_t len; uint8_t data[128]; char topic[32]; } poom_sbus_msg_t;
typedef void (*poom_sbus_handler_t)(const poom_sbus_msg_t *msg,void *user);
bool poom_sbus_subscribe_cb(const char *topic,poom_sbus_handler_t handler,void *user);
bool poom_sbus_unsubscribe_cb(const char *topic,poom_sbus_handler_t handler,void *user);
bool poom_sbus_publish(const char *topic,const void *data,size_t length,uint32_t flags);
