#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-gamma-control-unstable-v1-client-protocol.h"

struct output_info {
    struct wl_output *output;
    uint32_t global_id;
    char *output_name;
    struct output_info *next;
};

// .cal からパースした生データ
typedef struct {
    int count;
    float *in;
    float *r;
    float *g;
    float *b;
} cal_data_t;

struct context {
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_gamma_control_manager_v1 *gamma_manager;
    struct output_info *outputs;

    // キャリブレーションデータパス
    const char *target_output;
    const char *cal_file_path;
    cal_data_t cal;

    // ガンマ制御状態
    uint32_t gamma_size;
    int failed;
};

// --- .cal パースおよび補間処理 ---

static float interpolate(float x, float x0, float x1, float y0, float y1) {
    if (x1 == x0) return y0;
    return y0 + (x - x0) * (y1 - y0) / (x1 - x0);
}

static float clampf(float val, float min, float max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

static bool load_cal_file(const char *filename, cal_data_t *cal) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "エラー: .cal ファイル '%s' を開けませんでした\n", filename);
        return false;
    }

    char line[512];
    int num_sets = 0;

    // 1. NUMBER_OF_SETS の取得
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "NUMBER_OF_SETS %d", &num_sets) == 1) {
            break;
        }
    }

    if (num_sets <= 0) {
        fprintf(stderr, "エラー: .cal ファイルに NUMBER_OF_SETS が見つかりません\n");
        fclose(f);
        return false;
    }

    cal->in = malloc(num_sets * sizeof(float));
    cal->r  = malloc(num_sets * sizeof(float));
    cal->g  = malloc(num_sets * sizeof(float));
    cal->b  = malloc(num_sets * sizeof(float));

    // 2. BEGIN_DATA の位置まで進める
    bool in_data = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "BEGIN_DATA") != NULL) {
            in_data = true;
            break;
        }
    }

    if (!in_data) {
        fprintf(stderr, "エラー: BEGIN_DATA セクションが見つかりません\n");
        fclose(f);
        return false;
    }

    // 3. データの読み込み
    int count = 0;
    while (count < num_sets && fgets(line, sizeof(line), f)) {
        if (strstr(line, "END_DATA") != NULL) {
            break;
        }

        float in, r, g, b;
        if (sscanf(line, "%f %f %f %f", &in, &r, &g, &b) == 4) {
            cal->in[count] = in;
            cal->r[count]  = r;
            cal->g[count]  = g;
            cal->b[count]  = b;
            count++;
        }
    }
    fclose(f);

    cal->count = count;
    printf("[+] .cal ファイルを読み込みました (読込成功: %d / 設定期待値: %d エントリ)\n", count, num_sets);

    if (count == 0) {
        fprintf(stderr, "エラー: 有効な LUT データが 1 件も読み込めませんでした\n");
        return false;
    }

    printf("  先頭 [0]: in=%.6f -> R=%.6f, G=%.6f, B=%.6f\n", cal->in[0], cal->r[0], cal->g[0], cal->b[0]);
    printf("  末尾 [%d]: in=%.6f -> R=%.6f, G=%.6f, B=%.6f\n", count - 1, cal->in[count - 1], cal->r[count - 1], cal->g[count - 1], cal->b[count - 1]);

    return true;
}

