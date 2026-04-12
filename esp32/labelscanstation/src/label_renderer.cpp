// Label renderer using stb_truetype for smooth TTF font rendering
// Text is rotated 90° CCW so it reads lengthwise on the 29x90mm label

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "label_renderer.h"
#include "app_config.h"
#include "i3logo.h"
#include "roboto_bold.h"

#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"

static const char *TAG = "label_renderer";

static uint8_t s_framebuffer[LABEL_FB_SIZE];
static stbtt_fontinfo s_font;
static bool s_font_ready = false;

// Font pixel heights (user spec: 15mm name, 8mm date on 29mm wide label)
// User reported 2x too large, so halve: ~7.5mm name, ~4mm date
// 306px / 29mm = 10.55 px/mm
#define NAME_PX_HEIGHT  111  // ~10.5mm
#define DATE_PX_HEIGHT  63   // ~6mm

// Set a pixel in the 1-bit framebuffer
static inline void set_pixel(int x, int y) {
    if (x < 0 || x >= LABEL_PRINTABLE_W || y < 0 || y >= LABEL_PRINTABLE_H)
        return;
    s_framebuffer[y * LABEL_FB_STRIDE + x / 8] |= (0x80 >> (x % 8));
}

// Draw a horizontal line
static void draw_hline(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x++)
        set_pixel(x, y);
}

// Blit an 8-bit grayscale glyph bitmap into the framebuffer, rotated 90° CCW.
// In rotated coordinates: text's x → fb +y, text's y → fb -x.
// origin_fb_x is the baseline x in fb (top of text), origin_fb_y is the left edge.
static void blit_glyph_rotated(const uint8_t *bitmap, int bw, int bh,
                                int origin_fb_x, int origin_fb_y,
                                int glyph_x_off, int glyph_y_off) {
    for (int gy = 0; gy < bh; gy++) {
        for (int gx = 0; gx < bw; gx++) {
            uint8_t alpha = bitmap[gy * bw + gx];
            if (alpha > 127) {  // threshold to 1-bit
                // Rotated: text x → fb y, text y → fb -x
                int fb_x = origin_fb_x - (glyph_y_off + gy);
                int fb_y = origin_fb_y + (glyph_x_off + gx);
                set_pixel(fb_x, fb_y);
            }
        }
    }
}

// Render a string rotated 90° CCW using stb_truetype.
// fb_x: baseline x-position in framebuffer (text extends upward from here)
// fb_y: starting y-position (text advances in +y direction)
// Returns the total advance in fb y-pixels.
static int render_string_rot(const char *str, float scale, int fb_x, int fb_y) {
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);

    float x_pos = 0;
    const char *p = str;
    while (*p) {
        int ch = (unsigned char)*p;
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&s_font, ch, &advance, &lsb);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&s_font, ch, scale, scale, &x0, &y0, &x1, &y1);

        int bw = x1 - x0;
        int bh = y1 - y0;

        if (bw > 0 && bh > 0) {
            uint8_t *bitmap = (uint8_t *)malloc(bw * bh);
            if (bitmap) {
                stbtt_MakeCodepointBitmap(&s_font, bitmap, bw, bh, bw, scale, scale, ch);
                int glyph_x_off = (int)(x_pos + 0.5f) + x0;
                int glyph_y_off = y0 + (int)(ascent * scale + 0.5f);
                blit_glyph_rotated(bitmap, bw, bh, fb_x, fb_y, glyph_x_off, glyph_y_off);
                free(bitmap);
            }
        }

        x_pos += advance * scale;

        // Kerning
        if (*(p + 1)) {
            int kern = stbtt_GetCodepointKernAdvance(&s_font, ch, (unsigned char)*(p + 1));
            x_pos += kern * scale;
        }
        p++;
    }
    return (int)(x_pos + 0.5f);
}

// Measure string width in pixels at given scale
static int measure_string(const char *str, float scale) {
    float x_pos = 0;
    const char *p = str;
    while (*p) {
        int ch = (unsigned char)*p;
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&s_font, ch, &advance, &lsb);
        x_pos += advance * scale;
        if (*(p + 1)) {
            int kern = stbtt_GetCodepointKernAdvance(&s_font, ch, (unsigned char)*(p + 1));
            x_pos += kern * scale;
        }
        p++;
    }
    return (int)(x_pos + 0.5f);
}

