#include "wallpaper.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wp_prefs_default(wp_prefs *prefs) {
    prefs->speed = 1.0;
    prefs->fit = WP_FIT_FILL;
    prefs->pause_when_covered = true;
    prefs->half_speed_on_battery = false;
    prefs->half_speed_in_low_power = false;
}

void wp_config_default(wp_config *config) {
    wp_prefs_default(&config->prefs);
    config->content = WP_CONTENT_BUILTIN;
    config->path[0] = '\0';
}

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static bool same_word(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

static void apply(wp_config *config, const char *key, const char *value) {
    wp_prefs *p = &config->prefs;
    if (strcmp(key, "speed") == 0) {
        double speed;
        if (wp_speed_parse(value, &speed)) p->speed = wp_speed_clamp(speed);
    } else if (strcmp(key, "fit") == 0) {
        if (same_word(value, "fill")) p->fit = WP_FIT_FILL;
        else if (same_word(value, "fit")) p->fit = WP_FIT_FIT;
    } else if (strcmp(key, "pause_when_covered") == 0) {
        wp_switch_parse(value, &p->pause_when_covered);
    } else if (strcmp(key, "half_speed_on_battery") == 0) {
        wp_switch_parse(value, &p->half_speed_on_battery);
    } else if (strcmp(key, "half_speed_in_low_power") == 0) {
        wp_switch_parse(value, &p->half_speed_in_low_power);
    } else if (strcmp(key, "content") == 0) {
        if (same_word(value, "builtin")) config->content = WP_CONTENT_BUILTIN;
        else if (same_word(value, "video")) config->content = WP_CONTENT_VIDEO;
        else if (same_word(value, "image")) config->content = WP_CONTENT_IMAGE;
    } else if (strcmp(key, "path") == 0) {
        snprintf(config->path, sizeof config->path, "%s", value);
    }
}

bool wp_config_parse(wp_config *config, const char *text) {
    if (!text) return false;
    char line[WP_PATH_MAX + 64];
    const char *cursor = text;
    while (*cursor) {
        const char *newline = strchr(cursor, '\n');
        size_t length = newline ? (size_t)(newline - cursor) : strlen(cursor);
        if (length >= sizeof line) length = sizeof line - 1;
        memcpy(line, cursor, length);
        line[length] = '\0';
        cursor = newline ? newline + 1 : cursor + strlen(cursor);
        char *content = trim(line);
        if (*content == '\0' || *content == '#' || *content == ';') continue;
        char *equals = strchr(content, '=');
        if (!equals) continue;
        *equals = '\0';
        apply(config, trim(content), trim(equals + 1));
    }
    return true;
}

static const char *fit_name(wp_fit fit) {
    return fit == WP_FIT_FIT ? "fit" : "fill";
}

static const char *content_name(wp_content content) {
    switch (content) {
    case WP_CONTENT_VIDEO: return "video";
    case WP_CONTENT_IMAGE: return "image";
    default: return "builtin";
    }
}

size_t wp_config_format(const wp_config *config, char *out, size_t size) {
    const wp_prefs *p = &config->prefs;
    int written = snprintf(out, size,
        "speed = %.2f\n"
        "fit = %s\n"
        "pause_when_covered = %s\n"
        "half_speed_on_battery = %s\n"
        "half_speed_in_low_power = %s\n"
        "content = %s\n"
        "path = %s\n",
        p->speed, fit_name(p->fit), p->pause_when_covered ? "on" : "off", p->half_speed_on_battery ? "on" : "off", p->half_speed_in_low_power ? "on" : "off",
        content_name(config->content), config->path);
    return written < 0 ? 0 : (size_t)written;
}

bool wp_speed_parse(const char *text, double *out) {
    if (!text) return false;
    char cleaned[64];
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c && n < sizeof cleaned - 1; c++) {
        if (c[0] == 0xC3 && c[1] == 0x97) { c++; continue; }
        if (*c == 'x' || *c == 'X' || isspace(*c)) continue;
        cleaned[n++] = *c == ',' ? '.' : (char)*c;
    }
    cleaned[n] = '\0';
    bool percent = n > 0 && cleaned[n - 1] == '%';
    if (percent) cleaned[--n] = '\0';
    if (n == 0) return false;
    char *end;
    double value = strtod(cleaned, &end);
    if (*end != '\0' || !isfinite(value)) return false;
    *out = percent ? value / 100.0 : value;
    return true;
}

