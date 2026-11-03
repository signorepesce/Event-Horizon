#define _GNU_SOURCE
#include "../core/mpv.h"
#include "../core/wallpaper.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP "event_horizon"
#define POWER_POLL 30.0
#define MAX_TIERS 16
#define MAX_RECTS 256
#define SCAN_DELAY 0.3
#define RETRY_DELAY 30.0
#define REFUSED_MAX 8

typedef struct { int screen_number; short x_org, y_org, width, height; } XineramaScreenInfo;
extern Bool XineramaIsActive(Display *display);
extern XineramaScreenInfo *XineramaQueryScreens(Display *display, int *count);

static struct {
    wp_config config;
    char config_path[PATH_MAX];
    char runtime[PATH_MAX];
    char socket_path[PATH_MAX];
    char data_dir[PATH_MAX];
    char variants_dir[PATH_MAX];
    pid_t encoder;
    char encoding[NAME_MAX + 1];
    char encoding_source[PATH_MAX];
    char fit_name[NAME_MAX + 1];
    char fit_source[PATH_MAX];
    wp_tier fit_tier;
    double fit_due;
    char refused[REFUSED_MAX][NAME_MAX + 1];
    int refused_count;
    char tier_paths[MAX_TIERS][PATH_MAX];
    wp_tier tiers[MAX_TIERS];
    int tier_count;
    char playing_path[PATH_MAX];
    double loop_length;
    bool global_concat;
    wp_system system;
    wp_surface surface;
    double last_power;
    double rate;
    int mpv_version;
    pid_t mpv;
    double mpv_started;
    double retry_at;
    bool covered;
    double last_scan;
    double scan_at;
    bool wayland;
    Display *display;
    Window root, window;
    wp_rect canvas;
    wp_rect regions[16];
    int region_count;
    Atom atoms[16];
    int signal_pipe[2];
    volatile sig_atomic_t reload, quit;
} app;

enum { A_TYPE, A_DESKTOP, A_DOCK, A_STATE, A_HIDDEN, A_BELOW, A_STICKY, A_SKIP_TASKBAR, A_SKIP_PAGER,
       A_WM_DESKTOP, A_CURRENT_DESKTOP, A_CLIENT_STACKING, A_CLIENT_LIST, A_WORKAREA, A_ACTIVE, A_COUNT };

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void on_signal(int number) {
    if (number == SIGHUP) app.reload = 1;
    else if (number != SIGCHLD) app.quit = 1;
    int saved = errno;
    ssize_t ignored = write(app.signal_pipe[1], "x", 1);
    (void)ignored;
    errno = saved;
}

static bool read_file(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = '\0';
    return true;
}

static void resolve_paths(void) {
    const char *home = getenv("HOME");
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash = n > 0 ? strrchr(exe, '/') : NULL;
    if (slash) *slash = '\0';
    char base[PATH_MAX], data[PATH_MAX];
    struct stat folder;
    snprintf(data, sizeof data, "%s/data", exe);
    bool local = slash && stat(data, &folder) == 0 && S_ISDIR(folder.st_mode);

    const char *config = getenv("XDG_CONFIG_HOME");
    if (local) snprintf(base, sizeof base, "%s", data);
    else if (config && *config) snprintf(base, sizeof base, "%s/%s", config, APP);
    else snprintf(base, sizeof base, "%s/.config/%s", home ? home : ".", APP);
    mkdir(base, 0755);
    snprintf(app.config_path, sizeof app.config_path, "%s/config", base);

    const char *cache = getenv("XDG_CACHE_HOME");
    if (local) snprintf(base, sizeof base, "%s", data);
    else if (cache && *cache) snprintf(base, sizeof base, "%s/%s", cache, APP);
    else {
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s/.cache", home ? home : ".");
        mkdir(parent, 0755);
        snprintf(base, sizeof base, "%s/%s", parent, APP);
    }
    mkdir(base, 0755);
    snprintf(app.variants_dir, sizeof app.variants_dir, "%s/variants", base);
    mkdir(app.variants_dir, 0755);

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) snprintf(app.runtime, sizeof app.runtime, "%s/%s", runtime, APP);
    else snprintf(app.runtime, sizeof app.runtime, "/tmp/%s-%u", APP, (unsigned)getuid());
    mkdir(app.runtime, 0700);
    struct stat own;
    if (lstat(app.runtime, &own) != 0 || !S_ISDIR(own.st_mode) || own.st_uid != getuid() || (own.st_mode & 077)) {
        fprintf(stderr, "%s is not a private folder of this user; not using it\n", app.runtime);
        exit(1);
    }
    snprintf(app.socket_path, sizeof app.socket_path, "%s/mpv.sock", app.runtime);

    const char *env = getenv("EVENT_HORIZON_DATA");
    char candidates[5][PATH_MAX];
    int count = 0;
    if (env && *env) snprintf(candidates[count++], PATH_MAX, "%s", env);
    if (slash) {
        snprintf(candidates[count++], PATH_MAX, "%s/share", exe);
        snprintf(candidates[count++], PATH_MAX, "%s/../share/%s", exe, APP);
    }
    snprintf(candidates[count++], PATH_MAX, "%s/.local/share/%s", home ? home : ".", APP);
    snprintf(candidates[count++], PATH_MAX, "/usr/share/%s", APP);
    app.data_dir[0] = '\0';
    for (int i = 0; i < count && !app.data_dir[0]; i++) {
        DIR *dir = opendir(candidates[i]);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            wp_tier tier;
            if (wp_tier_parse(entry->d_name, &tier)) {
                snprintf(app.data_dir, sizeof app.data_dir, "%s", candidates[i]);
                break;
            }
        }
        closedir(dir);
    }
}

