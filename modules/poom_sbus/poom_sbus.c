// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_sbus.h"

#include <string.h>

#include "freertos/task.h"

typedef struct
{
    char name[POOM_SBUS_TOPIC_NAME_MAX];
    struct
    {
        poom_sbus_handler_t cb;
        void *user;
    } handlers[POOM_SBUS_MAX_HANDLERS_PER_TOPIC];
    uint8_t count;
    bool used;
} poom_sbus_topic_slot_t;

static poom_sbus_topic_slot_t s_poom_sbus_topics[POOM_SBUS_MAX_TOPICS];
static SemaphoreHandle_t s_poom_sbus_lock = NULL;
static QueueHandle_t s_poom_sbus_dispatch_q = NULL;
static TaskHandle_t s_poom_sbus_dispatch_task = NULL;

/**
 * @brief Returns the current tick timestamp in milliseconds.
 * @return uint32_t
 */
static uint32_t poom_sbus_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief Acquires the module mutex.
 * @return bool
 */
static bool poom_sbus_lock_(void)
{
    if(s_poom_sbus_lock == NULL)
    {
        return false;
    }
    return (xSemaphoreTake(s_poom_sbus_lock, portMAX_DELAY) == pdTRUE);
}

/**
 * @brief Releases the module mutex.
 * @return void
 */
static void poom_sbus_unlock_(void)
{
    if(s_poom_sbus_lock != NULL)
    {
        (void)xSemaphoreGive(s_poom_sbus_lock);
    }
}

/**
 * @brief Finds the index of a registered topic.
 * @param[in] name Topic name.
 * @return int
 */