double wp_speed_clamp(double value) {
    if (!isfinite(value)) return 1.0;
    if (value < WP_SPEED_MIN) value = WP_SPEED_MIN;
    if (value > WP_SPEED_MAX) value = WP_SPEED_MAX;
    return round(value * 100.0) / 100.0;
}

bool wp_switch_parse(const char *text, bool *out) {
    if (!text) return false;
    if (same_word(text, "on") || same_word(text, "true") || same_word(text, "yes") || strcmp(text, "1") == 0) *out = true;
    else if (same_word(text, "off") || same_word(text, "false") || same_word(text, "no") || strcmp(text, "0") == 0) *out = false;
    else return false;
    return true;
}

double wp_rate(const wp_prefs *prefs, const wp_system *system) {
    bool half = (prefs->half_speed_on_battery && system->on_battery) || (prefs->half_speed_in_low_power && system->low_power);
    return wp_speed_clamp(half ? prefs->speed / 2.0 : prefs->speed);
}

double wp_slider_speed(double position) {
    double octave = (position * 2.0 - 1.0) * WP_SLIDER_OCTAVES;
    double nearest = round(octave);
    if (fabs(octave - nearest) < WP_SLIDER_SNAP) octave = nearest;
    return wp_speed_clamp(exp2(octave));
}

double wp_slider_position(double speed) {
    double octave = log2(wp_speed_clamp(speed));
    if (octave < -WP_SLIDER_OCTAVES) octave = -WP_SLIDER_OCTAVES;
    if (octave > WP_SLIDER_OCTAVES) octave = WP_SLIDER_OCTAVES;
    return (octave / WP_SLIDER_OCTAVES + 1.0) / 2.0;
}

bool wp_frozen(const wp_system *system) {
    return system->manual_pause || system->display_asleep || system->screensaver || system->locked ||
           system->session_inactive || system->hot;
}

wp_want wp_decide(const wp_prefs *prefs, const wp_system *system, bool covered) {
    if (wp_frozen(system)) return WP_WANT_RELEASE;
    return covered && prefs->pause_when_covered ? WP_WANT_PAUSE : WP_WANT_PLAY;
}

wp_want wp_want_merge(wp_want a, wp_want b) {
    if (a == WP_WANT_PLAY || b == WP_WANT_PLAY) return WP_WANT_PLAY;
    if (a == WP_WANT_PAUSE || b == WP_WANT_PAUSE) return WP_WANT_PAUSE;
    return WP_WANT_RELEASE;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/' || *c == '\\') base = c + 1;
    }
    return base;
}

static bool has_extension(const char *name, const char *const *list) {
    const char *dot = strrchr(base_name(name), '.');
    if (!dot) return false;
    for (; *list; list++) {
        if (same_word(dot + 1, *list)) return true;
    }
    return false;
}

bool wp_is_video_name(const char *name) {
    static const char *const types[] = {"mp4", "mov", "m4v", "mkv", "webm", NULL};
    return name && has_extension(name, types);
}

bool wp_is_image_name(const char *name) {
    static const char *const types[] = {"jpg", "jpeg", "png", "heic", "heif", "tif", "tiff", "bmp", "gif", "webp", NULL};
    return name && has_extension(name, types);
}

static bool all_digits(const char *start, const char *end) {
    if (start == end) return false;
    for (const char *c = start; c < end; c++) {
        if (!isdigit((unsigned char)*c)) return false;
    }
    return true;
}

bool wp_tier_parse(const char *name, wp_tier *tier) {
    if (!wp_is_video_name(name)) return false;
    const char *base = base_name(name);
    const char *stop = strrchr(base, '.');
    int width = 0, height = 0, fps = 0;
    bool cropped = false;
    const char *part = base;
    while (part < stop) {
        const char *dash = part;
        while (dash < stop && *dash != '-') dash++;
        const char *x = part;
        while (x < dash && *x != 'x') x++;
        if (x < dash && all_digits(part, x) && all_digits(x + 1, dash)) {
            width = atoi(part);
            height = atoi(x + 1);
        } else if (dash - part > 3 && strncmp(dash - 3, "fps", 3) == 0 && all_digits(part, dash - 3)) {
            fps = atoi(part);
        } else if (dash - part == 4 && strncmp(part, "crop", 4) == 0) {
            cropped = true;
        }
        part = dash + 1;
    }
    if (width <= 0 || height <= 0) return false;
    tier->width = width;
    tier->height = height;
    tier->fps = fps > 0 ? fps : 30;
    tier->cropped = cropped;
    return true;
}

