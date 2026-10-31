#include "mpv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add(wp_mpv_options *options, const char *option, const char *value) {
    if (options->count >= WP_MPV_OPTIONS_MAX) return;
    snprintf(options->items[options->count++], WP_MPV_OPTION_MAX, "%s%s", option, value ? value : "");
}

void wp_mpv_build_options(wp_mpv_options *options, wp_mpv_platform platform, wp_fit fit,
                          double rate, bool paused, const char *ipc, const char *window) {
    static const char *const fixed[] = {
        "--no-config", "--really-quiet", "--no-terminal", "--force-window=yes", "--keep-open=no",
        "--loop-file=inf", "--no-audio", "--mute=yes", "--hwdec=auto-safe", "--vo=gpu",
        "--gpu-dumb-mode=yes", "--scale=bilinear", "--dscale=bilinear", "--cscale=bilinear",
        "--interpolation=no", "--deband=no", "--video-sync=audio", "--framedrop=vo",
        "--cache=no", "--demuxer-max-bytes=16MiB", "--demuxer-readahead-secs=2", "--demuxer-lavf-o=safe=0",
        "--osc=no", "--osd-level=0", "--no-osd-bar", "--input-default-bindings=no",
        "--input-vo-keyboard=no", "--input-cursor=no", "--cursor-autohide=always",
        "--border=no", "--title=event_horizon",
    };
    options->count = 0;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) add(options, fixed[i], NULL);
    if (platform == WP_MPV_WINDOWS) {
        add(options, "--gpu-api=d3d11", NULL);
        add(options, "--d3d11-flip=yes", NULL);
    }
    add(options, fit == WP_FIT_FIT ? "--panscan=0.0" : "--panscan=1.0", NULL);
    char speed[32];
    snprintf(speed, sizeof speed, "%.4f", rate > 0 ? rate : 1.0);
    add(options, "--speed=", speed);
    if (paused) add(options, "--pause=yes", NULL);
    if (ipc && *ipc) add(options, "--input-ipc-server=", ipc);
    if (window && *window) add(options, "--wid=", window);
}

void wp_mpv_encode_options(wp_mpv_options *options, wp_tier target, const char *output) {
    static const char *const fixed[] = {
        "--no-config", "--really-quiet", "--no-terminal", "--no-audio", "--of=mp4", "--ofopts=use_editlist=0",
        "--ovc=libx264",
    };
    options->count = 0;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) add(options, fixed[i], NULL);
    char value[WP_MPV_OPTION_MAX];
    snprintf(value, sizeof value, "crf=18,preset=medium,bf=0,g=%d", target.fps > 0 ? target.fps * 2 : 60);
    add(options, "--ovcopts=", value);
    snprintf(value, sizeof value,
             "lavfi=[scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,crop=%d:%d,format=yuv420p]",
             target.width, target.height, target.width, target.height);
    add(options, "--vf=", value);
    if (output && *output) add(options, "--o=", output);
}

static size_t json_escape(const char *in, char *out, size_t size) {
    size_t n = 0;
    if (size == 0) return 0;
    for (const unsigned char *c = (const unsigned char *)in; c && *c; c++) {
        char buffer[8];
        const char *piece = buffer;
        if (*c == '"') piece = "\\\"";
        else if (*c == '\\') piece = "\\\\";
        else if (*c == '\n') piece = "\\n";
        else if (*c == '\r') piece = "\\r";
        else if (*c == '\t') piece = "\\t";
        else if (*c < 0x20) snprintf(buffer, sizeof buffer, "\\u%04x", *c);
        else { buffer[0] = (char)*c; buffer[1] = '\0'; }
        size_t length = strlen(piece);
        if (n + length >= size) break;
        memcpy(out + n, piece, length);
        n += length;
    }
    out[n] = '\0';
    return n;
}

size_t wp_mpv_command(char *out, size_t size, int request_id, const char *const *argv, int argc) {
    size_t n = 0;
    int written = snprintf(out, size, "{\"command\":[");
    if (written < 0 || (size_t)written >= size) return 0;
    n = (size_t)written;
    for (int i = 0; i < argc; i++) {
        char escaped[WP_PATH_MAX * 2];
        json_escape(argv[i], escaped, sizeof escaped);
        written = snprintf(out + n, size - n, "%s\"%s\"", i ? "," : "", escaped);
        if (written < 0 || (size_t)written >= size - n) return 0;
        n += (size_t)written;
    }
    written = request_id > 0 ? snprintf(out + n, size - n, "],\"request_id\":%d}\n", request_id)
                             : snprintf(out + n, size - n, "]}\n");
    if (written < 0 || (size_t)written >= size - n) return 0;
    return n + (size_t)written;
}

