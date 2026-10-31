#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WP_PATH_MAX 1024
#define WP_RELEASE_DELAY 90.0
#define WP_SWITCH_HOLD 2.0
#define WP_DISPLAY_FPS 60.0
#define WP_SPEED_MIN 0.01
#define WP_SPEED_MAX 100.0
#define WP_SLIDER_OCTAVES 2.0
#define WP_SLIDER_SNAP 0.08
#define WP_COVERAGE_POLL 5.0
#define WP_FITTED_SAVING 0.2
#define WP_FITTED_LIMIT 6
#define WP_FIT_DELAY 30.0
#define WP_SCENE "Gargantua"
#define WP_LOOP_SPAN 21600.0
#define WP_LOOP_REPEATS_MAX 10000
#define WP_ASPECT_TOLERANCE 0.01
#define WP_DIMENSION_MAX 32768

typedef struct { double x, y, w, h; } wp_rect;

typedef enum { WP_FIT_FILL, WP_FIT_FIT } wp_fit;
typedef enum { WP_CONTENT_BUILTIN, WP_CONTENT_VIDEO, WP_CONTENT_IMAGE } wp_content;

typedef struct {
    double speed;
    wp_fit fit;
    bool pause_when_covered;
    bool half_speed_on_battery;
    bool half_speed_in_low_power;
} wp_prefs;

typedef struct {
    wp_prefs prefs;
    wp_content content;
    char path[WP_PATH_MAX];
} wp_config;

typedef struct {
    bool manual_pause;
    bool display_asleep;
    bool screensaver;
    bool locked;
    bool session_inactive;
    bool on_battery;
    bool low_power;
    bool hot;
} wp_system;

typedef struct { int width, height, fps; bool cropped; } wp_tier;

typedef enum { WP_RELEASED, WP_PAUSED, WP_PLAYING } wp_state;
typedef enum { WP_WANT_PLAY, WP_WANT_PAUSE, WP_WANT_RELEASE } wp_want;

typedef struct {
    wp_state state;
    double paused_at;
    double rate;
    double resume;
} wp_surface;

typedef struct {
    bool build;
    bool play;
    bool pause;
    bool release;
    double rate;
    double start;
} wp_action;

typedef struct {
    uint64_t current;
    uint64_t pending;
    double since;
    bool has_current;
    bool has_pending;
} wp_choice;

void wp_prefs_default(wp_prefs *prefs);
void wp_config_default(wp_config *config);
bool wp_config_parse(wp_config *config, const char *text);
size_t wp_config_format(const wp_config *config, char *out, size_t size);

bool wp_speed_parse(const char *text, double *out);
double wp_speed_clamp(double value);
bool wp_switch_parse(const char *text, bool *out);
double wp_slider_speed(double position);
double wp_slider_position(double speed);

double wp_rate(const wp_prefs *prefs, const wp_system *system);
bool wp_frozen(const wp_system *system);
wp_want wp_decide(const wp_prefs *prefs, const wp_system *system, bool covered);
wp_want wp_want_merge(wp_want a, wp_want b);

bool wp_is_video_name(const char *name);
bool wp_is_image_name(const char *name);
bool wp_tier_parse(const char *name, wp_tier *tier);
size_t wp_tier_name(const char *prefix, wp_tier tier, char *out, size_t size);
int wp_pick_tier(const wp_tier *tiers, int count, double target_width, double target_height, double rate, wp_fit fit);
bool wp_fitted_tier(wp_tier source, double target_width, double target_height, wp_fit fit, wp_tier *out);
bool wp_fitted_allowed(const wp_system *system);
int wp_loop_repeats(double duration);

wp_rect wp_canvas(const wp_rect *screens, int count);
wp_rect wp_displayed(double image_width, double image_height, wp_rect area, wp_fit fit);
bool wp_covered(wp_rect region, const wp_rect *windows, int count);

void wp_surface_init(wp_surface *surface);
wp_action wp_surface_step(wp_surface *surface, wp_want want, double rate, double now, double release_delay);
void wp_surface_hold(wp_surface *surface, double position);
double wp_surface_deadline(const wp_surface *surface, double release_delay);

uint64_t wp_hash(const char *text);
void wp_choice_init(wp_choice *choice);
bool wp_choice_offer(wp_choice *choice, uint64_t key, double now, double hold, bool immediate);
double wp_choice_deadline(const wp_choice *choice, double hold);

double wp_next_wake(double now, double a, double b);

#endif
