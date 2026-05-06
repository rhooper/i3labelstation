// Label renderer using stb_truetype for smooth TTF font rendering
// Text is rotated 90° CCW so it reads lengthwise on the 29x90mm label

// stb_truetype's default rasterizer allocates a single ~56 KB chunk per
// glyph for its active-edge list, plus a points/edges array. Under heap
// fragmentation (e.g. after the HelloClub DB load) the largest contiguous
// free block can drop below 56 KB and the rasterizer asserts. Replace
// stbtt's allocator with a bump arena that lives outside the system heap,
// so render memory is bounded and never depends on heap state.
#include <cstddef>
#include <cstdlib>

extern "C" void *stbtt_arena_alloc(size_t size);
extern "C" void  stbtt_arena_free(void *p);

#define STBTT_malloc(sz, ud)  ((void)(ud), stbtt_arena_alloc(sz))
#define STBTT_free(ptr, ud)   ((void)(ud), stbtt_arena_free(ptr))

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "label_renderer.h"
#include "card_lookup.h"
#include "app_config.h"
#include "brother_ql.h"
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
static uint16_t s_render_width = LABEL_PRINTABLE_W;
static uint16_t s_render_height = LABEL_PRINTABLE_H;
static uint16_t s_render_stride = LABEL_FB_STRIDE;

// stb_truetype scratch arena. Sized to fit one glyph's worst-case state:
//   active-edge chunk: 2000 * sizeof(stbtt__active_edge) ~= 56 KB
//   points + edges + small temps:                          ~ 4 KB
// 64 KB gives headroom. Allocated once in label_renderer_init from heap so
// it doesn't bloat .bss; reset (bump pointer back to 0) before each glyph.
#define STBTT_ARENA_SIZE (64 * 1024)
static uint8_t *s_stbtt_arena = nullptr;
static size_t   s_stbtt_arena_used = 0;
static size_t   s_stbtt_arena_peak = 0;
static int      s_stbtt_arena_fallbacks = 0;

extern "C" void *stbtt_arena_alloc(size_t size) {
    if (!s_stbtt_arena) {
        // Renderer not initialized yet; fall back to system malloc.
        s_stbtt_arena_fallbacks++;
        return malloc(size);
    }
    size_t aligned = (size + 7) & ~(size_t)7;
    if (s_stbtt_arena_used + aligned > STBTT_ARENA_SIZE) {
        s_stbtt_arena_fallbacks++;
        return malloc(size);
    }
    void *p = s_stbtt_arena + s_stbtt_arena_used;
    s_stbtt_arena_used += aligned;
    if (s_stbtt_arena_used > s_stbtt_arena_peak)
        s_stbtt_arena_peak = s_stbtt_arena_used;
    return p;
}

extern "C" void stbtt_arena_free(void *p) {
    // Pointers inside the arena are released en bloc when the arena is
    // reset between glyphs — nothing to do here. Pointers outside the arena
    // came from the malloc fallback above; free them normally.
    if (!p) return;
    if (s_stbtt_arena &&
        (uint8_t *)p >= s_stbtt_arena &&
        (uint8_t *)p <  s_stbtt_arena + STBTT_ARENA_SIZE) {
        return;
    }
    free(p);
}

// Font pixel heights for 29mm (306px) reference width
// Scaled proportionally for other widths
#define REF_WIDTH       306
#define NAME_PX_HEIGHT  127  // ~12mm at 306px
#define DATE_PX_HEIGHT  63   // ~6mm at 306px
#define TIME_PX_HEIGHT  42   // ~4mm at 306px

// Set a pixel in the 1-bit framebuffer
static inline void set_pixel(int x, int y) {
    if (x < 0 || x >= s_render_width || y < 0 || y >= s_render_height)
        return;
    s_framebuffer[y * s_render_stride + x / 8] |= (0x80 >> (x % 8));
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

static int utf8_peek_next(const char *p) {
    return utf8_decode(&p);
}

// Render a string rotated 90° CCW using stb_truetype (UTF-8 input).
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
                // Reset the stbtt arena before each glyph: every per-glyph
                // alloc (active edges, points, edges array) gets recycled.
                s_stbtt_arena_used = 0;
                stbtt_MakeCodepointBitmap(&s_font, bitmap, bw, bh, bw, scale, scale, ch);
                int glyph_x_off = (int)(x_pos + 0.5f) + x0;
                int glyph_y_off = y0 + (int)(ascent * scale + 0.5f);
                blit_glyph_rotated(bitmap, bw, bh, fb_x, fb_y, glyph_x_off, glyph_y_off);
                free(bitmap);
            }
        }

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