size_t wp_tier_name(const char *prefix, wp_tier tier, char *out, size_t size) {
    if (!prefix || !out || size == 0 || tier.width <= 0 || tier.height <= 0 || tier.fps <= 0) return 0;
    int n = snprintf(out, size, "%s-%dx%d-%dfps%s.mp4", prefix, tier.width, tier.height, tier.fps,
                     tier.cropped ? "-crop" : "");
    return n < 0 || (size_t)n >= size ? 0 : (size_t)n;
}

static double cover_scale(double width, double height, double target_width, double target_height, wp_fit fit) {
    if (width <= 0 || height <= 0) return HUGE_VAL;
    double sx = target_width / width, sy = target_height / height;
    if (fit == WP_FIT_FIT) return sx < sy ? sx : sy;
    return sx > sy ? sx : sy;
}

static bool tier_fits_view(const wp_tier *tier, double target_width, double target_height, wp_fit fit) {
    if (!tier->cropped) return true;
    if (fit != WP_FIT_FILL || target_height <= 0 || tier->height <= 0) return false;
    double wanted = target_width / target_height, have = (double)tier->width / tier->height;
    return fabs(have - wanted) / wanted <= WP_ASPECT_TOLERANCE;
}

static long pixels(const wp_tier *tier) {
    return (long)tier->width * tier->height;
}

static bool same_size(const wp_tier *a, const wp_tier *b) {
    return a->width == b->width && a->height == b->height && a->cropped == b->cropped;
}

int wp_pick_tier(const wp_tier *tiers, int count, double target_width, double target_height, double rate, wp_fit fit) {
    int best = -1, largest = -1;
    for (int i = 0; i < count; i++) {
        if (!tier_fits_view(&tiers[i], target_width, target_height, fit)) continue;
        if (largest < 0 || pixels(&tiers[i]) > pixels(&tiers[largest])) largest = i;
        if (cover_scale(tiers[i].width, tiers[i].height, target_width, target_height, fit) <= 1.02 &&
            (best < 0 || pixels(&tiers[i]) < pixels(&tiers[best]))) best = i;
    }
    if (best < 0) best = largest;
    if (best < 0) return -1;
    int chosen = -1, fastest = -1;
    for (int i = 0; i < count; i++) {
        if (!same_size(&tiers[i], &tiers[best])) continue;
        if (fastest < 0 || tiers[i].fps > tiers[fastest].fps) fastest = i;
        if (tiers[i].fps * rate >= WP_DISPLAY_FPS && (chosen < 0 || tiers[i].fps < tiers[chosen].fps)) chosen = i;
    }
    return chosen >= 0 ? chosen : fastest;
}

static int even_ceil(double value) {
    int n = (int)ceil(value - 1e-6);
    return n + (n & 1);
}

bool wp_fitted_tier(wp_tier source, double target_width, double target_height, wp_fit fit, wp_tier *out) {
    if (!out || source.cropped || source.width <= 0 || source.height <= 0 || source.fps <= 0) return false;
    if (target_width < 16 || target_height < 16) return false;
    double scale = cover_scale(source.width, source.height, target_width, target_height, fit);
    if (scale > 1.0) return false;
    int width = even_ceil(fit == WP_FIT_FILL ? target_width : source.width * scale);
    int height = even_ceil(fit == WP_FIT_FILL ? target_height : source.height * scale);
    if (width > source.width) width = source.width;
    if (height > source.height) height = source.height;
    double saving = 1.0 - (double)width * height / ((double)source.width * source.height);
    if (saving < WP_FITTED_SAVING) return false;
    double aspect = (double)source.width / source.height, fitted = (double)width / height;
    out->width = width;
    out->height = height;
    out->fps = source.fps;
    out->cropped = fabs(fitted - aspect) / aspect > WP_ASPECT_TOLERANCE;
    return true;
}

int wp_loop_repeats(double duration) {
    if (!(duration > 0)) return 0;
    double count = WP_LOOP_SPAN / duration;
    if (count >= WP_LOOP_REPEATS_MAX) return WP_LOOP_REPEATS_MAX;
    return count < 1 ? 2 : (int)count + 1;
}

bool wp_fitted_allowed(const wp_system *system) {
    return !system->on_battery && !system->low_power && !system->hot;
}