static void scan_tiers(const char *folder) {
    DIR *dir = opendir(folder);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) && app.tier_count < MAX_TIERS) {
        wp_tier tier;
        if (!wp_tier_parse(entry->d_name, &tier)) continue;
        app.tiers[app.tier_count] = tier;
        snprintf(app.tier_paths[app.tier_count], PATH_MAX, "%s/%s", folder, entry->d_name);
        app.tier_count++;
    }
    closedir(dir);
}

static void forget_stale_variants(void) {
    char stamp[1024] = "", seen[1024] = "", record[PATH_MAX];
    for (int i = 0; i < app.tier_count; i++) {
        struct stat info;
        const char *slash = strrchr(app.tier_paths[i], '/');
        size_t used = strlen(stamp);
        if (stat(app.tier_paths[i], &info) == 0) {
            snprintf(stamp + used, sizeof stamp - used, "%s %lld\n", slash ? slash + 1 : app.tier_paths[i],
                     (long long)info.st_size);
        }
    }
    if (!stamp[0]) return;
    snprintf(record, sizeof record, "%s/source", app.variants_dir);
    FILE *file = fopen(record, "r");
    if (file) {
        seen[fread(seen, 1, sizeof seen - 1, file)] = '\0';
        fclose(file);
    }
    if (strcmp(seen, stamp) == 0) return;
    DIR *dir = opendir(app.variants_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!wp_is_video_name(entry->d_name)) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", app.variants_dir, entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    file = fopen(record, "w");
    if (file) {
        fputs(stamp, file);
        fclose(file);
    }
}

static void load_tiers(void) {
    app.tier_count = 0;
    scan_tiers(app.data_dir);
    forget_stale_variants();
    scan_tiers(app.variants_dir);
}

static int tier_index(const char *path) {
    for (int i = 0; i < app.tier_count; i++) {
        if (strcmp(app.tier_paths[i], path) == 0) return i;
    }
    return -1;
}

static void load_config(void) {
    wp_config_default(&app.config);
    char text[8192];
    if (read_file(app.config_path, text, sizeof text)) wp_config_parse(&app.config, text);
}

static void save_config(void) {
    char text[8192];
    wp_config_format(&app.config, text, sizeof text);
    char temporary[PATH_MAX + 8];
    snprintf(temporary, sizeof temporary, "%s.tmp", app.config_path);
    FILE *f = fopen(temporary, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
    rename(temporary, app.config_path);
}

static void read_power(void) {
    bool mains = false, battery_discharging = false;
    DIR *dir = opendir("/sys/class/power_supply");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char path[PATH_MAX], value[64];
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", entry->d_name);
            if (!read_file(path, value, sizeof value)) continue;
            if (strcmp(value, "Mains") == 0) {
                snprintf(path, sizeof path, "/sys/class/power_supply/%s/online", entry->d_name);
                if (read_file(path, value, sizeof value) && strcmp(value, "1") == 0) mains = true;
            } else if (strcmp(value, "Battery") == 0) {
                snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", entry->d_name);
                if (read_file(path, value, sizeof value) && strcmp(value, "Discharging") == 0) battery_discharging = true;
            }
        }
        closedir(dir);
    }
    app.system.on_battery = battery_discharging && !mains;

    char profile[64];
    app.system.low_power = read_file("/sys/firmware/acpi/platform_profile", profile, sizeof profile) &&
                           (strcmp(profile, "low-power") == 0 || strcmp(profile, "quiet") == 0);

    long hottest = 0;
    dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
            char path[PATH_MAX], value[64];
            snprintf(path, sizeof path, "/sys/class/thermal/%s/temp", entry->d_name);
            if (read_file(path, value, sizeof value)) {
                long milli = strtol(value, NULL, 10);
                if (milli > hottest && milli < 150000) hottest = milli;
            }
        }
        closedir(dir);
    }
    app.system.hot = hottest >= 90000;
    app.last_power = now();
}

static int ipc_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool ipc_request(const char *path, const char *command, int request_id, char *reply, size_t size) {
    int fd = ipc_connect(path);
    if (fd < 0) return false;
    size_t length = strlen(command);
    bool ok = write(fd, command, length) == (ssize_t)length;
    if (ok && request_id > 0 && reply) {
        ok = false;
        char buffer[4096];
        size_t used = 0;
        double deadline = now() + 0.5;
        while (!ok && now() < deadline) {
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            ssize_t n = read(fd, buffer + used, sizeof buffer - 1 - used);
            if (n <= 0) break;
            used += (size_t)n;
            buffer[used] = '\0';
            char *line = buffer, *newline;
            while ((newline = strchr(line, '\n'))) {
                *newline = '\0';
                double id;
                if (wp_json_number(line, "request_id", &id) && (int)id == request_id) {
                    snprintf(reply, size, "%s", line);
                    ok = true;
                    break;
                }
                line = newline + 1;
            }
            if (!ok) {
                size_t rest = strlen(line);
                memmove(buffer, line, rest + 1);
                used = rest;
            }
        }
    }
    close(fd);
    return ok;
}

