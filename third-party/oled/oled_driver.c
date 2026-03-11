#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "font8x8_basic.h"
#include "oled_driver.h"
#include "oled_transport.h"

static const char *OLED_DRIVER_TAG = "oled_driver";

#if defined(CONFIG_OLED_DRIVER_ENABLE_LOG) && CONFIG_OLED_DRIVER_ENABLE_LOG

#define OLED_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", OLED_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define OLED_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", OLED_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define OLED_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", OLED_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define OLED_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", OLED_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define OLED_PRINTF_E(...)
#define OLED_PRINTF_W(...)
#define OLED_PRINTF_I(...)
#define OLED_PRINTF_D(...)

#endif

#define OLED_MAX_TEXT_LEN       (16)
#define OLED_MAX_TEXT_BUF_LEN   (OLED_MAX_TEXT_LEN + 1)
#define FONT_WIDTH              (8)

static void oled_driver_display_image_(oled_driver_t *dev,
                                       int page,
                                       int seg,
                                       const uint8_t *images,
                                       int width);
static void oled_driver_line_(oled_driver_t *dev,
                              int x1,
                              int y1,
                              int x2,
                              int y2,
                              bool invert);
static void oled_driver_invert_(uint8_t *buf, size_t blen);
static void oled_driver_flip_(uint8_t *buf, size_t blen);
static uint8_t oled_driver_copy_bit_(uint8_t src, int src_bits, uint8_t dst, int dst_bits);
static uint8_t oled_driver_rotate_byte_(uint8_t value);

void oled_driver_init(oled_driver_t* dev, int width, int height)
{
    oled_transport_init(dev, width, height);

    for(int i = 0; i < dev->_pages; i++)
    {
        memset(dev->_page[i]._segs, 0, width);
    }
}

void oled_driver_show_buffer(oled_driver_t* dev)
{

    for(int page = 0; page < dev->_pages; page++)
    {
        oled_transport_display_image(dev, page, 0, dev->_page[page]._segs,
                            dev->_width);
    }

}

void oled_driver_set_buffer(oled_driver_t* dev, uint8_t* buffer)
{
    int index = 0;
    for(int page = 0; page < dev->_pages; page++)
    {
        memcpy(&dev->_page[page]._segs, &buffer[index], dev->_width);
        index += dev->_width;
    }
}

void oled_driver_get_buffer(oled_driver_t* dev, uint8_t* buffer)
{
    int index = 0;
    for(int page = 0; page < dev->_pages; page++)
    {
        memcpy(&buffer[index], &dev->_page[page]._segs, dev->_width);
        index += dev->_width;
    }
}

static void oled_driver_display_image_(oled_driver_t *dev,
                                       int page,
                                       int seg,
                                       const uint8_t *images,
                                       int width)
{
    oled_transport_display_image(dev, page, seg, images, width);

    memcpy(&dev->_page[page]._segs[seg], images, width);
}

void oled_driver_display_text(oled_driver_t* dev,
                              int page,
                              char* text,
                              int x,
                              bool invert)
{
    if(page >= dev->_pages)
        return;
    int _text_len = strlen(text);
    if(_text_len > OLED_MAX_TEXT_LEN)
        _text_len = OLED_MAX_TEXT_LEN;

    uint8_t seg = x;
    uint8_t image[FONT_WIDTH];
    for(uint8_t i = 0; i < _text_len; i++)
    {
        memcpy(image, font8x8_basic_tr[(uint8_t)text[i]], FONT_WIDTH);
        if(invert)
            oled_driver_invert_(image, FONT_WIDTH);
        if(dev->_flip)
            oled_driver_flip_(image, FONT_WIDTH);
        oled_driver_display_image_(dev, page, seg, image, FONT_WIDTH);
        seg = seg + FONT_WIDTH;
    }
}


void oled_driver_clear_buffer(oled_driver_t* dev)
{
    for(int i = 0; i < dev->_pages; i++)
    {
        memset(dev->_page[i]._segs, 0, dev->_width);
    }
}

void oled_driver_clear_screen(oled_driver_t* dev, bool invert)
{
    (void)invert;
    oled_driver_clear_buffer(dev);
    oled_driver_show_buffer(dev);
}