static int poom_sbus_find_topic_idx_(const char *name)
{
    for(int i = 0; i < POOM_SBUS_MAX_TOPICS; i++)
    {
        if(s_poom_sbus_topics[i].used && (strncmp(s_poom_sbus_topics[i].name, name, POOM_SBUS_TOPIC_NAME_MAX) == 0))
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Allocates a new topic slot.
 * @param[in] name Topic name.
 * @return int
 */
static int poom_sbus_alloc_topic_idx_(const char *name)
{
    for(int i = 0; i < POOM_SBUS_MAX_TOPICS; i++)
    {
        if(!s_poom_sbus_topics[i].used)
        {
            memset(&s_poom_sbus_topics[i], 0, sizeof(s_poom_sbus_topics[i]));
            strncpy(s_poom_sbus_topics[i].name, name, POOM_SBUS_TOPIC_NAME_MAX - 1);
            s_poom_sbus_topics[i].name[POOM_SBUS_TOPIC_NAME_MAX - 1] = '\0';
            s_poom_sbus_topics[i].used = true;
            return i;
        }
    }
    return -1;
}

/**
 * @brief Runs the internal queue dispatcher loop.
 * @param[in,out] arg Not used.
 * @return void
 */
static void poom_sbus_dispatcher_task_(void *arg)
{
    poom_sbus_msg_t msg;

    (void)arg;

    for(;;)
    {
        if(xQueueReceive(s_poom_sbus_dispatch_q, &msg, portMAX_DELAY) == pdTRUE)
        {
            poom_sbus_handler_t callbacks[POOM_SBUS_MAX_HANDLERS_PER_TOPIC];
            void *users[POOM_SBUS_MAX_HANDLERS_PER_TOPIC];
            uint8_t count = 0;

            if(poom_sbus_lock_())
            {
                const int topic_idx = poom_sbus_find_topic_idx_(msg.topic);
                if(topic_idx >= 0)
                {
                    count = s_poom_sbus_topics[topic_idx].count;
                    for(uint8_t i = 0; i < count; i++)
                    {
                        callbacks[i] = s_poom_sbus_topics[topic_idx].handlers[i].cb;
                        users[i] = s_poom_sbus_topics[topic_idx].handlers[i].user;
                    }
                }
                poom_sbus_unlock_();
            }

            for(uint8_t i = 0; i < count; i++)
            {
                if(callbacks[i] != NULL)
                {
                    callbacks[i](&msg, users[i]);
                    taskYIELD();
                }
            }
        }
    }
}

/**
 * @brief Initializes internal resources for the bus.
 * @return void
 */
void poom_sbus_init(void)
{
    memset(s_poom_sbus_topics, 0, sizeof(s_poom_sbus_topics));

    if(s_poom_sbus_lock == NULL)
    {
        s_poom_sbus_lock = xSemaphoreCreateMutex();
    }
}

/**
 * @brief Starts the internal dispatcher task and queue.
 * @param[in] task_priority Dispatcher task priority.
 * @param[in] task_stack_words Dispatcher stack size in words.
 * @return bool
 */
bool poom_sbus_start(UBaseType_t task_priority, uint32_t task_stack_words)
{
    if(s_poom_sbus_lock == NULL)
    {
        poom_sbus_init();
    }

    if(s_poom_sbus_lock == NULL)
    {
        return false;
    }

    if(s_poom_sbus_dispatch_q == NULL)
    {
        s_poom_sbus_dispatch_q = xQueueCreate(POOM_SBUS_DISPATCH_QUEUE_DEPTH, sizeof(poom_sbus_msg_t));
        if(s_poom_sbus_dispatch_q == NULL)
        {
            return false;
        }
    }

    if(s_poom_sbus_dispatch_task == NULL)
    {
        const BaseType_t created = xTaskCreate(
            poom_sbus_dispatcher_task_,
            "poom_sbus_dispatch",
            task_stack_words,
            NULL,
            task_priority,
            &s_poom_sbus_dispatch_task);

        if(created != pdPASS)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Registers a topic name if it does not exist.
 * @param[in] name Topic name.
 * @return bool
 */
bool poom_sbus_register_topic(const char *name)
{
    int topic_idx;

    if((name == NULL) || (*name == '\0'))
    {
        return false;
    }

    if(s_poom_sbus_lock == NULL)
    {
        poom_sbus_init();
    }

    if(!poom_sbus_lock_())
    {
        return false;
    }

    topic_idx = poom_sbus_find_topic_idx_(name);
    if(topic_idx < 0)
    {
        topic_idx = poom_sbus_alloc_topic_idx_(name);
    }

    poom_sbus_unlock_();
    return (topic_idx >= 0);
}

/**
 * @brief Subscribes a callback to a topic.
 * @param[in] topic Topic name.
 * @param[in] cb Callback handler.
 * @param[in,out] user Opaque user context.
 * @return bool
 */
bool poom_sbus_subscribe_cb(const char *topic, poom_sbus_handler_t cb, void *user)
{
    int topic_idx;

    if((topic == NULL) || (cb == NULL))
    {
        return false;
    }

    if(s_poom_sbus_lock == NULL)
    {
        poom_sbus_init();
    }

    if(!poom_sbus_lock_())
    {
        return false;
    }

    topic_idx = poom_sbus_find_topic_idx_(topic);
    if(topic_idx < 0)
    {
        topic_idx = poom_sbus_alloc_topic_idx_(topic);
    }
    if(topic_idx < 0)
    {
        poom_sbus_unlock_();
        return false;
    }

    for(uint8_t i = 0; i < s_poom_sbus_topics[topic_idx].count; i++)
    {
        if((s_poom_sbus_topics[topic_idx].handlers[i].cb == cb) &&
           (s_poom_sbus_topics[topic_idx].handlers[i].user == user))
        {
            poom_sbus_unlock_();
            return true;
        }
    }

    if(s_poom_sbus_topics[topic_idx].count >= POOM_SBUS_MAX_HANDLERS_PER_TOPIC)
    {
        poom_sbus_unlock_();
        return false;
    }

    s_poom_sbus_topics[topic_idx].handlers[s_poom_sbus_topics[topic_idx].count].cb = cb;
    s_poom_sbus_topics[topic_idx].handlers[s_poom_sbus_topics[topic_idx].count].user = user;
    s_poom_sbus_topics[topic_idx].count++;

    poom_sbus_unlock_();
    return true;
}

/**
 * @brief Unsubscribes a callback from a topic.
 * @param[in] topic Topic name.
 * @param[in] cb Callback handler.
 * @param[in,out] user Opaque user context.
 * @return bool
 */
bool poom_sbus_unsubscribe_cb(const char *topic, poom_sbus_handler_t cb, void *user)
{
    int topic_idx;

    if((topic == NULL) || (cb == NULL))
    {
        return false;
    }

    if(s_poom_sbus_lock == NULL)
    {
        poom_sbus_init();
    }

    if(!poom_sbus_lock_())
    {
        return false;
    }

    topic_idx = poom_sbus_find_topic_idx_(topic);
    if(topic_idx < 0)
    {
        poom_sbus_unlock_();
        return false;
    }

    for(uint8_t i = 0; i < s_poom_sbus_topics[topic_idx].count; i++)
    {
        if((s_poom_sbus_topics[topic_idx].handlers[i].cb == cb) &&
           (s_poom_sbus_topics[topic_idx].handlers[i].user == user))
        {
            for(uint8_t j = i + 1; j < s_poom_sbus_topics[topic_idx].count; j++)
            {
                s_poom_sbus_topics[topic_idx].handlers[j - 1] = s_poom_sbus_topics[topic_idx].handlers[j];
            }
            s_poom_sbus_topics[topic_idx].count--;
            break;
        }
    }

    poom_sbus_unlock_();
    return true;
}

/**
 * @brief Publishes a message to a topic.
 * @param[in] topic Topic name.
 * @param[in] data Payload buffer.
 * @param[in] len Payload length.
 * @param[in] to Queue send timeout in ticks.
 * @return bool
 */
bool poom_sbus_publish(const char *topic, const void *data, size_t len, TickType_t to)
{
    poom_sbus_msg_t msg = {0};

    if((topic == NULL) || (data == NULL) || (s_poom_sbus_dispatch_q == NULL))
    {
        return false;
    }

    if(len > POOM_SBUS_MSG_DATA_SIZE)
    {
        len = POOM_SBUS_MSG_DATA_SIZE;
    }

    msg.ts_ms = poom_sbus_now_ms_();
    msg.len = (uint16_t)len;
    strncpy(msg.topic, topic, POOM_SBUS_TOPIC_NAME_MAX - 1);
    msg.topic[POOM_SBUS_TOPIC_NAME_MAX - 1] = '\0';
    memcpy(msg.data, data, len);

    return (xQueueSend(s_poom_sbus_dispatch_q, &msg, to) == pdPASS);
}
