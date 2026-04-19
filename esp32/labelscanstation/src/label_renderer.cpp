// Label renderer using stb_truetype for smooth TTF font rendering
// Text is rotated 90° CCW so it reads lengthwise on the 29x90mm label

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "label_renderer.h"
#include "card_lookup.h"
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
#define NAME_PX_HEIGHT  127  // ~12mm
#define DATE_PX_HEIGHT  63   // ~6mm
#define TIME_PX_HEIGHT  42   // ~4mm

// Set a pixel in the 1-bit framebuffer
static inline void set_pixel(int x, int y) {
    if (x < 0 || x >= LABEL_PRINTABLE_W || y < 0 || y >= LABEL_PRINTABLE_H)
        return;
    s_framebuffer[y * LABEL_FB_STRIDE + x / 8] |= (0x80 >> (x % 8));
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

// Decode one UTF-8 codepoint from *p, advance *p past it. Returns codepoint or 0xFFFD on error.
static int utf8_decode(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    int ch;
    if (s[0] < 0x80) {
        ch = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        ch = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        ch = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        ch = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    } else {
        ch = 0xFFFD;  // replacement character
        *p += 1;
    }
    return ch;
}

// Peek at next UTF-8 codepoint without advancing
static int utf8_peek_next(const char *p) {
    return utf8_decode(&p);
}

// Render a string rotated 90° CCW using stb_truetype (UTF-8 input).
// fb_x: baseline x-position in framebuffer (text extends upward from here)
// fb_y: starting y-position (text advances in +y direction)
// Returns the total advance in fb y-pixels.
static int render_string_rot(const char *str, float scale, int fb_x, int fb_y) {
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);

    float x_pos = 0;
    const char *p = str;
    while (*p) {
        const char *next = p;
        int ch = utf8_decode(&next);
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

        // Kerning with next character
        if (*next) {
            int next_ch = utf8_peek_next(next);
            int kern = stbtt_GetCodepointKernAdvance(&s_font, ch, next_ch);
            x_pos += kern * scale;
        }
        p = next;
    }
    return (int)(x_pos + 0.5f);
}

// Measure string width in pixels at given scale (UTF-8 input)
static int measure_string(const char *str, float scale) {
    float x_pos = 0;
    const char *p = str;
    while (*p) {
        const char *next = p;
        int ch = utf8_decode(&next);
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&s_font, ch, &advance, &lsb);
        x_pos += advance * scale;
        if (*next) {
            int next_ch = utf8_peek_next(next);
            int kern = stbtt_GetCodepointKernAdvance(&s_font, ch, next_ch);
            x_pos += kern * scale;
        }
        p = next;
    }
    return (int)(x_pos + 0.5f);
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

const uint8_t *label_renderer_render(const char *name) {
    if (!s_font_ready) {
        ESP_LOGE(TAG, "Font not initialized");
        return nullptr;
    }

    memset(s_framebuffer, 0, LABEL_FB_SIZE);

    // Compute font scales from pixel heights
    float name_scale = stbtt_ScaleForPixelHeight(&s_font, NAME_PX_HEIGHT);
    float date_scale = stbtt_ScaleForPixelHeight(&s_font, DATE_PX_HEIGHT);
    float time_scale = stbtt_ScaleForPixelHeight(&s_font, TIME_PX_HEIGHT);

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

    // Name — top-aligned, with word wrapping if too long
    int name_descent_px = (int)(-descent * name_scale + 0.5f);
    int name_line_height = (int)((ascent - descent) * name_scale + 0.5f);
    int name_top_margin = 0;
    int name_fb_x = LABEL_PRINTABLE_W - name_top_margin - name_descent_px;
    int max_line_width = LABEL_PRINTABLE_H - text_y_start - 20;

    // Word-wrap: split on spaces, measure words, break lines
    {
        char name_buf[LOOKUP_NAME_MAX];
        strncpy(name_buf, name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';

        // Collect words
        const char *words[16];
        int word_count = 0;
        char *saveptr;
        char *tok = strtok_r(name_buf, " ", &saveptr);
        while (tok && word_count < 16) {
            words[word_count++] = tok;
            tok = strtok_r(nullptr, " ", &saveptr);
        }

        int space_width = measure_string(" ", name_scale);
        int cur_fb_x = name_fb_x;
        int line_start = 0;

        while (line_start < word_count) {
            // Build a line by adding words until it overflows
            char line[LOOKUP_NAME_MAX] = {};
            int line_width = 0;
            int line_end = line_start;

            for (int i = line_start; i < word_count; i++) {
                int w = measure_string(words[i], name_scale);
                int trial = (i == line_start) ? w : line_width + space_width + w;
                if (trial > max_line_width && i > line_start)
                    break;
                if (i > line_start) {
                    strcat(line, " ");
                    line_width += space_width;
                }
                strcat(line, words[i]);
                line_width += w;
                line_end = i + 1;
            }

            render_string_rot(line, name_scale, cur_fb_x, text_y_start);
            cur_fb_x -= name_line_height + 2;  // 2px line gap (reduced)
            line_start = line_end;
        }
    }

    // Date — bottom-left
    int date_ascent_px = (int)(ascent * date_scale + 0.5f);
    int date_bottom_margin = 5;
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

    // Time — bottom-right, smaller font, "HH:MM am" format
    if (timeinfo.tm_year > (2020 - 1900)) {
        char time_str[16];
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        const char *ampm = timeinfo.tm_hour < 12 ? "am" : "pm";
        snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour12, timeinfo.tm_min, ampm);

        int time_ascent_px = (int)(ascent * time_scale + 0.5f);
        int time_fb_x = time_ascent_px + date_bottom_margin;
        int time_width = measure_string(time_str, time_scale);
        int right_margin = 20;
        int time_fb_y = LABEL_PRINTABLE_H - time_width - right_margin;
        render_string_rot(time_str, time_scale, time_fb_x, time_fb_y);

        ESP_LOGI(TAG, "Label rendered: '%s' + '%s' + '%s' + logo", name, date_str, time_str);
    } else {
        ESP_LOGI(TAG, "Label rendered: '%s' + '%s' + logo", name, date_str);
    }

    return s_framebuffer;
}