static void ipc_broadcast(const char *command) {
    ipc_request(app.socket_path, command, 0, NULL, 0);
    DIR *dir = opendir(app.runtime);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "mpv.sock-", 9) != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", app.runtime, entry->d_name);
        ipc_request(path, command, 0, NULL, 0);
    }
    closedir(dir);
}

static double ipc_number(const char *property) {
    const char *argv[] = {"get_property", property};
    char command[256], reply[4096];
    wp_mpv_command(command, sizeof command, 7, argv, 2);
    double value = -1;
    if (ipc_request(app.socket_path, command, 7, reply, sizeof reply)) wp_json_number(reply, "data", &value);
    return value;
}

static int ipc_version(void) {
    const char *argv[] = {"get_property", "mpv-version"};
    char command[256], reply[4096], text[128];
    wp_mpv_command(command, sizeof command, 8, argv, 2);
    if (ipc_request(app.socket_path, command, 8, reply, sizeof reply) &&
        wp_json_string(reply, "data", text, sizeof text)) return wp_mpv_version(text);
    return 0;
}

static bool in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return false;
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", path);
    for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, name);
        if (access(full, X_OK) == 0) return true;
    }
    return false;
}

static wp_rect drm_canvas(void) {
    wp_rect best = {0, 0, 1920, 1080};
    double best_pixels = 0;
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return best;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strchr(entry->d_name, '-')) continue;
        char path[PATH_MAX], value[64];
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", entry->d_name);
        if (!read_file(path, value, sizeof value) || strcmp(value, "connected") != 0) continue;
        snprintf(path, sizeof path, "/sys/class/drm/%s/modes", entry->d_name);
        int w = 0, h = 0;
        if (read_file(path, value, sizeof value) && sscanf(value, "%dx%d", &w, &h) == 2 && (double)w * h > best_pixels) {
            best_pixels = (double)w * h;
            best = (wp_rect){0, 0, w, h};
        }
    }
    closedir(dir);
    return best;
}

static bool choose_content(char *path, size_t size, bool *image) {
    *image = false;
    const wp_config *c = &app.config;
    if ((c->content == WP_CONTENT_VIDEO || c->content == WP_CONTENT_IMAGE) && access(c->path, R_OK) == 0) {
        snprintf(path, size, "%s", c->path);
        *image = c->content == WP_CONTENT_IMAGE;
        return true;
    }
    int index = wp_pick_tier(app.tiers, app.tier_count, app.canvas.w, app.canvas.h, app.rate, app.config.prefs.fit);
    if (index < 0) return false;
    snprintf(path, size, "%s", app.tier_paths[index]);
    return true;
}

static bool write_out(void *context, const char *data, size_t size) {
    return fwrite(data, 1, size, context) == size;
}

static size_t read_in(void *context, uint64_t offset, void *buffer, size_t size) {
    if (fseeko(context, (off_t)offset, SEEK_SET) != 0) return 0;
    return fread(buffer, 1, size, context);
}

static double video_length(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    double length = -1;
    if (fseeko(file, 0, SEEK_END) == 0) {
        off_t end = ftello(file);
        if (end > 0) length = wp_mp4_duration(read_in, file, (uint64_t)end);
    }
    fclose(file);
    return length;
}

static bool prepare_source(const char *path, bool image, char *source, size_t size) {
    snprintf(source, size, "%s", path);
    app.loop_length = image ? -1 : video_length(path);
    if (app.loop_length <= 0) return false;
    char list[PATH_MAX], temp[PATH_MAX];
    snprintf(list, sizeof list, "%s/loop-%016llx.ffconcat", app.runtime, (unsigned long long)wp_hash(path));
    snprintf(temp, sizeof temp, "%s.tmp", list);
    FILE *file = fopen(temp, "w");
    if (!file) return false;
    bool ok = wp_mpv_concat_write(write_out, file, path, app.loop_length) > 0;
    ok = fclose(file) == 0 && ok;
    if (!ok || rename(temp, list) != 0) {
        unlink(temp);
        return false;
    }
    snprintf(source, size, "%s", list);
    return true;
}

static double loop_position(void) {
    double position = ipc_number("time-pos");
    if (position > 0 && app.loop_length > 0) position = fmod(position, app.loop_length);
    return position;
}

static void stop_mpv(void) {
    if (app.mpv > 0) {
        kill(app.mpv, SIGTERM);
        for (int i = 0; i < 20 && waitpid(app.mpv, NULL, WNOHANG) == 0; i++) usleep(25000);
        if (waitpid(app.mpv, NULL, WNOHANG) == 0) {
            kill(app.mpv, SIGKILL);
            waitpid(app.mpv, NULL, 0);
        }
    }
    app.mpv = 0;
    app.playing_path[0] = '\0';
    unlink(app.socket_path);
}

static void join_option(char *out, size_t size, const char *text) {
    size_t used = strlen(out);
    if (used + 1 < size) snprintf(out + used, size - used, "%s%s", used ? " " : "", text);
}

