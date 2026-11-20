#pragma once
#include <stdint.h>
#include <stddef.h>
uint16_t htons(uint16_t value);
uint16_t ntohs(uint16_t value);
uint32_t htonl(uint32_t value);
uint32_t ntohl(uint32_t value);
int inet_pton(int family, const char *text, void *out);
const char *inet_ntop(int family, const void *address, char *out, size_t length);