void oled_driver_clear_line(oled_driver_t* dev, int page, int length, bool invert)
{
    if((length <= 0) || (length > OLED_MAX_TEXT_LEN))
    {
        return;
    }

    char buffer[OLED_MAX_TEXT_BUF_LEN];
    memset(buffer, ' ', length);
    buffer[length] = '\0';

    oled_driver_display_text(dev, page, buffer, length, invert);
}

void oled_driver_bitmaps(oled_driver_t* dev,
                         int xpos,
                         int ypos,
                         const uint8_t* bitmap,
                         int width,
                         int height,
                         bool invert)
{
    if((width % FONT_WIDTH) != 0)
    {
        OLED_PRINTF_E("width must be a multiple of FONT_WIDTH");
        return;
    }

    int _width = width / FONT_WIDTH;
    uint8_t wk0;
    uint8_t wk1;
    uint8_t wk2;
    uint8_t page    = (ypos / FONT_WIDTH);
    uint8_t _seg    = xpos;
    uint8_t dstBits = (ypos % FONT_WIDTH);
    OLED_PRINTF_D("ypos=%d page=%d dstBits=%d", ypos, page, dstBits);
    int offset = 0;
    for(int _height = 0; _height < height; _height++)
    {
        for(int index = 0; index < _width; index++)
        {
            for(int srcBits = 7; srcBits >= 0; srcBits--)
            {
                wk0 = dev->_page[page]._segs[_seg];
                if(dev->_flip)
                    wk0 = oled_driver_rotate_byte_(wk0);

                wk1 = bitmap[index + offset];
                if(invert)
                    wk1 = ~wk1;



                wk2 = oled_driver_copy_bit_(wk1, srcBits, wk0, dstBits);
                if(dev->_flip)
                    wk2 = oled_driver_rotate_byte_(wk2);

                OLED_PRINTF_D("index=%d offset=%d page=%d _seg=%d, wk2=%02x",
                           index, offset, page, _seg, wk2);
                dev->_page[page]._segs[_seg] = wk2;
                _seg++;
            }
        }
        offset = offset + _width;
        dstBits++;
        _seg = xpos;
        if(dstBits == FONT_WIDTH)
        {
            page++;
            dstBits = 0;
        }
    }

    oled_driver_show_buffer(dev);
}


void oled_driver_draw_pixel(oled_driver_t* dev, int xpos, int ypos, bool invert)
{
    uint8_t _page = (ypos / FONT_WIDTH);
    uint8_t _bits = (ypos % FONT_WIDTH);
    uint8_t _seg  = xpos;
    uint8_t wk0   = dev->_page[_page]._segs[_seg];
    uint8_t wk1   = 1 << _bits;
    OLED_PRINTF_D("ypos=%d _page=%d _bits=%d wk0=0x%02x wk1=0x%02x", ypos, _page,
               _bits, wk0, wk1);
    if(invert)
    {
        wk0 = wk0 & ~wk1;
    }
    else
    {
        wk0 = wk0 | wk1;
    }
    if(dev->_flip)
        wk0 = oled_driver_rotate_byte_(wk0);
    OLED_PRINTF_D("wk0=0x%02x wk1=0x%02x", wk0, wk1);
    dev->_page[_page]._segs[_seg] = wk0;
}


static void oled_driver_line_(oled_driver_t* dev,
                              int x1,
                              int y1,
                              int x2,
                              int y2,
                              bool invert)
{
    int i;
    int dx, dy;
    int sx, sy;
    int E;

    /* distance between two points */
    dx = (x2 > x1) ? x2 - x1 : x1 - x2;
    dy = (y2 > y1) ? y2 - y1 : y1 - y2;

    /* direction of two point */
    sx = (x2 > x1) ? 1 : -1;
    sy = (y2 > y1) ? 1 : -1;

    /* inclination < 1 */
    if(dx > dy)
    {
        E = -dx;
        for(i = 0; i <= dx; i++)
        {
            oled_driver_draw_pixel(dev, x1, y1, invert);
            x1 += sx;
            E += 2 * dy;
            if(E >= 0)
            {
                y1 += sy;
                E -= 2 * dx;
            }
        }

        /* inclination >= 1 */
    }
    else
    {
        E = -dy;
        for(i = 0; i <= dy; i++)
        {
            oled_driver_draw_pixel(dev, x1, y1, invert);
            y1 += sy;
            E += 2 * dx;
            if(E >= 0)
            {
                x1 += sx;
                E -= 2 * dy;
            }
        }
    }
}