static void start_mpv(const char *path, bool image, double start) {
    stop_mpv();
    char source[PATH_MAX];
    bool concat = prepare_source(path, image, source, sizeof source);
    char start_option[48] = "", format_option[64];
    if (start > 0) snprintf(start_option, sizeof start_option, "%sstart=%.3f", app.wayland ? "" : "--", start);
    snprintf(format_option, sizeof format_option, "%s%s", app.wayland ? "" : "--", WP_MPV_CONCAT_FORMAT);
    char window[32] = "";
    if (!app.wayland) snprintf(window, sizeof window, "%lu", (unsigned long)app.window);
    wp_mpv_options options;
    wp_mpv_build_options(&options, WP_MPV_LINUX, app.config.prefs.fit, app.rate, false, app.socket_path, window);
    char *argv[WP_MPV_OPTIONS_MAX + 16];
    int argc = 0;
    char joined[WP_MPV_OPTIONS_MAX * WP_MPV_OPTION_MAX];
    if (app.wayland) {
        joined[0] = '\0';
        for (int i = 0; i < options.count; i++) {
            const char *item = options.items[i];
            if (strncmp(item, "--wid=", 6) == 0 || strcmp(item, "--no-terminal") == 0) continue;
            join_option(joined, sizeof joined, item + 2);
        }
        if (image) join_option(joined, sizeof joined, "image-display-duration=inf");
        if (concat) join_option(joined, sizeof joined, format_option);
        if (start_option[0]) join_option(joined, sizeof joined, start_option);
        argv[argc++] = "mpvpaper";
        argv[argc++] = "--auto-pause";
        argv[argc++] = "-o";
        argv[argc++] = joined;
        argv[argc++] = "ALL";
    } else {
        argv[argc++] = "mpv";
        for (int i = 0; i < options.count; i++) argv[argc++] = options.items[i];
        if (image) argv[argc++] = "--image-display-duration=inf";
        if (start_option[0]) argv[argc++] = start_option;
        if (concat) {
            argv[argc++] = "--{";
            argv[argc++] = format_option;
        } else {
            argv[argc++] = "--";
        }
    }
    argv[argc++] = source;
    if (concat && !app.wayland) argv[argc++] = "--}";
    argv[argc] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) return;
    app.mpv = pid;
    app.mpv_started = now();
    app.global_concat = concat && app.wayland;
    snprintf(app.playing_path, sizeof app.playing_path, "%s", path);
    for (int i = 0; i < 40 && access(app.socket_path, F_OK) != 0; i++) usleep(25000);
    app.mpv_version = ipc_version();
}

static void switch_file(const char *path, bool image, bool keep_time) {
    if (app.mpv <= 0) return;
    double position = keep_time ? loop_position() : 0;
    if (position < 0) position = 0;
    char source[PATH_MAX];
    bool concat = prepare_source(path, image, source, sizeof source);
    if (app.global_concat && !concat) {
        start_mpv(path, image, position);
        return;
    }
    char command[WP_MPV_COMMAND_MAX];
    wp_mpv_set(command, sizeof command, "image-display-duration", image ? "inf" : "5");
    ipc_broadcast(command);
    wp_mpv_loadfile(command, sizeof command, source, position, app.mpv_version, concat);
    ipc_broadcast(command);
    snprintf(app.playing_path, sizeof app.playing_path, "%s", path);
}

static void send_rate(double rate) {
    char value[32], command[256];
    snprintf(value, sizeof value, "%.4f", rate);
    wp_mpv_set(command, sizeof command, "speed", value);
    ipc_broadcast(command);
}

static void send_pause(bool paused) {
    char command[256];
    wp_mpv_set(command, sizeof command, "pause", paused ? "yes" : "no");
    ipc_broadcast(command);
}

static int ignore_x_errors(Display *display, XErrorEvent *event) {
    (void)display;
    (void)event;
    return 0;
}

static unsigned char *property(Window window, Atom name, Atom type, unsigned long *count) {
    Atom actual;
    int format;
    unsigned long after;
    unsigned char *data = NULL;
    *count = 0;
    if (XGetWindowProperty(app.display, window, name, 0, 4096, False, type, &actual, &format, count, &after, &data) != Success) {
        *count = 0;
        return NULL;
    }
    return data;
}

static bool has_atom(Window window, Atom list, Atom wanted) {
    unsigned long count;
    Atom *atoms = (Atom *)property(window, list, XA_ATOM, &count);
    bool found = false;
    for (unsigned long i = 0; atoms && i < count; i++) found = found || atoms[i] == wanted;
    if (atoms) XFree(atoms);
    return found;
}

static long cardinal(Window window, Atom name, long fallback) {
    unsigned long count;
    long *value = (long *)property(window, name, XA_CARDINAL, &count);
    long result = value && count > 0 ? value[0] : fallback;
    if (value) XFree(value);
    return result;
}

