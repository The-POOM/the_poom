#include "oled_driver.h"

#include <string.h>

#include "oled_transport.h"

void oled_driver_init(oled_driver_t *dev, int width, int height)
{
    if (dev == NULL)
    {
        return;
    }

    oled_transport_init(dev, width, height);

    // Keep legacy page buffers cleared for any caller that inspects them.
    for (int page = 0; page < dev->_pages; page++)
    {
        (void)memset(dev->_page[page]._segs, 0, (size_t)dev->_width);
    }
}