// Render a string centered in a y-region
static void render_string_rot_centered(const char *str, float scale, int fb_x,
                                        int y_start, int y_end) {
    int w = measure_string(str, scale);
    int fb_y = y_start + (y_end - y_start - w) / 2;
    render_string_rot(str, scale, fb_x, fb_y);
}

// Draw the i3 logo bitmap at position (dst_x, dst_y) in the framebuffer, rotated 90° CCW.
// scale: integer scale factor (1=original, 2=double, etc.)
// After rotation: logo occupies HEIGHT*scale px in x, WIDTH*scale px in y.
static void draw_logo(int dst_x, int dst_y, int scale = 1) {
    for (int y = 0; y < I3LOGO_HEIGHT; y++) {
        for (int x = 0; x < I3LOGO_WIDTH; x++) {
            int src_byte = y * I3LOGO_STRIDE + x / 8;
            uint8_t src_bit = 0x80 >> (x % 8);
            if (i3logo_data[src_byte] & src_bit) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int fx = dst_x + (I3LOGO_HEIGHT * scale - 1 - (y * scale + sy));
                        int fy = dst_y + (x * scale + sx);
                        set_pixel(fx, fy);
                    }
                }
            }
        }
    }
}

void label_renderer_init() {
    int offset = stbtt_GetFontOffsetForIndex(roboto_bold_ttf, 0);
    if (!stbtt_InitFont(&s_font, roboto_bold_ttf, offset)) {
        ESP_LOGE(TAG, "Failed to init TTF font");
        return;
    }
    s_font_ready = true;
    ESP_LOGI(TAG, "TTF renderer initialized (%dx%d), font loaded", LABEL_PRINTABLE_W, LABEL_PRINTABLE_H);
}

const uint8_t *label_renderer_render(const char *card_id) {
    if (!s_font_ready) {
        ESP_LOGE(TAG, "Font not initialized");
        return nullptr;
    }

    memset(s_framebuffer, 0, LABEL_FB_SIZE);

    // Compute font scales from pixel heights
    float name_scale = stbtt_ScaleForPixelHeight(&s_font, NAME_PX_HEIGHT);
    float date_scale = stbtt_ScaleForPixelHeight(&s_font, DATE_PX_HEIGHT);

    // Logo — rotated 90° CCW, 2x scaled, centered across label width
    // After rotation at 2x: occupies I3LOGO_HEIGHT*2 px in x, I3LOGO_WIDTH*2 px in y
    int logo_scale = 2;
    int logo_x = (LABEL_PRINTABLE_W - I3LOGO_HEIGHT * logo_scale) / 2;
    int logo_y = 20;
    draw_logo(logo_x, logo_y, logo_scale);

    // Text starts after logo
    int text_y_start = logo_y + I3LOGO_WIDTH * logo_scale + 30;

    // Font metrics
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);

    // Name — near top of label (high fb_x)
    int name_ascent_px = (int)(ascent * name_scale + 0.5f);
    int name_descent_px = (int)(-descent * name_scale + 0.5f);
    int name_top_margin = 15;  // px from top edge
    int name_fb_x = LABEL_PRINTABLE_W - name_top_margin - name_descent_px;
    render_string_rot(card_id, name_scale, name_fb_x, text_y_start);

    // Date — bottom-aligned to label edge, left-justified
    int date_ascent_px = (int)(ascent * date_scale + 0.5f);
    int date_bottom_margin = 5;  // px from bottom edge
    int date_fb_x = date_ascent_px + date_bottom_margin;

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char date_str[32];
    if (timeinfo.tm_year > (2020 - 1900)) {
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);
    } else {
        snprintf(date_str, sizeof(date_str), "(no time sync)");
    }
    render_string_rot(date_str, date_scale, date_fb_x, text_y_start);

    ESP_LOGI(TAG, "Label rendered: '%s' + '%s' + logo", card_id, date_str);
    return s_framebuffer;
}