static void x11_geometry(void) {
    int screen = DefaultScreen(app.display);
    app.canvas = (wp_rect){0, 0, DisplayWidth(app.display, screen), DisplayHeight(app.display, screen)};
    wp_rect work = app.canvas;
    unsigned long count;
    long current = cardinal(app.root, app.atoms[A_CURRENT_DESKTOP], 0);
    long *area = (long *)property(app.root, app.atoms[A_WORKAREA], XA_CARDINAL, &count);
    if (area && (unsigned long)(current * 4 + 3) < count) {
        work = (wp_rect){(double)area[current * 4], (double)area[current * 4 + 1],
                         (double)area[current * 4 + 2], (double)area[current * 4 + 3]};
    }
    if (area) XFree(area);
    app.region_count = 0;
    int monitors = 0;
    XineramaScreenInfo *info = XineramaIsActive(app.display) ? XineramaQueryScreens(app.display, &monitors) : NULL;
    for (int i = 0; i < monitors && app.region_count < 16; i++) {
        double x0 = info[i].x_org > work.x ? info[i].x_org : work.x;
        double y0 = info[i].y_org > work.y ? info[i].y_org : work.y;
        double x1 = info[i].x_org + info[i].width < work.x + work.w ? info[i].x_org + info[i].width : work.x + work.w;
        double y1 = info[i].y_org + info[i].height < work.y + work.h ? info[i].y_org + info[i].height : work.y + work.h;
        if (x1 > x0 && y1 > y0) app.regions[app.region_count++] = (wp_rect){x0, y0, x1 - x0, y1 - y0};
    }
    if (info) XFree(info);
    if (app.region_count == 0) app.regions[app.region_count++] = work;
}

static bool x11_covered(void) {
    long current = cardinal(app.root, app.atoms[A_CURRENT_DESKTOP], 0);
    unsigned long count;
    Window *list = (Window *)property(app.root, app.atoms[A_CLIENT_STACKING], XA_WINDOW, &count);
    if (!list) list = (Window *)property(app.root, app.atoms[A_CLIENT_LIST], XA_WINDOW, &count);
    wp_rect rects[MAX_RECTS];
    int n = 0;
    for (unsigned long i = 0; list && i < count && n < MAX_RECTS; i++) {
        Window w = list[i];
        if (w == app.window) continue;
        if (has_atom(w, app.atoms[A_TYPE], app.atoms[A_DESKTOP]) || has_atom(w, app.atoms[A_TYPE], app.atoms[A_DOCK])) continue;
        if (has_atom(w, app.atoms[A_STATE], app.atoms[A_HIDDEN])) continue;
        long desk = cardinal(w, app.atoms[A_WM_DESKTOP], current);
        if (desk != current && desk != 0xFFFFFFFFL && desk != -1) continue;
        XWindowAttributes a;
        if (!XGetWindowAttributes(app.display, w, &a) || a.map_state != IsViewable) continue;
        int x, y;
        Window child;
        if (!XTranslateCoordinates(app.display, w, app.root, 0, 0, &x, &y, &child)) continue;
        rects[n++] = (wp_rect){x, y, a.width, a.height};
    }
    if (list) XFree(list);
    for (int i = 0; i < app.region_count; i++) {
        if (!wp_covered(app.regions[i], rects, n)) return false;
    }
    return true;
}

static bool x11_display_off(void) {
    int event, error;
    CARD16 level;
    BOOL enabled;
    return DPMSQueryExtension(app.display, &event, &error) && DPMSCapable(app.display) &&
           DPMSInfo(app.display, &level, &enabled) && enabled && level != DPMSModeOn;
}

