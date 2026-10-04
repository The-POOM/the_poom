// SPDX-License-Identifier: MIT
#ifndef CAPTIVE_CLIENT_IDENTITY_H
#define CAPTIVE_CLIENT_IDENTITY_H

#include <stddef.h>
#include <string.h>

/* Evidence from the browser is a hint, never a verified model or owner name.
 * Specific platforms must precede generic Linux/Mac compatibility tokens. */
static inline const char *captive_client_type(const char *ua)
{
    if(ua == NULL) return "";
    if(strstr(ua, "iPhone") != NULL) return "iPhone";
    if(strstr(ua, "iPad") != NULL) return "iPad";
    if(strstr(ua, "iPod") != NULL) return "iPod";
    if(strstr(ua, "Android") != NULL) return "Android";
    if(strstr(ua, "CrOS") != NULL) return "ChromeOS";
    if(strstr(ua, "Windows") != NULL) return "Windows";
    if((strstr(ua, "Macintosh") != NULL) || (strstr(ua, "Mac OS X") != NULL)) return "Mac";
    if(strstr(ua, "Linux") != NULL) return "Linux";
    return "";
}

/* The OLED uses a byte font: never pass controls or UTF-8 bytes to it. */
static inline void captive_client_copy_name(char *out, size_t capacity,
                                             const char *name, size_t length)
{
    if(capacity == 0U) return;
    size_t i = 0U;
    while((i + 1U < capacity) && (i < length) && (name[i] != '\0'))
    {
        unsigned char c = (unsigned char)name[i];
        out[i++] = ((c >= 32U) && (c <= 126U)) ? (char)c : '?';
    }
    out[i] = '\0';
}

#endif