// Faint dotted line across the framebuffer width at the given fb_y.
// Used to mark the cut boundary between two stacked short labels (Mode 2,
// die-cut >= 90mm).
static void draw_cut_guide(int fb_y) {
    for (int x = 5; x < (int)s_render_width - 5; x += 6) {
        set_pixel(x, fb_y);
        set_pixel(x, fb_y + 1);
    }
}

// Word-wrap helper: render `text` at `font_scale` starting with first line's
// baseline at fb_x = start_fb_x, lines stacking toward lower fb_x. Lines are
// laid out at fb_y = fb_y_left (the left edge of each line in the rotated
// frame). Caller picks max_line_width.
//
// Returns the fb_x of the (top) baseline below the last line — the next
// line's baseline if more text were added.
static int render_wrapped(const char *text, float font_scale,
                          int start_fb_x, int fb_y_left, int max_line_width) {
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);
    int line_height = (int)((ascent - descent) * font_scale + 0.5f);

    char buf[LOOKUP_NAME_MAX];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char *words[16];
    int word_count = 0;
    char *saveptr;
    char *tok = strtok_r(buf, " ", &saveptr);
    while (tok && word_count < 16) {
        words[word_count++] = tok;
        tok = strtok_r(nullptr, " ", &saveptr);
    }

    int space_width = measure_string(" ", font_scale);
    int cur_fb_x = start_fb_x;
    int line_start = 0;

    while (line_start < word_count) {
        char line[LOOKUP_NAME_MAX] = {};
        int line_width = 0;
        int line_end = line_start;

        for (int i = line_start; i < word_count; i++) {
            int w = measure_string(words[i], font_scale);
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

        render_string_rot(line, font_scale, cur_fb_x, fb_y_left);
        cur_fb_x -= line_height + 2;
        line_start = line_end;
    }
    return cur_fb_x;
}

void label_renderer_init() {
    if (!s_stbtt_arena) {
        s_stbtt_arena = (uint8_t *)malloc(STBTT_ARENA_SIZE);
        if (!s_stbtt_arena) {
            ESP_LOGE(TAG, "Failed to allocate stbtt arena (%d bytes)", STBTT_ARENA_SIZE);
        }
    }
    int offset = stbtt_GetFontOffsetForIndex(roboto_bold_ttf, 0);
    if (!stbtt_InitFont(&s_font, roboto_bold_ttf, offset)) {
        ESP_LOGE(TAG, "Failed to init TTF font");
        return;
    }
    s_font_ready = true;
    ESP_LOGI(TAG, "TTF renderer initialized (fb %d bytes, stbtt arena %d bytes)",
             LABEL_FB_SIZE, STBTT_ARENA_SIZE);
}

uint16_t label_renderer_stride() {
    return s_render_stride;
}

// =====================================================================
// Mode-specific renderers
// =====================================================================

