// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_SBUS_H
#define POOM_SBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_SBUS_MAX_TOPICS 16
#define POOM_SBUS_TOPIC_NAME_MAX 32
#define POOM_SBUS_MSG_DATA_SIZE 128
#define POOM_SBUS_MAX_HANDLERS_PER_TOPIC 12
#define POOM_SBUS_DISPATCH_QUEUE_DEPTH 32

#define SBUS_MAX_TOPICS POOM_SBUS_MAX_TOPICS
#define SBUS_TOPIC_NAME_MAX POOM_SBUS_TOPIC_NAME_MAX
#define SBUS_MSG_DATA_SIZE POOM_SBUS_MSG_DATA_SIZE
#define SBUS_MAX_HANDLERS_PER_TOPIC POOM_SBUS_MAX_HANDLERS_PER_TOPIC
#define SBUS_DISPATCH_QUEUE_DEPTH POOM_SBUS_DISPATCH_QUEUE_DEPTH

/**
 * @brief Defines one message delivered by the internal bus.
 */
typedef struct
{
    uint32_t ts_ms;
    uint16_t len;
    uint8_t data[POOM_SBUS_MSG_DATA_SIZE];
    char topic[POOM_SBUS_TOPIC_NAME_MAX];
} poom_sbus_msg_t;

/**
 * @brief Defines the callback signature for topic subscribers.
 */
typedef void (*poom_sbus_handler_t)(const poom_sbus_msg_t *msg, void *user);

/**
 * @brief Initializes internal resources for the bus.
 * @return void
 */
void poom_sbus_init(void);

/**
 * @brief Starts the internal dispatcher task and queue.
 * @param[in] task_priority Dispatcher task priority.
 * @param[in] task_stack_words Dispatcher stack size in words.
 * @return bool
 */
bool poom_sbus_start(UBaseType_t task_priority, uint32_t task_stack_words);

/**
 * @brief Registers a topic name if it does not exist.
 * @param[in] name Topic name.
 * @return bool
 */
bool poom_sbus_register_topic(const char *name);

/**
 * @brief Subscribes a callback to a topic.
 * @param[in] topic Topic name.
 * @param[in] cb Callback handler.
 * @param[in,out] user Opaque user context.
 * @return bool
 */
bool poom_sbus_subscribe_cb(const char *topic, poom_sbus_handler_t cb, void *user);

/**
 * @brief Unsubscribes a callback from a topic.
 * @param[in] topic Topic name.
 * @param[in] cb Callback handler.
 * @param[in,out] user Opaque user context.
 * @return bool
 */
bool poom_sbus_unsubscribe_cb(const char *topic, poom_sbus_handler_t cb, void *user);

/**
 * @brief Publishes a message to a topic.
 * @param[in] topic Topic name.
 * @param[in] data Payload buffer.
 * @param[in] len Payload length.
 * @param[in] to Queue send timeout in ticks.
 * @return bool
 */
bool poom_sbus_publish(const char *topic, const void *data, size_t len, TickType_t to);

#ifdef __cplusplus
}
#endif

#endif