static bool x11_open(void) {
    app.display = XOpenDisplay(NULL);
    if (!app.display) return false;
    XSetErrorHandler(ignore_x_errors);
    char *names[A_COUNT] = {"_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_BELOW", "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER", "_NET_WM_DESKTOP", "_NET_CURRENT_DESKTOP",
        "_NET_CLIENT_LIST_STACKING", "_NET_CLIENT_LIST", "_NET_WORKAREA", "_NET_ACTIVE_WINDOW"};
    XInternAtoms(app.display, names, A_COUNT, False, app.atoms);
    int screen = DefaultScreen(app.display);
    app.root = RootWindow(app.display, screen);
    x11_geometry();
    XSetWindowAttributes attributes = {0};
    attributes.background_pixel = BlackPixel(app.display, screen);
    attributes.event_mask = StructureNotifyMask;
    app.window = XCreateWindow(app.display, app.root, 0, 0, (unsigned)app.canvas.w, (unsigned)app.canvas.h, 0,
                               CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &attributes);
    XChangeProperty(app.display, app.window, app.atoms[A_TYPE], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&app.atoms[A_DESKTOP], 1);
    Atom states[] = {app.atoms[A_BELOW], app.atoms[A_STICKY], app.atoms[A_SKIP_TASKBAR], app.atoms[A_SKIP_PAGER]};
    XChangeProperty(app.display, app.window, app.atoms[A_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 4);
    long every = 0xFFFFFFFFL;
    XChangeProperty(app.display, app.window, app.atoms[A_WM_DESKTOP], XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&every, 1);
    XWMHints hints = {.flags = InputHint, .input = False};
    XSetWMHints(app.display, app.window, &hints);
    XStoreName(app.display, app.window, APP);
    XMapWindow(app.display, app.window);
    XLowerWindow(app.display, app.window);
    XSelectInput(app.display, app.root, PropertyChangeMask | StructureNotifyMask);
    XFlush(app.display);
    return true;
}

static bool current_covered(void) {
    double t = now();
    if (t - app.last_scan >= wp_coverage_poll(app.covered) - 0.05 || (app.scan_at >= 0 && t >= app.scan_at)) {
        app.covered = x11_covered();
        app.last_scan = t;
        app.scan_at = -1;
    }
    return app.covered;
}

static bool is_refused(const char *name) {
    for (int i = 0; i < app.refused_count; i++) {
        if (strcmp(app.refused[i], name) == 0) return true;
    }
    return false;
}

static void refuse(const char *name) {
    if (app.refused_count < REFUSED_MAX && !is_refused(name))
        snprintf(app.refused[app.refused_count++], NAME_MAX + 1, "%s", name);
}

static void consider_fitted(const char *path) {
    int index = tier_index(path);
    wp_tier tier;
    char name[NAME_MAX + 1];
    if (app.config.content != WP_CONTENT_BUILTIN || index < 0 || !wp_fitted_allowed(&app.system) ||
        !wp_fitted_tier(app.tiers[index], app.canvas.w, app.canvas.h, app.config.prefs.fit, &tier) ||
        wp_tier_name(WP_SCENE, tier, name, sizeof name) == 0) {
        app.fit_name[0] = '\0';
        return;
    }
    char destination[PATH_MAX];
    snprintf(destination, sizeof destination, "%s/%s", app.variants_dir, name);
    if (app.encoder > 0 || strcmp(app.fit_name, name) == 0 || is_refused(name) || access(destination, F_OK) == 0) return;
    snprintf(app.fit_name, sizeof app.fit_name, "%s", name);
    snprintf(app.fit_source, sizeof app.fit_source, "%s", app.tier_paths[index]);
    app.fit_tier = tier;
    app.fit_due = now() + WP_FIT_DELAY;
}

static void start_encoder(void) {
    char temp[PATH_MAX];
    snprintf(temp, sizeof temp, "%s/tmp", app.variants_dir);
    mkdir(temp, 0755);
    snprintf(temp, sizeof temp, "%s/tmp/%s", app.variants_dir, app.fit_name);
    unlink(temp);
    static wp_mpv_options options;
    wp_mpv_encode_options(&options, app.fit_tier, temp);
    char *argv[WP_MPV_OPTIONS_MAX + 4];
    int argc = 0;
    argv[argc++] = "mpv";
    for (int i = 0; i < options.count; i++) argv[argc++] = options.items[i];
    argv[argc++] = "--";
    argv[argc++] = app.fit_source;
    argv[argc] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        setpriority(PRIO_PROCESS, 0, 19);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) {
        refuse(app.fit_name);
    } else {
        app.encoder = pid;
        snprintf(app.encoding, sizeof app.encoding, "%s", app.fit_name);
        snprintf(app.encoding_source, sizeof app.encoding_source, "%s", app.fit_source);
    }
    app.fit_name[0] = '\0';
}

static void prune_variants(const char *keep) {
    DIR *dir = opendir(app.variants_dir);
    if (!dir) return;
    char paths[64][PATH_MAX];
    time_t times[64];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < 64) {
        if (!wp_is_video_name(entry->d_name)) continue;
        snprintf(paths[count], PATH_MAX, "%s/%s", app.variants_dir, entry->d_name);
        struct stat info;
        if (strcmp(paths[count], keep) == 0 || stat(paths[count], &info) != 0) continue;
        times[count++] = info.st_mtime;
    }
    closedir(dir);
    for (int kept = 1; kept < WP_FITTED_LIMIT && count > 0; kept++) {
        int newest = 0;
        for (int i = 1; i < count; i++) {
            if (times[i] > times[newest]) newest = i;
        }
        times[newest] = times[count - 1];
        memcpy(paths[newest], paths[count - 1], PATH_MAX);
        count--;
    }
    for (int i = 0; i < count; i++) unlink(paths[i]);
}

static void finish_encoder(int status) {
    char temp[PATH_MAX], destination[PATH_MAX];
    snprintf(temp, sizeof temp, "%s/tmp/%s", app.variants_dir, app.encoding);
    snprintf(destination, sizeof destination, "%s/%s", app.variants_dir, app.encoding);
    double expected = video_length(app.encoding_source), produced = video_length(temp);
    bool exited = WIFEXITED(status);
    if (exited && WEXITSTATUS(status) == 0 && expected > 0 && fabs(produced - expected) < 0.01 &&
        rename(temp, destination) == 0) {
        prune_variants(destination);
        load_tiers();
    } else {
        unlink(temp);
        if (exited) refuse(app.encoding);
        fprintf(stderr, "fitted variant %s was not made\n", app.encoding);
    }
    app.encoder = 0;
    app.encoding[0] = '\0';
}

static void stop_encoder(void) {
    if (app.encoder <= 0) return;
    kill(app.encoder, SIGTERM);
    int status = 0;
    waitpid(app.encoder, &status, 0);
    finish_encoder(status);
}