static void oled_driver_invert_(uint8_t *buf, size_t blen)
{
    uint8_t wk;
    for(int i = 0; i < blen; i++)
    {
        wk     = buf[i];
        buf[i] = ~wk;
    }
}


static void oled_driver_flip_(uint8_t *buf, size_t blen)
{
    for(int i = 0; i < blen; i++)
    {
        buf[i] = oled_driver_rotate_byte_(buf[i]);
    }
}

static uint8_t oled_driver_copy_bit_(uint8_t src, int srcBits, uint8_t dst, int dstBits)
{
    OLED_PRINTF_D("src=%02x srcBits=%d dst=%02x dstBits=%d", src, srcBits, dst,
               dstBits);
    uint8_t smask = 0x01 << srcBits;
    uint8_t dmask = 0x01 << dstBits;
    uint8_t _src  = src & smask;
    uint8_t _dst;
    if(_src != 0)
    {
        _dst = dst | dmask;
    }
    else
    {
        _dst = dst & ~(dmask);
    }
    return _dst;
}



static uint8_t oled_driver_rotate_byte_(uint8_t ch1)
{
    uint8_t ch2 = 0;
    for(int j = 0; j < FONT_WIDTH; j++)
    {
        ch2 = (ch2 << 1) + (ch1 & 0x01);
        ch1 = ch1 >> 1;
    }
    return ch2;
}

void oled_driver_draw_line(oled_driver_t* dev,
                           int x1,
                           int y1,
                           int x2,
                           int y2,
                           bool invert)
{
    oled_driver_line_(dev, x1, y1, x2, y2, invert);
}

void oled_driver_draw_hline(oled_driver_t* dev,
                            int x,
                            int y,
                            int width,
                            bool invert)
{
    for(int i = 0; i < width; i++)
    {
        oled_driver_draw_pixel(dev, x + i, y, invert);
    }
}

void oled_driver_draw_vline(oled_driver_t* dev,
                            int x,
                            int y,
                            int height,
                            bool invert)
{
    for(int i = 0; i < height; i++)
    {
        oled_driver_draw_pixel(dev, x, y + i, invert);
    }
}

void oled_driver_draw_round_rect(oled_driver_t* dev,
                                 int x, int y,
                                 int width, int height,
                                 int r,
                                 bool invert)
{
    if (width <= 0 || height <= 0) return;
    if (r < 0) r = 0;
    int maxr = (width < height ? width : height) / 2;
    if (r > maxr) r = maxr;

    if (r == 0) {
        oled_driver_draw_rect(dev, x, y, width, height, invert);
        return;
    }

    int cx = x + r;
    int cy = y + r;
    int cx2 = x + width - 1 - r;
    int cy2 = y + height - 1 - r;

    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int px = 0;
    int py = r;

    oled_driver_draw_pixel(dev, cx     , cy - r, invert);
    oled_driver_draw_pixel(dev, cx - r , cy    , invert);
    oled_driver_draw_pixel(dev, cx2    , cy - r, invert);
    oled_driver_draw_pixel(dev, cx2 + r, cy    , invert);
    oled_driver_draw_pixel(dev, cx2    , cy2 + r, invert);
    oled_driver_draw_pixel(dev, cx2 + r, cy2    , invert);
    oled_driver_draw_pixel(dev, cx     , cy2 + r, invert);
    oled_driver_draw_pixel(dev, cx - r , cy2    , invert);

    while (px < py) {
        if (f >= 0) {
            py--;
            ddF_y += 2;
            f += ddF_y;
        }
        px++;
        ddF_x += 2;
        f += ddF_x;

        oled_driver_draw_pixel(dev, cx - px, cy - py, invert);
        oled_driver_draw_pixel(dev, cx - py, cy - px, invert);

        oled_driver_draw_pixel(dev, cx2 + px, cy - py, invert);
        oled_driver_draw_pixel(dev, cx2 + py, cy - px, invert);

        oled_driver_draw_pixel(dev, cx2 + px, cy2 + py, invert);
        oled_driver_draw_pixel(dev, cx2 + py, cy2 + px, invert);

        oled_driver_draw_pixel(dev, cx - px, cy2 + py, invert);
        oled_driver_draw_pixel(dev, cx - py, cy2 + px, invert);
    }

    oled_driver_draw_hline(dev, x + r, y, width - 2*r, invert);
    oled_driver_draw_hline(dev, x + r, y + height - 1, width - 2*r, invert);
    oled_driver_draw_vline(dev, x, y + r, height - 2*r, invert);
    oled_driver_draw_vline(dev, x + width - 1, y + r, height - 2*r, invert);
}