// Mode 1 (NORMAL): full layout — logo + name + email + date + time + phone.
// Always renders into the full framebuffer [0, fb_h).
static void render_normal(const label_render_req_t *req) {
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);

    uint16_t width = req->fb_w;
    uint16_t height = req->fb_h;

    float scale_factor = (float)width / REF_WIDTH;
    float name_scale = stbtt_ScaleForPixelHeight(&s_font, (int)(NAME_PX_HEIGHT * scale_factor));
    float date_scale = stbtt_ScaleForPixelHeight(&s_font, (int)(DATE_PX_HEIGHT * scale_factor));
    float time_scale = stbtt_ScaleForPixelHeight(&s_font, (int)(TIME_PX_HEIGHT * scale_factor));

    // Logo — rotated 90° CCW, scaled to fit width
    int logo_scale = (width >= 250) ? 2 : 1;
    int logo_x = (width - I3LOGO_HEIGHT * logo_scale) / 2;
    int logo_y = 5;
    draw_logo(logo_x, logo_y, logo_scale);

    int text_y_start = logo_y + I3LOGO_WIDTH * logo_scale + 8;

    // Name — top-aligned, with word wrapping
    int name_descent_px = (int)(-descent * name_scale + 0.5f);
    int name_fb_x = width - name_descent_px;
    int max_line_width = height - text_y_start - 5;
    render_wrapped(req->name, name_scale, name_fb_x, text_y_start, max_line_width);

    // Date — bottom-left, format Mon-D-YYYY (3-letter month, no zero-pad on day).
    int date_ascent_px = (int)(ascent * date_scale + 0.5f);
    int date_bottom_margin = 2;
    int date_fb_x = date_ascent_px + date_bottom_margin;
    int date_line_height = (int)((ascent - descent) * date_scale + 0.5f);

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char date_str[48];
    if (timeinfo.tm_year > (2020 - 1900)) {
        // Format: "Mon-D-YYYY" e.g. "May-5-2026". Built piecewise because
        // newlib's strftime doesn't support the GNU/BSD %-d (no-zero-pad)
        // extension — that produced garbage on-target.
        char month[8];
        strftime(month, sizeof(month), "%b", &timeinfo);
        if (req->mode == 3) {
            // PERMIT: render a date range "<today> to <today + N days>"
            // instead of a single date. Same fb_y position (left-aligned).
            time_t future = now + (time_t)req->days * 86400;
            struct tm later;
            localtime_r(&future, &later);
            char m2[8];
            strftime(m2, sizeof(m2), "%b", &later);
            snprintf(date_str, sizeof(date_str), "%s-%d-%d to %s-%d-%d",
                     month, timeinfo.tm_mday, timeinfo.tm_year + 1900,
                     m2, later.tm_mday, later.tm_year + 1900);
        } else {
            snprintf(date_str, sizeof(date_str), "%s-%d-%d",
                     month, timeinfo.tm_mday, timeinfo.tm_year + 1900);
        }
    } else {
        snprintf(date_str, sizeof(date_str), "(no time sync)");
    }
    render_string_rot(date_str, date_scale, date_fb_x, text_y_start);

    // Email line just above the date, same font, same left edge.
    // Shown in NORMAL (mode 1) and PERMIT (mode 3).
    if (req->email && req->email[0]) {
        int email_fb_x = date_fb_x + date_line_height + 2;
        render_string_rot(req->email, date_scale, email_fb_x, text_y_start);
    }

    // Time — bottom-right, smaller font. Phone sits just above it.
    if (timeinfo.tm_year > (2020 - 1900)) {
        char time_str[16];
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        const char *ampm = timeinfo.tm_hour < 12 ? "am" : "pm";
        snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour12, timeinfo.tm_min, ampm);

        int time_ascent_px = (int)(ascent * time_scale + 0.5f);
        int time_fb_x = time_ascent_px + date_bottom_margin;
        int time_line_height = (int)((ascent - descent) * time_scale + 0.5f);
        int time_width = measure_string(time_str, time_scale);
        int right_margin = 5;
        int time_fb_y = height - time_width - right_margin;
        render_string_rot(time_str, time_scale, time_fb_x, time_fb_y);

        if (req->phone && req->phone[0]) {
            int phone_fb_x = time_fb_x + time_line_height + 2;
            int phone_width = measure_string(req->phone, time_scale);
            int phone_fb_y = height - phone_width - right_margin;
            render_string_rot(req->phone, time_scale, phone_fb_x, phone_fb_y);
        }

        ESP_LOGI(TAG, "Normal label: '%s' + '%s' + '%s' (%dpx)", req->name, date_str, time_str, width);
    } else {
        ESP_LOGI(TAG, "Normal label: '%s' + '%s' (%dpx)", req->name, date_str, width);
    }
}