size_t wp_mpv_set(char *out, size_t size, const char *property, const char *value) {
    const char *argv[] = {"set_property", property, value};
    return wp_mpv_command(out, size, 0, argv, 3);
}

size_t wp_mpv_loadfile(char *out, size_t size, const char *path, double start, int version, bool concat) {
    char options[96];
    snprintf(options, sizeof options, "%s%sstart=%.3f", concat ? WP_MPV_CONCAT_FORMAT : "", concat ? "," : "",
             start > 0 ? start : 0.0);
    if (version >= 38) {
        const char *argv[] = {"loadfile", path, "replace", "-1", options};
        return wp_mpv_command(out, size, 0, argv, 5);
    }
    const char *argv[] = {"loadfile", path, "replace", options};
    return wp_mpv_command(out, size, 0, argv, 4);
}

static size_t concat_entry(const char *path, double duration, char *out, size_t size) {
    if (!path || !*path || strpbrk(path, "\r\n") || size == 0) return 0;
    size_t n = 0;
    const char *prefix = "file '";
    for (const char *c = prefix; *c && n + 1 < size; c++) out[n++] = *c;
    for (const char *c = path; *c; c++) {
        const char *piece = *c == '\'' ? "'\\''" : NULL;
        size_t length = piece ? strlen(piece) : 1;
        if (n + length + 1 >= size) return 0;
        if (piece) memcpy(out + n, piece, length);
        else out[n] = *c;
        n += length;
    }
    int written = duration > 0 ? snprintf(out + n, size - n, "'\nduration %.6f\n", duration)
                               : snprintf(out + n, size - n, "'\n");
    if (written < 0 || (size_t)written >= size - n) return 0;
    return n + (size_t)written;
}

int wp_mpv_concat_write(wp_write_fn write, void *context, const char *path, double duration) {
    int repeats = wp_loop_repeats(duration);
    char entry[WP_PATH_MAX * 4 + 64];
    size_t length = repeats > 0 && write ? concat_entry(path, duration, entry, sizeof entry) : 0;
    if (length == 0 || !write(context, WP_MPV_CONCAT_HEADER, strlen(WP_MPV_CONCAT_HEADER))) return 0;
    for (int i = 0; i < repeats; i++) {
        if (!write(context, entry, length)) return 0;
    }
    return repeats;
}