// 共有メモリ (shm) 用の匿名ファイル作成
static int create_anonymous_file(size_t size) {
    int fd = memfd_create("gamma-lut", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void apply_calibration(struct context *ctx, struct zwlr_gamma_control_v1 *control) {
    uint32_t size = ctx->gamma_size;
    size_t lut_bytes = size * sizeof(uint16_t) * 3;

    int fd = create_anonymous_file(lut_bytes);
    if (fd < 0) {
        perror("memfd_create failed");
        return;
    }

    uint16_t *lut = mmap(NULL, lut_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (lut == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return;
    }

    uint16_t *r_table = lut;
    uint16_t *g_table = lut + size;
    uint16_t *b_table = lut + (size * 2);

    cal_data_t *cal = &ctx->cal;

    for (uint32_t i = 0; i < size; i++) {
        float x = (float)i / (float)(size - 1);
        float r_val = 0.0f, g_val = 0.0f, b_val = 0.0f;

        if (x <= cal->in[0]) {
            r_val = cal->r[0];
            g_val = cal->g[0];
            b_val = cal->b[0];
        } else if (x >= cal->in[cal->count - 1]) {
            r_val = cal->r[cal->count - 1];
            g_val = cal->g[cal->count - 1];
            b_val = cal->b[cal->count - 1];
        } else {
            for (int j = 0; j < cal->count - 1; j++) {
                if (x >= cal->in[j] && x <= cal->in[j + 1]) {
                    r_val = interpolate(x, cal->in[j], cal->in[j + 1], cal->r[j], cal->r[j + 1]);
                    g_val = interpolate(x, cal->in[j], cal->in[j + 1], cal->g[j], cal->g[j + 1]);
                    b_val = interpolate(x, cal->in[j], cal->in[j + 1], cal->b[j], cal->b[j + 1]);
                    break;
                }
            }
        }

        r_val = clampf(r_val, 0.0f, 1.0f);
        g_val = clampf(g_val, 0.0f, 1.0f);
        b_val = clampf(b_val, 0.0f, 1.0f);

        r_table[i] = (uint16_t)(r_val * 65535.0f + 0.5f);
        g_table[i] = (uint16_t)(g_val * 65535.0f + 0.5f);
        b_table[i] = (uint16_t)(b_val * 65535.0f + 0.5f);
    }

    printf("  [LUT 生成結果] index [0]: R=%u, G=%u, B=%u\n", r_table[0], g_table[0], b_table[0]);
    printf("  [LUT 生成結果] index [%u]: R=%u, G=%u, B=%u\n", size - 1, r_table[size - 1], g_table[size - 1], b_table[size - 1]);

    munmap(lut, lut_bytes);

    zwlr_gamma_control_v1_set_gamma(control, fd);
    close(fd);
}

static void free_cal_data(cal_data_t *cal) {
    free(cal->in);
    free(cal->r);
    free(cal->g);
    free(cal->b);
}

// --- wl_output イベントハンドラ (名前取得用) ---

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height, int32_t refresh) {}
static void output_handle_done(void *data, struct wl_output *wl_output) {}
static void output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor) {}

static void output_handle_name(void *data, struct wl_output *wl_output, const char *name) {
    struct output_info *info = data;
    if (info->output_name) free(info->output_name);
    info->output_name = strdup(name);
}

static void output_handle_description(void *data, struct wl_output *wl_output, const char *description) {}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

// --- zwlr_gamma_control_v1 イベントハンドラ ---

static void gamma_control_handle_gamma_size(void *data, struct zwlr_gamma_control_v1 *control, uint32_t size) {
    struct context *ctx = data;
    ctx->gamma_size = size;
}

static void gamma_control_handle_failed(void *data, struct zwlr_gamma_control_v1 *control) {
    struct context *ctx = data;
    fprintf(stderr, "Error: ガンマ制御の適用に失敗しました（他のアプリが制御中などの可能性があります）\n");
    ctx->failed = 1;
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
    .gamma_size = gamma_control_handle_gamma_size,
    .failed = gamma_control_handle_failed,
};

// --- Registry イベントハンドラ ---

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface, uint32_t version) {
    struct context *ctx = data;

    if (strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
        ctx->gamma_manager = wl_registry_bind(registry, name, &zwlr_gamma_control_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct output_info *info = calloc(1, sizeof(struct output_info));
        info->global_id = name;
        // wl_output バージョン 4 で bind して name イベントを受信可能にする
        uint32_t bind_version = (version >= 4) ? 4 : version;
        info->output = wl_registry_bind(registry, name, &wl_output_interface, bind_version);
        info->next = ctx->outputs;
        ctx->outputs = info;

        wl_output_add_listener(info->output, &output_listener, info);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void cleanup_outputs(struct context *ctx) {
    struct output_info *curr = ctx->outputs;
    while (curr) {
        struct output_info *next = curr->next;
        if (curr->output_name) free(curr->output_name);
        if (curr->output) wl_output_destroy(curr->output);
        free(curr);
        curr = next;
    }
    ctx->outputs = NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <path_to_cal_file> [output_name]\n", argv[0]);
        return 1;
    }

    struct context ctx = {
        .cal_file_path = argv[1],
        .target_output = (argc >= 3) ? argv[2] : "eDP-1",
    };

    if (!load_cal_file(ctx.cal_file_path, &ctx.cal)) {
        return 1;
    }

    printf("対象出力名: %s\n", ctx.target_output);

    ctx.display = wl_display_connect(NULL);
    if (!ctx.display) {
        fprintf(stderr, "Waylandディスプレイに接続できませんでした。\n");
        free_cal_data(&ctx.cal);
        return 1;
    }

    ctx.registry = wl_display_get_registry(ctx.display);
    wl_registry_add_listener(ctx.registry, &registry_listener, &ctx);
    
    // 1回目のラウンドトリップ: グローバルオブジェクト(wl_output, gamma_manager)の列挙
    wl_display_roundtrip(ctx.display);

    // 2回目のラウンドトリップ: 各 wl_output の name イベント受信
    wl_display_roundtrip(ctx.display);

    if (!ctx.gamma_manager) {
        fprintf(stderr, "エラー: コンポジタが wlr-gamma-control-v1 をサポートしていません。\n");
        cleanup_outputs(&ctx);
        free_cal_data(&ctx.cal);
        return 1;
    }

    // ターゲット出力の検索
    struct output_info *target_info = NULL;
    for (struct output_info *info = ctx.outputs; info != NULL; info = info->next) {
        if (info->output_name) {
            printf("[検出出力] ID: %u, Name: %s\n", info->global_id, info->output_name);
            if (strcmp(info->output_name, ctx.target_output) == 0) {
                target_info = info;
            }
        }
    }

    if (!target_info) {
        fprintf(stderr, "エラー: 指定された出力 '%s' が見つかりませんでした。\n", ctx.target_output);
        cleanup_outputs(&ctx);
        free_cal_data(&ctx.cal);
        return 1;
    }

    printf("[+] ターゲット出力 '%s' を確認しました。\n", target_info->output_name);

    struct zwlr_gamma_control_v1 *control =
        zwlr_gamma_control_manager_v1_get_gamma_control(ctx.gamma_manager, target_info->output);

    zwlr_gamma_control_v1_add_listener(control, &gamma_control_listener, &ctx);
    wl_display_roundtrip(ctx.display); // gamma_size の取得

    if (ctx.gamma_size == 0 || ctx.failed) {
        fprintf(stderr, "ガンマ制御の初期化に失敗しました。\n");
        zwlr_gamma_control_v1_destroy(control);
        cleanup_outputs(&ctx);
        free_cal_data(&ctx.cal);
        return 1;
    }

    printf("ディスプレイ LUTサイズ: %u エントリ\n", ctx.gamma_size);

    // .cal データから補間・適用
    apply_calibration(&ctx, control);
    wl_display_flush(ctx.display);

    printf("キャリブレーション値を Wayland 経由でハードウェア LUT に適用しました。(Ctrl+Cで終了すると復元します)\n");

    // 常駐ループ
    while (wl_display_dispatch(ctx.display) != -1 && !ctx.failed) {
    }

    zwlr_gamma_control_v1_destroy(control);
    cleanup_outputs(&ctx);
    wl_display_disconnect(ctx.display);
    free_cal_data(&ctx.cal);
    return 0;
}
