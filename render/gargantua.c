#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define MAX_W 7680
#define MAX_H 4800
#define MAX_THREADS 64
#define SAMPLES 4
#define TAPS 12
#define FPS 120
#define TAU 6.28318530718f

typedef struct { float x, y, z; } v3;
typedef void (*rows_fn)(int first, int last);

static float lens[MAX_W * MAX_H][4];
static float glow[MAX_W * MAX_H][3];
static float quarter[(MAX_W / 4) * (MAX_H / 4)][3];
static float bloom_a[(MAX_W / 4) * (MAX_H / 4)][3];
static float bloom_b[(MAX_W / 4) * (MAX_H / 4)][3];
static unsigned char frame[MAX_W * MAX_H * 6];
static int width, height, small_w, small_h, threads = 1;
static float phase, step;
static rows_fn current;
static int current_rows;
static atomic_int next_row;

static v3 v(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static v3 add(v3 a, v3 b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 scale(v3 a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float length3(v3 a) { return sqrtf(dot(a, a)); }
static v3 normal(v3 a) { return scale(a, 1.0f / length3(a)); }
static float fract(float x) { return x - floorf(x); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float wrap(float a) { return atan2f(sinf(a), cosf(a)); }
static float fifth(float r) { float r2 = r * r; return r2 * r2 * r; }

static float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float hash(v3 p) {
    p = v(fract(p.x * 0.1031f), fract(p.y * 0.1031f), fract(p.z * 0.1031f));
    float d = dot(p, v(p.y + 33.33f, p.z + 33.33f, p.x + 33.33f));
    p = v(p.x + d, p.y + d, p.z + d);
    return fract((p.x + p.y) * p.z);
}

static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

static float noise(v3 p) {
    v3 i = v(floorf(p.x), floorf(p.y), floorf(p.z));
    float fx = fade(p.x - i.x), fy = fade(p.y - i.y), fz = fade(p.z - i.z);
    float a = hash(i), b = hash(add(i, v(1, 0, 0))), c = hash(add(i, v(0, 1, 0))), d = hash(add(i, v(1, 1, 0)));
    float e = hash(add(i, v(0, 0, 1))), f = hash(add(i, v(1, 0, 1))), g = hash(add(i, v(0, 1, 1)));
    float h = hash(add(i, v(1, 1, 1)));
    return mixf(mixf(mixf(a, b, fx), mixf(c, d, fx), fy), mixf(mixf(e, f, fx), mixf(g, h, fx), fy), fz);
}

static float fbm(v3 p, float footprint) {
    float value = 0.0f, amplitude = 0.51f;
    for (int i = 0; i < 7; i++) {
        float visibility = 1.0f - smooth(0.20f, 0.85f, footprint);
        value += amplitude * (visibility > 0.0f ? mixf(0.5f, noise(p), visibility) : 0.5f);
        p = add(scale(p, 2.07f), v(13.1f, 7.3f, 5.2f));
        amplitude *= 0.54f;
        footprint *= 2.07f;
    }
    return value;
}

static void trace(float u, float w, float aspect, float out[4]) {
    v3 eye = v(0.0f, 2.65f, 19.0f), forward = normal(scale(eye, -1.0f)), right = v(1, 0, 0);
    v3 up = cross(right, forward);
    v3 screen = add(scale(right, (u - 0.5f) * aspect), scale(up, -(w - 0.5f)));
    v3 p = eye, velocity = normal(add(forward, scale(screen, 0.98f)));
    v3 moment = cross(p, velocity);
    float h2 = dot(moment, moment);
    for (int i = 0; i < 4000; i++) {
        float r = length3(p);
        if (r < 1.005f) break;
        if (r > 35.0f) {
            v3 d = normal(velocity);
            out[0] = -2.0f, out[1] = d.x, out[2] = d.y, out[3] = d.z;
            return;
        }
        float dt = clampf(r * 0.010f, 0.004f, 0.40f);
        v3 half = add(velocity, scale(p, -0.5f * dt * 1.5f * h2 / fifth(r)));
        v3 next = add(p, scale(half, dt));
        v3 after = sub(half, scale(next, 0.5f * dt * 1.5f * h2 / fifth(length3(next))));
        if (p.y * next.y < 0.0f) {
            v3 c = add(p, scale(sub(next, p), p.y / (p.y - next.y)));
            float cr = sqrtf(c.x * c.x + c.z * c.z);
            if (cr >= 3.0f && cr < 13.4f) {
                v3 orbit = normal(v(-c.z, 0.0f, c.x));
                float speed = sqrtf(0.5f / (cr - 1.0f));
                float doppler = sqrtf(1.0f - 1.0f / cr) * sqrtf(1.0f - speed * speed) /
                                (1.0f + speed * dot(normal(velocity), orbit));
                out[0] = cr, out[1] = atan2f(c.z, c.x), out[2] = doppler, out[3] = 1.0f;
                return;
            }
        }
        p = next;
        velocity = after;
    }
    out[0] = -1.0f, out[1] = 0.0f, out[2] = 0.0f, out[3] = 1.0f;
}

static void geometry_rows(int first, int last) {
    float aspect = (float)width / (float)height;
    for (int y = first; y < last; y++) {
        for (int x = 0; x < width; x++) {
            float sum_r = 0, sum_s = 0, sum_c = 0, sum_d = 0, hits = 0, captured = 0, escaped = 0;
            v3 away = v(0, 0, 0);
            for (int sy = 0; sy < SAMPLES; sy++) {
                for (int sx = 0; sx < SAMPLES; sx++) {
                    float t[4];
                    trace(((float)x + ((float)sx + 0.5f) / SAMPLES) / (float)width,
                          ((float)y + ((float)sy + 0.5f) / SAMPLES) / (float)height, aspect, t);
                    if (t[0] > 0.0f) {
                        sum_r += t[0], sum_s += sinf(t[1]), sum_c += cosf(t[1]), sum_d += t[2], hits += 1.0f;
                    } else if (t[0] < -1.5f) {
                        away = add(away, v(t[1], t[2], t[3]));
                        escaped += 1.0f;
                    } else {
                        captured += 1.0f;
                    }
                }
            }
            float *m = lens[y * width + x];
            if (hits > 0.0f) {
                m[0] = sum_r / hits, m[1] = atan2f(sum_s, sum_c), m[2] = sum_d / hits, m[3] = hits / (SAMPLES * SAMPLES);
            } else if (escaped >= captured && length3(away) > 0.0f) {
                v3 d = normal(away);
                m[0] = -2.0f, m[1] = d.x, m[2] = d.y, m[3] = d.z;
            } else {
                m[0] = -1.0f, m[1] = 0.0f, m[2] = 0.0f, m[3] = 1.0f;
            }
        }
    }
}

static float gas(float r, float angle, float turn, float footprint) {
    float theta = angle - turn;
    v3 p = v(cosf(theta) * r * 0.85f, sinf(theta) * r * 0.85f, r * 1.5f);
    float n = fbm(p, footprint * 1.5f);
    float curl_x = fbm(add(scale(p, 0.74f), v(3.7f, 8.2f, 1.5f)), footprint) - 0.5f;
    float curl_y = fbm(add(scale(p, 0.74f), v(8.3f, 2.1f, 6.4f)), footprint) - 0.5f;
    float warped = r + 0.28f * (n - 0.5f) + 0.16f * curl_x;
    float bent = theta + 0.13f * n + 0.07f * curl_y;
    float swirl = fbm(v(cosf(bent) * r * 3.0f, sinf(bent) * r * 3.0f, warped * 11.0f), footprint * 12.0f);
    float fine = fbm(v(cosf(theta + 0.04f * curl_x) * r * 8.0f, sinf(theta + 0.04f * curl_x) * r * 8.0f, warped * 26.0f),
                     footprint * 30.0f);
    float micro = fbm(v(cosf(bent) * r * 18.0f, sinf(bent) * r * 18.0f, warped * 47.0f), footprint * 50.0f);
    return 0.021f + n + 0.34f * swirl + 0.26f * fine + 0.10f * micro;
}

static void shade(float r, float angle, float turn, float footprint, float doppler, float color[3]) {
    float inner = smooth(3.0f, 3.35f, r), outer = 1.0f - smooth(9.0f, 13.4f, r);
    if (inner * outer <= 0.0f) return;
    float t = 1.0f - smooth(3.0f, 7.5f, r), n = 0.0f;
    if (t < 1.0f) n += (1.0f - t) * gas(r, angle, turn, footprint);
    if (t > 0.0f) n += t * gas(r, angle, turn * 2.0f, footprint);
    float b = powf(3.0f / r, 2.5f) * inner * outer * n * n * n * 4.2f * doppler * doppler;
    float heat = clampf((6.0f - r) / 4.0f + 0.45f * n, 0.0f, 1.0f);
    color[0] += b;
    color[1] += mixf(0.06f, 0.58f, heat) * b;
    color[2] += mixf(0.003f, 0.20f, heat) * b;
}

static void slope(int x, int y, int dx, int dy, const float *m, float out[2]) {
    float shortest = -1.0f;
    out[0] = out[1] = 0.0f;
    for (int s = -1; s <= 1; s += 2) {
        int qx = x + dx * s, qy = y + dy * s;
        if (qx < 0 || qy < 0 || qx >= width || qy >= height) continue;
        const float *n = lens[qy * width + qx];
        if (n[0] <= 0.0f) continue;
        float dr = (n[0] - m[0]) * (float)s, da = wrap(n[1] - m[1]) * (float)s, l = hypotf(dr, m[0] * da);
        if (shortest < 0.0f || l < shortest) {
            float k = l > 0.3f ? 0.3f / l : 1.0f;
            out[0] = dr * k, out[1] = da * k, shortest = l;
        }
    }
}

static void disk(int x, int y, const float *m, float color[3]) {
    float r = m[0], sa[2], sb[2];
    slope(x, y, 1, 0, m, sa);
    slope(x, y, 0, 1, m, sb);
    float la = hypotf(sa[0], r * sa[1]), lb = hypotf(sb[0], r * sb[1]);
    float lm = r * step * (2.0f - smooth(3.0f, 7.5f, r));
    int na = 1, nb = 1, nm = 1;
    for (int i = 0; i < TAPS; i++) {
        int ga = na, gb = nb, gm = nm;
        float ea = la / (float)na, eb = lb / (float)nb, em = lm / (float)nm;
        if (ea >= eb && ea >= em) ga++;
        else if (eb >= em) gb++;
        else gm++;
        if (ga * gb * gm > TAPS) break;
        na = ga, nb = gb, nm = gm;
    }
    float footprint = clampf(maxf(maxf(la / (float)na, lb / (float)nb), lm / (float)nm), 0.0001f, 0.3f);
    int count = na * nb * nm;
    for (int k = 0; k < count; k++) {
        float oa = ((float)(k % na) + 0.5f) / (float)na - 0.5f;
        float ob = ((float)((k / na) % nb) + 0.5f) / (float)nb - 0.5f;
        float om = ((float)(k / (na * nb)) + 0.5f) / (float)nm - 0.5f;
        shade(r + oa * sa[0] + ob * sb[0], m[1] + oa * sa[1] + ob * sb[1], phase + om * step, footprint, m[2], color);
    }
    for (int c = 0; c < 3; c++) color[c] *= m[3] / (float)count;
}

static void star(const float *m, const float *nx, const float *ny, float color[3]) {
    v3 d = normal(v(m[1], m[2], m[3]));
    float ax = fabsf(d.x), ay = fabsf(d.y), az = fabsf(d.z), fu, fv, side;
    if (ax >= ay && ax >= az) fu = d.y / ax, fv = d.z / ax, side = d.x > 0.0f ? 0.0f : 1.0f;
    else if (ay >= az) fu = d.x / ay, fv = d.z / ay, side = d.y > 0.0f ? 2.0f : 3.0f;
    else fu = d.x / az, fv = d.y / az, side = d.z > 0.0f ? 4.0f : 5.0f;
    const float cells = 400.0f;
    float id_u = floorf((fu * 0.5f + 0.5f) * cells), id_v = floorf((fv * 0.5f + 0.5f) * cells), seed = side * 37.0f;
    if (hash(v(id_u, id_v, seed + 17.0f)) < 0.995f) return;
    float spot_u = (id_u + 0.15f + 0.7f * hash(v(id_u, id_v, seed + 3.0f))) / cells * 2.0f - 1.0f;
    float spot_v = (id_v + 0.15f + 0.7f * hash(v(id_u, id_v, seed + 7.0f))) / cells * 2.0f - 1.0f;
    v3 aim = side < 1.5f ? v(side < 0.5f ? 1.0f : -1.0f, spot_u, spot_v)
           : side < 3.5f ? v(spot_u, side < 2.5f ? 1.0f : -1.0f, spot_v)
           : v(spot_u, spot_v, side < 4.5f ? 1.0f : -1.0f);
    float spread = 0.0f;
    if (nx[0] < -1.5f) spread = maxf(spread, length3(sub(d, normal(v(nx[1], nx[2], nx[3])))));
    if (ny[0] < -1.5f) spread = maxf(spread, length3(sub(d, normal(v(ny[1], ny[2], ny[3])))));
    const float size = 0.00037f;
    float angle = length3(sub(d, normal(aim))), footprint = maxf(spread, size);
    float light = expf(-angle * angle / (footprint * footprint)) * (size * size) / (footprint * footprint);
    float shine = hash(v(id_u, id_v, seed + 29.0f)), tint = hash(v(id_u, id_v, seed + 23.0f));
    float k = light * (0.10f + 0.60f * shine * shine * shine);
    color[0] = mixf(1.0f, 0.78f, tint) * k;
    color[1] = mixf(0.80f, 0.86f, tint) * k;
    color[2] = mixf(0.62f, 1.0f, tint) * k;
}

static void cloud_rows(int first, int last) {
    for (int y = first; y < last; y++) {
        int below = y + 1 < height ? y + 1 : height - 1;
        for (int x = 0; x < width; x++) {
            int beside = x + 1 < width ? x + 1 : width - 1;
            const float *m = lens[y * width + x], *nx = lens[y * width + beside], *ny = lens[below * width + x];
            float *color = glow[y * width + x];
            color[0] = color[1] = color[2] = 0.0f;
            if (m[0] > 0.0f) disk(x, y, m, color);
            else if (m[0] < -1.5f) star(m, nx, ny, color);
        }
    }
}

static void downsample_rows(int first, int last) {
    for (int y = first; y < last; y++) {
        for (int x = 0; x < small_w; x++) {
            float *out = quarter[y * small_w + x];
            out[0] = out[1] = out[2] = 0.0f;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    const float *in = glow[(y * 4 + dy) * width + x * 4 + dx];
                    out[0] += in[0] / 16.0f, out[1] += in[1] / 16.0f, out[2] += in[2] / 16.0f;
                }
            }
        }
    }
}

static void sample(float (*source)[3], float u, float w, float out[3]) {
    float px = u * (float)small_w - 0.5f, py = w * (float)small_h - 0.5f;
    float x0 = floorf(px), y0 = floorf(py), fx = px - x0, fy = py - y0;
    int xa = (int)clampf(x0, 0.0f, (float)(small_w - 1)), xb = (int)clampf(x0 + 1.0f, 0.0f, (float)(small_w - 1));
    int ya = (int)clampf(y0, 0.0f, (float)(small_h - 1)), yb = (int)clampf(y0 + 1.0f, 0.0f, (float)(small_h - 1));
    for (int c = 0; c < 3; c++) {
        out[c] = mixf(mixf(source[ya * small_w + xa][c], source[ya * small_w + xb][c], fx),
                      mixf(source[yb * small_w + xa][c], source[yb * small_w + xb][c], fx), fy);
    }
}

static void blur(float (*source)[3], float (*target)[3], float dx, float dy, int first, int last) {
    for (int y = first; y < last; y++) {
        for (int x = 0; x < small_w; x++) {
            float u = ((float)x + 0.5f) / (float)small_w, w = ((float)y + 0.5f) / (float)small_h;
            float sum[3] = {0, 0, 0}, weights = 0.0f;
            for (int i = -20; i <= 20; i++) {
                float weight = expf(-(float)(i * i) / 120.0f), tap[3];
                sample(source, u + dx * (float)i / 960.0f, w + dy * (float)i / 600.0f, tap);
                sum[0] += tap[0] * weight, sum[1] += tap[1] * weight, sum[2] += tap[2] * weight;
                weights += weight;
            }
            float *out = target[y * small_w + x];
            out[0] = sum[0] / weights, out[1] = sum[1] / weights, out[2] = sum[2] / weights;
        }
    }
}

static void blur_across(int first, int last) { blur(quarter, bloom_a, 1.0f, 0.0f, first, last); }
static void blur_down(int first, int last) { blur(bloom_a, bloom_b, 0.0f, 1.0f, first, last); }

static void composite_rows(int first, int last) {
    for (int y = first; y < last; y++) {
        for (int x = 0; x < width; x++) {
            float bloom[3], c[3];
            sample(bloom_b, ((float)x + 0.5f) / (float)width, ((float)y + 0.5f) / (float)height, bloom);
            for (int k = 0; k < 3; k++) {
                c[k] = 1.0f - expf(-(glow[y * width + x][k] + bloom[k] * 0.22f) * 1.25f);
                c[k] = powf(maxf(c[k], 0.0f), 1.0f / 2.2f);
            }
            float dither = (hash(v((float)x, (float)y, 71.0f)) - 0.5f) * (0.65f / 1023.0f);
            dither *= smooth(0.002f, 0.015f, maxf(c[0], maxf(c[1], c[2])));
            unsigned char *out = frame + ((size_t)y * (size_t)width + (size_t)x) * 6;
            for (int k = 0; k < 3; k++) {
                unsigned level = (unsigned)(clampf(c[k] + dither, 0.0f, 1.0f) * 1023.0f + 0.5f);
                unsigned wide = (level << 6) | (level >> 4);
                out[k * 2] = (unsigned char)(wide & 0xff);
                out[k * 2 + 1] = (unsigned char)(wide >> 8);
            }
        }
    }
}

static void work(void) {
    for (int y = atomic_fetch_add(&next_row, 4); y < current_rows; y = atomic_fetch_add(&next_row, 4)) {
        current(y, y + 4 < current_rows ? y + 4 : current_rows);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID unused) { (void)unused; work(); return 0; }
static int cores(void) { SYSTEM_INFO info; GetSystemInfo(&info); return (int)info.dwNumberOfProcessors; }
#else
static void *worker(void *unused) { (void)unused; work(); return NULL; }
static int cores(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

static void parallel(rows_fn fn, int rows) {
    current = fn;
    current_rows = rows;
    atomic_store(&next_row, 0);
#ifdef _WIN32
    HANDLE ids[MAX_THREADS];
    int started = 0;
    for (int i = 1; i < threads; i++) {
        ids[started] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (ids[started]) started++;
    }
    work();
    for (int i = 0; i < started; i++) {
        WaitForSingleObject(ids[i], INFINITE);
        CloseHandle(ids[i]);
    }
#else
    pthread_t ids[MAX_THREADS];
    int started = 0;
    for (int i = 1; i < threads; i++) {
        if (pthread_create(&ids[started], NULL, worker, NULL) == 0) started++;
    }
    work();
    for (int i = 0; i < started; i++) pthread_join(ids[i], NULL);
#endif
}

static void render(float turn) {
    phase = turn;
    parallel(cloud_rows, height);
    parallel(downsample_rows, small_h);
    parallel(blur_across, small_h);
    parallel(blur_down, small_h);
    parallel(composite_rows, height);
}

static int number(const char *text, int lo, int hi) {
    char *end;
    long value = strtol(text, &end, 10);
    return *text && !*end && value >= lo && value <= hi ? (int)value : -1;
}

int main(int argc, char **argv) {
    int seconds = argc == 4 ? number(argv[3], 0, 600) : -1;
    width = argc == 4 ? number(argv[1], 64, MAX_W) : -1;
    height = argc == 4 ? number(argv[2], 64, MAX_H) : -1;
    if (width < 0 || height < 0 || seconds < 0) {
        fprintf(stderr, "usage: gargantua <width up to %d> <height up to %d> <seconds>\n"
                        "writes raw 16-bit little-endian RGB frames at %d fps to standard output;\n"
                        "0 seconds writes one 16-bit PPM picture\n",
                MAX_W, MAX_H, FPS);
        return 2;
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    small_w = width / 4;
    small_h = height / 4;
    threads = cores();
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    time_t start = time(NULL);
    fprintf(stderr, "Gargantua %dx%d on %d threads: bending light\n", width, height, threads);
    parallel(geometry_rows, height);
    size_t bytes = (size_t)width * (size_t)height * 6;
    int frames = (seconds > 0 ? seconds : 24) * FPS;
    step = TAU / (float)frames;
    if (seconds == 0) {
        render(0.0f);
        for (size_t i = 0; i < bytes; i += 2) {
            unsigned char low = frame[i];
            frame[i] = frame[i + 1];
            frame[i + 1] = low;
        }
        printf("P6\n%d %d\n65535\n", width, height);
        return fwrite(frame, 1, bytes, stdout) == bytes && fflush(stdout) == 0 ? 0 : 1;
    }
    for (int i = 0; i < frames; i++) {
        render((float)i * step);
        if (fwrite(frame, 1, bytes, stdout) != bytes) {
            fprintf(stderr, "the encoder stopped reading\n");
            return 1;
        }
        if (i % FPS == FPS - 1) {
            double spent = difftime(time(NULL), start);
            fprintf(stderr, "frame %d/%d, %.0f s so far, about %.0f s left\n", i + 1, frames, spent,
                    spent / (i + 1) * (frames - i - 1));
        }
    }
    return fflush(stdout) == 0 ? 0 : 1;
}