static uint32_t be32(const unsigned char *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t be64(const unsigned char *b) {
    return (uint64_t)be32(b) << 32 | be32(b + 4);
}

static bool find_box(wp_read_fn read, void *context, uint64_t from, uint64_t to, const char *type,
                     uint64_t *body, uint64_t *end) {
    uint64_t at = from;
    while (at + 8 <= to) {
        unsigned char head[16];
        if (read(context, at, head, 8) != 8) return false;
        uint64_t length = be32(head), header = 8;
        if (length == 1) {
            if (read(context, at + 8, head + 8, 8) != 8) return false;
            length = be64(head + 8);
            header = 16;
        } else if (length == 0) {
            length = to - at;
        }
        if (length < header || length > to - at) return false;
        if (memcmp(head + 4, type, 4) == 0) {
            *body = at + header;
            *end = at + length;
            return true;
        }
        at += length;
    }
    return false;
}

static bool find_path(wp_read_fn read, void *context, uint64_t from, uint64_t to, const char *const *types,
                      int count, uint64_t *body, uint64_t *end) {
    for (int i = 0; i < count; i++) {
        if (!find_box(read, context, from, to, types[i], body, end)) return false;
        from = *body;
        to = *end;
    }
    return true;
}

static double track_duration(wp_read_fn read, void *context, uint64_t trak, uint64_t trak_end) {
    unsigned char b[32];
    uint64_t body, end;
    static const char *const hdlr[] = {"mdia", "hdlr"};
    if (!find_path(read, context, trak, trak_end, hdlr, 2, &body, &end) || end - body < 12 ||
        read(context, body + 8, b, 4) != 4 || memcmp(b, "vide", 4) != 0) return -1;
    static const char *const mdhd[] = {"mdia", "mdhd"};
    if (!find_path(read, context, trak, trak_end, mdhd, 2, &body, &end) || read(context, body, b, 24) != 24) return -1;
    double scale = b[0] == 1 ? be32(b + 20) : be32(b + 12);
    static const char *const stts[] = {"mdia", "minf", "stbl", "stts"};
    if (scale <= 0 || !find_path(read, context, trak, trak_end, stts, 4, &body, &end) ||
        read(context, body, b, 8) != 8) return -1;
    uint64_t entries = be32(b + 4);
    if (body + 8 + entries * 8 > end) return -1;
    double total = 0;
    uint32_t last = 0;
    unsigned char chunk[4096];
    for (uint64_t done = 0; done < entries;) {
        uint64_t batch = entries - done < sizeof chunk / 8 ? entries - done : sizeof chunk / 8;
        if (read(context, body + 8 + done * 8, chunk, (size_t)batch * 8) != batch * 8) return -1;
        for (uint64_t i = 0; i < batch; i++) {
            uint32_t samples = be32(chunk + i * 8), delta = be32(chunk + i * 8 + 4);
            if (delta == 0) delta = last;
            else last = delta;
            total += (double)samples * delta;
        }
        done += batch;
    }
    return total > 0 ? total / scale : -1;
}

double wp_mp4_duration(wp_read_fn read, void *context, uint64_t length) {
    uint64_t moov, moov_end, mvhd, mvhd_end;
    if (!read || !find_box(read, context, 0, length, "moov", &moov, &moov_end)) return -1;
    uint64_t trak, trak_end;
    for (uint64_t at = moov; find_box(read, context, at, moov_end, "trak", &trak, &trak_end); at = trak_end) {
        double seconds = track_duration(read, context, trak, trak_end);
        if (seconds > 0) return seconds;
    }
    if (!find_box(read, context, moov, moov_end, "mvhd", &mvhd, &mvhd_end)) return -1;
    unsigned char b[32];
    if (read(context, mvhd, b, 1) != 1) return -1;
    double scale, units;
    if (b[0] == 1) {
        if (read(context, mvhd, b, 32) != 32) return -1;
        scale = be32(b + 20);
        units = (double)be64(b + 24);
    } else {
        if (read(context, mvhd, b, 20) != 20) return -1;
        scale = be32(b + 12);
        units = be32(b + 16);
    }
    return scale > 0 ? units / scale : -1;
}

int wp_mpv_version(const char *text) {
    if (!text) return 0;
    const char *c = strstr(text, "mpv");
    c = c ? c + 3 : text;
    while (*c && !isdigit((unsigned char)*c)) c++;
    char *end;
    long major = strtol(c, &end, 10);
    if (end == c || *end != '.') return 0;
    long minor = strtol(end + 1, &end, 10);
    if (major != 0) return (int)(major * 1000 + minor);
    return (int)minor;
}

static const char *find_value(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t length = strlen(key);
    for (const char *c = strchr(json, '"'); c; c = strchr(c + 1, '"')) {
        if (strncmp(c + 1, key, length) == 0 && c[1 + length] == '"') {
            const char *v = c + 2 + length;
            while (isspace((unsigned char)*v)) v++;
            if (*v != ':') continue;
            v++;
            while (isspace((unsigned char)*v)) v++;
            return v;
        }
    }
    return NULL;
}

bool wp_json_number(const char *json, const char *key, double *out) {
    const char *v = find_value(json, key);
    if (!v || !(isdigit((unsigned char)*v) || *v == '-' || *v == '.')) return false;
    char *end;
    double value = strtod(v, &end);
    if (end == v) return false;
    *out = value;
    return true;
}

bool wp_json_string(const char *json, const char *key, char *out, size_t size) {
    const char *v = find_value(json, key);
    if (!v || *v != '"' || size == 0) return false;
    size_t n = 0;
    for (v++; *v && *v != '"'; v++) {
        char c = *v;
        if (c == '\\' && v[1]) {
            v++;
            c = *v == 'n' ? '\n' : *v == 't' ? '\t' : *v;
        }
        if (n + 1 < size) out[n++] = c;
    }
    out[n] = '\0';
    return *v == '"';
}