// Mode 2 (SHORT): no logo, name in date-font (wrapped, top-aligned), date,
// time, phone. Renders into fb_y range [fb_y_start, fb_y_end) so a die-cut
// >= 90mm can stack two layouts in one framebuffer.
static void render_short(const label_render_req_t *req, int fb_y_start, int fb_y_end) {
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);

    uint16_t width = req->fb_w;

    float scale_factor = (float)width / REF_WIDTH;
    float date_scale = stbtt_ScaleForPixelHeight(&s_font, (int)(DATE_PX_HEIGHT * scale_factor));
    float time_scale = stbtt_ScaleForPixelHeight(&s_font, (int)(TIME_PX_HEIGHT * scale_factor));

    int date_descent_px = (int)(-descent * date_scale + 0.5f);
    int date_ascent_px = (int)(ascent * date_scale + 0.5f);
    int date_line_height = (int)((ascent - descent) * date_scale + 0.5f);
    int date_bottom_margin = 2;
    int date_fb_x = date_ascent_px + date_bottom_margin;
    int time_ascent_px = (int)(ascent * time_scale + 0.5f);
    int time_line_height = (int)((ascent - descent) * time_scale + 0.5f);
    int time_fb_x = time_ascent_px + date_bottom_margin;

    int left_margin = 2;
    int right_margin = 5;
    int span = fb_y_end - fb_y_start;
    int max_line_width = span - left_margin - right_margin;

    // Name in date-font, wrapped, top-aligned (high fb_x, near top of label).
    int name_fb_x = width - date_descent_px;
    render_wrapped(req->name, date_scale, name_fb_x, fb_y_start + left_margin, max_line_width);
    (void)date_line_height;  // available if we later care about post-wrap fb_x

    // Date — bottom-left of this layout's span.
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char date_str[32];
    if (timeinfo.tm_year > (2020 - 1900)) {
        // Format: "Mon-D-YYYY" e.g. "May-5-2026". Built piecewise because
        // newlib's strftime doesn't support the GNU/BSD %-d (no-zero-pad)
        // extension — that produced garbage on-target.
        char month[8];
        strftime(month, sizeof(month), "%b", &timeinfo);
        snprintf(date_str, sizeof(date_str), "%s-%d-%d",
                 month, timeinfo.tm_mday, timeinfo.tm_year + 1900);
    } else {
        snprintf(date_str, sizeof(date_str), "(no time sync)");
    }
    render_string_rot(date_str, date_scale, date_fb_x, fb_y_start + left_margin);

    // Time + phone above it — bottom-right of this layout's span.
    if (timeinfo.tm_year > (2020 - 1900)) {
        char time_str[16];
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        const char *ampm = timeinfo.tm_hour < 12 ? "am" : "pm";
        snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour12, timeinfo.tm_min, ampm);

        int time_width = measure_string(time_str, time_scale);
        int time_fb_y = fb_y_end - time_width - right_margin;
        render_string_rot(time_str, time_scale, time_fb_x, time_fb_y);

        if (req->phone && req->phone[0]) {
            int phone_fb_x = time_fb_x + time_line_height + 2;
            int phone_width = measure_string(req->phone, time_scale);
            int phone_fb_y = fb_y_end - phone_width - right_margin;
            render_string_rot(req->phone, time_scale, phone_fb_x, phone_fb_y);
        }
    }

    ESP_LOGI(TAG, "Short label: '%s' [fb_y %d..%d]", req->name, fb_y_start, fb_y_end);
}

const uint8_t *label_renderer_render(const label_render_req_t *req) {
    if (!s_font_ready) {
        ESP_LOGE(TAG, "Font not initialized");
        return nullptr;
    }
    if (!req || !req->name) {
        ESP_LOGE(TAG, "Null render request");
        return nullptr;
    }

    uint16_t width = req->fb_w;
    uint16_t height = req->fb_h;

    s_render_width = width;
    s_render_height = height;
    s_render_stride = (width + 7) / 8;

    size_t fb_size = (size_t)s_render_stride * height;
    if (fb_size > LABEL_FB_SIZE) {
        ESP_LOGE(TAG, "Framebuffer too small: need %u, have %d", (unsigned)fb_size, LABEL_FB_SIZE);
        return nullptr;
    }
    memset(s_framebuffer, 0, fb_size);

    ESP_LOGI(TAG, "Rendering mode=%d %dx%d (stride=%d, %u bytes)",
             req->mode, width, height, s_render_stride, (unsigned)fb_size);

    if (req->mode == 2) {
        // Mode 2: SHORT layout, media-aware print quantity.
        if (req->media_type == MEDIA_TYPE_DIE_CUT && req->media_length_mm >= 90) {
            // Two short layouts stacked on one die-cut piece, with a cut guide.
            int mid = height / 2;
            render_short(req, 0, mid);
            render_short(req, mid, height);
            draw_cut_guide(mid);
        } else {
            // Continuous (caller passed smaller fb_h) or shorter die-cut: one layout.
            render_short(req, 0, height);
        }
    } else {
        // Default to Mode 1 layout for any other mode value.
        render_normal(req);
    }

    return s_framebuffer;
}