static void apply_state(void) {
    double rate = wp_rate(&app.config.prefs, &app.system);
    app.rate = rate;
    if (app.encoder > 0 && !wp_fitted_allowed(&app.system)) stop_encoder();
    bool covered = app.config.prefs.pause_when_covered && !app.wayland && !wp_frozen(&app.system) && current_covered();
    wp_want want = wp_decide(&app.config.prefs, &app.system, covered);
    if (want == WP_WANT_PLAY && app.mpv <= 0 && now() < app.retry_at) want = WP_WANT_RELEASE;
    wp_action action = wp_surface_step(&app.surface, want, rate, now(), WP_RELEASE_DELAY);

    char path[PATH_MAX];
    bool image = false;
    bool have = choose_content(path, sizeof path, &image);
    if (action.build && have && app.mpv <= 0) start_mpv(path, image, action.start);
    else if (have && app.mpv > 0 && strcmp(path, app.playing_path) != 0) {
        switch_file(path, image, tier_index(app.playing_path) >= 0 && tier_index(path) >= 0);
    }
    if (have) consider_fitted(path);
    if (action.play && app.mpv > 0) {
        send_rate(action.rate);
        send_pause(false);
    }
    if (action.pause || action.release) send_pause(true);
    bool unseen = app.system.display_asleep || app.system.session_inactive;
    if (app.surface.state == WP_RELEASED && app.mpv > 0 && unseen) {
        wp_surface_hold(&app.surface, loop_position());
        stop_mpv();
    }
}

static double next_wake(void) {
    double t = now();
    double next = app.last_power + POWER_POLL;
    if (!app.wayland && app.config.prefs.pause_when_covered && (!wp_frozen(&app.system) || app.system.display_asleep)) {
        next = wp_next_wake(t, next, app.last_scan + wp_coverage_poll(app.covered));
        next = wp_next_wake(t, next, app.scan_at);
    }
    next = wp_next_wake(t, next, wp_surface_deadline(&app.surface, WP_RELEASE_DELAY));
    if (app.mpv <= 0 && app.retry_at > t) next = wp_next_wake(t, next, app.retry_at);
    if (app.fit_name[0] && app.encoder <= 0) next = wp_next_wake(t, next, app.fit_due);
    return next;
}

static pid_t running_instance(void) {
    char path[PATH_MAX], text[32];
    snprintf(path, sizeof path, "%s/pid", app.runtime);
    if (!read_file(path, text, sizeof text)) return 0;
    pid_t pid = (pid_t)strtol(text, NULL, 10);
    if (pid <= 0 || kill(pid, 0) != 0) return 0;
    char name[32];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    return read_file(path, name, sizeof name) && strcmp(name, APP) == 0 ? pid : 0;
}

static int control(int argc, char **argv) {
    load_config();
    bool quit = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(arg, "--speed") == 0 && value) {
            double speed;
            if (!wp_speed_parse(value, &speed)) {
                fprintf(stderr, "invalid speed: %s\n", value);
                return 2;
            }
            app.config.prefs.speed = wp_speed_clamp(speed);
            i++;
        } else if (strcmp(arg, "--fit") == 0 && value) {
            if (strcmp(value, "fill") != 0 && strcmp(value, "fit") != 0) {
                fprintf(stderr, "fit is fill or fit\n");
                return 2;
            }
            char line[64];
            snprintf(line, sizeof line, "fit = %s", value);
            wp_config_parse(&app.config, line);
            i++;
        } else if ((strcmp(arg, "--pause-when-covered") == 0 || strcmp(arg, "--half-on-battery") == 0 ||
                    strcmp(arg, "--half-in-low-power") == 0) && value) {
            bool on;
            if (!wp_switch_parse(value, &on)) {
                fprintf(stderr, "%s is on or off\n", arg);
                return 2;
            }
            if (strcmp(arg, "--pause-when-covered") == 0) app.config.prefs.pause_when_covered = on;
            else if (strcmp(arg, "--half-on-battery") == 0) app.config.prefs.half_speed_on_battery = on;
            else app.config.prefs.half_speed_in_low_power = on;
            i++;
        } else if (strcmp(arg, "--set") == 0 && value) {
            char resolved[PATH_MAX];
            struct stat info;
            if (strcmp(value, "builtin") == 0) {
                app.config.content = WP_CONTENT_BUILTIN;
                app.config.path[0] = '\0';
            } else if (realpath(value, resolved) && stat(resolved, &info) == 0 && S_ISREG(info.st_mode)) {
                snprintf(app.config.path, sizeof app.config.path, "%s", resolved);
                app.config.content = wp_is_image_name(resolved) ? WP_CONTENT_IMAGE : WP_CONTENT_VIDEO;
            } else {
                fprintf(stderr, "cannot read: %s\n", value);
                return 2;
            }
            i++;
        } else if (strcmp(arg, "--quit") == 0) {
            quit = true;
        } else if (strcmp(arg, "--pause") != 0 && strcmp(arg, "--resume") != 0) {
            fprintf(stderr, "usage: %s [--speed N] [--fit fill|fit] [--pause-when-covered on|off]\n"
                            "       [--half-on-battery on|off] [--half-in-low-power on|off]\n"
                            "       [--set FILE|builtin] [--pause|--resume] [--quit]\n", APP);
            return 2;
        }
    }
    save_config();
    pid_t pid = running_instance();
    if (!pid) return 0;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/paused", app.runtime);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pause") == 0) close(open(path, O_CREAT | O_WRONLY, 0600));
        if (strcmp(argv[i], "--resume") == 0) unlink(path);
    }
    kill(pid, quit ? SIGTERM : SIGHUP);
    return 0;
}

