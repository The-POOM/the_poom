#pragma once
int printf(const char *, ...);
#define ESP_LOGI(tag, fmt, ...) printf("%s " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("%s " fmt, tag, ##__VA_ARGS__)