void oled_driver_draw_rect(oled_driver_t* dev,
                           int x,
                           int y,
                           int width,
                           int height,
                           bool invert)
{
    oled_driver_draw_hline(dev, x, y, width, invert);
    oled_driver_draw_hline(dev, x, y + height - 1, width, invert);
    oled_driver_draw_vline(dev, x, y, height, invert);
    oled_driver_draw_vline(dev, x + width - 1, y, height, invert);
}

void oled_driver_draw_custom_box(oled_driver_t* dev)
{
    int page   = 3;
    int x      = 0;
    int y      = page * FONT_WIDTH - 3;
    int width  = x + dev->_width - 4;
    int height = y - 6;

    oled_driver_draw_rect(dev, x, y, width, height, 0);
    oled_driver_draw_rect(dev, x, y, width - 1, height - 1, 0);


    oled_driver_draw_pixel(dev, x, y, 1);
    oled_driver_draw_pixel(dev, x + 1, y, 1);
    oled_driver_draw_pixel(dev, x, y + 1, 1);
    oled_driver_draw_pixel(dev, x + 1, y + 1, 0);


    oled_driver_draw_pixel(dev, width - 1, y, 1);
    oled_driver_draw_pixel(dev, width - 2, y, 1);
    oled_driver_draw_pixel(dev, width - 1, y + 1, 1);
    oled_driver_draw_pixel(dev, width - 2, y + 1, 0);


    oled_driver_draw_pixel(dev, x, y + height - 1, 1);
    oled_driver_draw_pixel(dev, x + 1, y + height - 1, 1);
    oled_driver_draw_pixel(dev, x, y + height - 2, 1);
    oled_driver_draw_pixel(dev, x + 1, y + height - 2, 0);


    oled_driver_draw_pixel(dev, width - 1, y + height - 1, 1);
    oled_driver_draw_pixel(dev, width - 2, y + height - 1, 1);
    oled_driver_draw_pixel(dev, width - 1, y + height - 2, 1);
    oled_driver_draw_pixel(dev, width - 2, y + height - 2, 0);

}

void oled_driver_draw_modal_box(oled_driver_t* dev, int pos_x, int modal_height)
{
    (void)modal_height;
#ifdef CONFIG_RESOLUTION_128X64
    int initial_page  = 2;
    int height_offset = 35;
    int y_offset      = 3;
#else
    int initial_page  = 1;
    int height_offset = 18;
    int y_offset      = 1;
#endif
    int page   = initial_page;
    int x      = pos_x;
    int y      = page * FONT_WIDTH - y_offset;
    int width  = x + dev->_width - 3;
    int height = y + height_offset;

    oled_driver_draw_rect(dev, x, y, width, height, 0);
    oled_driver_draw_rect(dev, x, y, width - 1, height - 1, 0);


    oled_driver_draw_pixel(dev, x, y, 1);
    oled_driver_draw_pixel(dev, x + 1, y, 1);
    oled_driver_draw_pixel(dev, x, y + 1, 1);
    oled_driver_draw_pixel(dev, x + 1, y + 1, 0);


    oled_driver_draw_pixel(dev, width - 1, y, 1);
    oled_driver_draw_pixel(dev, width - 2, y, 1);
    oled_driver_draw_pixel(dev, width - 1, y + 1, 1);
    oled_driver_draw_pixel(dev, width - 2, y + 1, 0);


    oled_driver_draw_pixel(dev, x, y + height - 1, 1);
    oled_driver_draw_pixel(dev, x + 1, y + height - 1, 1);
    oled_driver_draw_pixel(dev, x, y + height - 2, 1);
    oled_driver_draw_pixel(dev, x + 1, y + height - 2, 0);


    oled_driver_draw_pixel(dev, width - 1, y + height - 1, 1);
    oled_driver_draw_pixel(dev, width - 2, y + height - 1, 1);
    oled_driver_draw_pixel(dev, width - 1, y + height - 2, 1);
    oled_driver_draw_pixel(dev, width - 2, y + height - 2, 0);

}