int main(int argc, char **argv) {
    resolve_paths();
    if (argc > 1) return control(argc, argv);
    if (running_instance()) {
        fprintf(stderr, "%s is already running\n", APP);
        return 1;
    }
    char pid_path[PATH_MAX], pause_path[PATH_MAX];
    snprintf(pid_path, sizeof pid_path, "%s/pid", app.runtime);
    snprintf(pause_path, sizeof pause_path, "%s/paused", app.runtime);
    FILE *pid_file = fopen(pid_path, "w");
    if (pid_file) {
        fprintf(pid_file, "%d\n", (int)getpid());
        fclose(pid_file);
    }

    if (pipe2(app.signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return 1;
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGCHLD, &action, NULL);

    load_config();
    load_tiers();
    if (app.tier_count == 0) {
        fprintf(stderr, "no built-in videos found (EVENT_HORIZON_DATA or ~/.local/share/%s); "
                        "use %s --set FILE to show your own\n", APP, APP);
    }
    const char *wayland = getenv("WAYLAND_DISPLAY");
    app.wayland = wayland && *wayland;
    const char *player = app.wayland ? "mpvpaper" : "mpv";
    if (!in_path(player)) {
        fprintf(stderr, "%s needs %s; install it with your package manager\n", APP, player);
        return 1;
    }
    if (!app.wayland && !x11_open()) {
        fprintf(stderr, "no X11 or Wayland display\n");
        return 1;
    }
    if (app.wayland) app.canvas = drm_canvas();
    app.last_scan = -1e9;
    app.scan_at = -1;
    wp_surface_init(&app.surface);
    app.rate = -1;
    app.system.manual_pause = access(pause_path, F_OK) == 0;
    read_power();
    apply_state();

    while (!app.quit) {
        double wake = next_wake();
        int timeout = wake < 0 ? -1 : (int)((wake - now()) * 1000.0) + 1;
        if (timeout < 0 && wake >= 0) timeout = 0;
        struct pollfd fds[2] = {{app.signal_pipe[0], POLLIN, 0}, {-1, POLLIN, 0}};
        if (app.display) fds[1].fd = ConnectionNumber(app.display);
        if (!app.display || XPending(app.display) == 0) poll(fds, app.display ? 2 : 1, timeout);
        char drain[64];
        while (read(app.signal_pipe[0], drain, sizeof drain) > 0) {}
        int status = 0;
        if (app.encoder > 0 && waitpid(app.encoder, &status, WNOHANG) == app.encoder) finish_encoder(status);
        if (app.mpv > 0 && waitpid(app.mpv, NULL, WNOHANG) == app.mpv) {
            if (now() - app.mpv_started < 3.0 && app.config.content != WP_CONTENT_BUILTIN) {
                fprintf(stderr, "%s cannot be played; back to the built-in background\n", app.config.path);
                app.config.content = WP_CONTENT_BUILTIN;
                app.config.path[0] = '\0';
                save_config();
            } else if (now() - app.mpv_started < 3.0) {
                app.retry_at = now() + RETRY_DELAY;
                fprintf(stderr, "%s exited right after starting; retrying in %.0f s\n", app.wayland ? "mpvpaper" : "mpv", RETRY_DELAY);
            }
            app.mpv = 0;
            app.playing_path[0] = '\0';
            wp_surface_init(&app.surface);
        }
        bool rescan = false;
        while (app.display && XPending(app.display)) {
            XEvent event;
            XNextEvent(app.display, &event);
            if (event.type == ConfigureNotify && event.xconfigure.window == app.root) {
                x11_geometry();
                XMoveResizeWindow(app.display, app.window, 0, 0, (unsigned)app.canvas.w, (unsigned)app.canvas.h);
            }
            if (event.type == PropertyNotify) {
                Atom atom = event.xproperty.atom;
                if (atom == app.atoms[A_WORKAREA] || atom == app.atoms[A_CURRENT_DESKTOP]) x11_geometry();
                if (atom == app.atoms[A_CLIENT_STACKING] || atom == app.atoms[A_CLIENT_LIST] ||
                    atom == app.atoms[A_ACTIVE] || atom == app.atoms[A_CURRENT_DESKTOP] || atom == app.atoms[A_WORKAREA])
                    rescan = true;
            } else if (event.type == ConfigureNotify) {
                rescan = true;
            }
        }
        if (rescan && app.scan_at < 0) app.scan_at = now() + SCAN_DELAY;
        if (app.reload) {
            app.reload = 0;
            load_config();
            app.system.manual_pause = access(pause_path, F_OK) == 0;
            app.scan_at = now();
        }
        double t = now();
        if (t - app.last_power >= POWER_POLL) read_power();
        if (app.display) app.system.display_asleep = x11_display_off();
        if (app.fit_name[0] && app.encoder <= 0 && t >= app.fit_due && wp_fitted_allowed(&app.system)) start_encoder();
        apply_state();
    }
    stop_encoder();
    stop_mpv();
    unlink(pid_path);
    if (app.display) XCloseDisplay(app.display);
    return 0;
}