wp_rect wp_canvas(const wp_rect *screens, int count) {
    wp_rect none = {0, 0, 0, 0};
    if (count <= 0) return none;
    double x0 = screens[0].x, y0 = screens[0].y;
    double x1 = x0 + screens[0].w, y1 = y0 + screens[0].h;
    for (int i = 1; i < count; i++) {
        if (screens[i].x < x0) x0 = screens[i].x;
        if (screens[i].y < y0) y0 = screens[i].y;
        if (screens[i].x + screens[i].w > x1) x1 = screens[i].x + screens[i].w;
        if (screens[i].y + screens[i].h > y1) y1 = screens[i].y + screens[i].h;
    }
    wp_rect area = {x0, y0, x1 - x0, y1 - y0};
    return area;
}

wp_rect wp_displayed(double image_width, double image_height, wp_rect area, wp_fit fit) {
    if (image_width <= 0 || image_height <= 0) return area;
    double sx = area.w / image_width, sy = area.h / image_height;
    double factor = fit == WP_FIT_FILL ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    double w = image_width * factor, h = image_height * factor;
    wp_rect shown = {area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
    return shown;
}

bool wp_covered(wp_rect region, const wp_rect *windows, int count) {
    if (region.w <= 16 || region.h <= 16) return true;
    if (count <= 0) return false;
    const int columns = 25, rows = 17;
    for (int c = 0; c < columns; c++) {
        double x = region.x + (c + 0.5) / columns * region.w;
        for (int r = 0; r < rows; r++) {
            double y = region.y + (r + 0.5) / rows * region.h;
            bool hit = false;
            for (int i = 0; i < count && !hit; i++) {
                hit = x >= windows[i].x && x < windows[i].x + windows[i].w &&
                      y >= windows[i].y && y < windows[i].y + windows[i].h;
            }
            if (!hit) return false;
        }
    }
    return true;
}

void wp_surface_init(wp_surface *surface) {
    surface->state = WP_RELEASED;
    surface->paused_at = -1;
    surface->rate = 0;
    surface->resume = -1;
}

wp_action wp_surface_step(wp_surface *surface, wp_want want, double rate, double now, double release_delay) {
    wp_action action = {false, false, false, false, rate, 0};
    switch (want) {
    case WP_WANT_PLAY:
        if (surface->state == WP_RELEASED) {
            action.build = true;
            action.start = surface->resume > 0 ? surface->resume : 0;
            surface->resume = -1;
        }
        if (surface->state != WP_PLAYING || surface->rate != rate) action.play = true;
        surface->state = WP_PLAYING;
        surface->paused_at = -1;
        surface->rate = rate;
        break;
    case WP_WANT_PAUSE:
        if (surface->state == WP_PLAYING) {
            action.pause = true;
            surface->state = WP_PAUSED;
            surface->paused_at = now;
        } else if (surface->state == WP_PAUSED && now - surface->paused_at >= release_delay) {
            action.release = true;
            surface->state = WP_RELEASED;
            surface->paused_at = -1;
        }
        break;
    case WP_WANT_RELEASE:
        if (surface->state != WP_RELEASED) {
            action.release = true;
            surface->state = WP_RELEASED;
            surface->paused_at = -1;
        }
        break;
    }
    return action;
}

void wp_surface_hold(wp_surface *surface, double position) {
    if (position >= 0) surface->resume = position;
}

double wp_surface_deadline(const wp_surface *surface, double release_delay) {
    return surface->state == WP_PAUSED ? surface->paused_at + release_delay : -1;
}

uint64_t wp_hash(const char *text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *c = (const unsigned char *)text; c && *c; c++) {
        hash ^= *c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void wp_choice_init(wp_choice *choice) {
    memset(choice, 0, sizeof *choice);
}

bool wp_choice_offer(wp_choice *choice, uint64_t key, double now, double hold, bool immediate) {
    if (!choice->has_current || immediate) {
        bool changed = !choice->has_current || choice->current != key;
        choice->current = key;
        choice->has_current = true;
        choice->has_pending = false;
        return changed;
    }
    if (key == choice->current) {
        choice->has_pending = false;
        return false;
    }
    if (!choice->has_pending || choice->pending != key) {
        choice->pending = key;
        choice->since = now;
        choice->has_pending = true;
        return false;
    }
    if (now - choice->since >= hold) {
        choice->current = key;
        choice->has_pending = false;
        return true;
    }
    return false;
}

double wp_choice_deadline(const wp_choice *choice, double hold) {
    return choice->has_pending ? choice->since + hold : -1;
}

double wp_next_wake(double now, double a, double b) {
    double best = -1;
    if (a >= 0) best = a;
    if (b >= 0 && (best < 0 || b < best)) best = b;
    if (best >= 0 && best < now) best = now;
    return best;
}
