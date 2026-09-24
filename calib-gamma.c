#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

// #include "wlr-gamma-control-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

struct output_info {
    struct wl_output *output;
    uint32_t name;
    char *output_name;
    struct output_info *next;
};

struct context {
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_gamma_control_manager_v1 *gamma_manager;
    struct output_info *outputs;

    // キャリブレーションパラメータ
    const char *target_output;
    double gamma_r, gamma_g, gamma_b; // 各色のガンマ値 (デフォルト 1.0)
    double gain_r, gain_g, gain_b;   // 各色のゲイン/輝度補正 (0.0 ~ 1.0)

    // ガンマ制御状態
    uint32_t gamma_size;
    int failed;
};

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

// --- zwlr_gamma_control_v1 イベントハンドラ ---

static void gamma_control_handle_gamma_size(void *data,
                                            struct zwlr_gamma_control_v1 *control,
                                            uint32_t size) {
    struct context *ctx = data;
    ctx->gamma_size = size;
}

static void gamma_control_handle_failed(void *data,
                                        struct zwlr_gamma_control_v1 *control) {
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
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
    struct context *ctx = data;

    if (strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
        ctx->gamma_manager = wl_registry_bind(registry, name,
            &zwlr_gamma_control_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct output_info *info = calloc(1, sizeof(struct output_info));
        info->name = name;
        info->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        info->next = ctx->outputs;
        ctx->outputs = info;
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

// --- ガンマテーブル（LUT）の作成・送信 ---

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

    // ハードウェアLUT用のランプテーブル作成
    for (uint32_t i = 0; i < size; i++) {
        double norm = (double)i / (size - 1); // 0.0 ~ 1.0 に正規化

        // R, G, B それぞれのガンマおよびゲイン調整計算
        double r_val = pow(norm, 1.0 / ctx->gamma_r) * ctx->gain_r;
        double g_val = pow(norm, 1.0 / ctx->gamma_g) * ctx->gain_g;
        double b_val = pow(norm, 1.0 / ctx->gamma_b) * ctx->gain_b;

        // クランプ処理 (0.0 ~ 1.0)
        r_val = r_val > 1.0 ? 1.0 : (r_val < 0.0 ? 0.0 : r_val);
        g_val = g_val > 1.0 ? 1.0 : (g_val < 0.0 ? 0.0 : g_val);
        b_val = b_val > 1.0 ? 1.0 : (b_val < 0.0 ? 0.0 : b_val);

        // 16-bit 整数に変換 (0 ~ 65535)
        r_table[i] = (uint16_t)(r_val * 65535.0);
        g_table[i] = (uint16_t)(g_val * 65535.0);
        b_table[i] = (uint16_t)(b_val * 65535.0);
    }

    munmap(lut, lut_bytes);

    // wlrootsのガンマ制御オブジェクトにfdを送信
    zwlr_gamma_control_v1_set_gamma(control, fd);
    close(fd);
}

int main(int argc, char *argv[]) {
    struct context ctx = {
        .target_output = "eDP-1", // デフォルトはノートPC液晶
        .gamma_r = 1.0, .gamma_g = 1.0, .gamma_b = 1.0,
        .gain_r = 1.0,  .gain_g = 1.0,  .gain_b = 1.0,
    };

    if (argc > 1) ctx.target_output = argv[1];
    if (argc > 4) {
        ctx.gamma_r = atof(argv[2]);
        ctx.gamma_g = atof(argv[3]);
        ctx.gamma_b = atof(argv[4]);
    }
    if (argc > 7) {
        ctx.gain_r = atof(argv[5]);
        ctx.gain_g = atof(argv[6]);
        ctx.gain_b = atof(argv[7]);
    }

    printf("対象出力: %s\n", ctx.target_output);
    printf("Gamma RGB : %.2f, %.2f, %.2f\n", ctx.gamma_r, ctx.gamma_g, ctx.gamma_b);
    printf("Gain  RGB : %.2f, %.2f, %.2f\n", ctx.gain_r, ctx.gain_g, ctx.gain_b);

    ctx.display = wl_display_connect(NULL);
    if (!ctx.display) {
        fprintf(stderr, "Waylandディスプレイに接続できませんでした。\n");
        return 1;
    }

    ctx.registry = wl_display_get_registry(ctx.display);
    wl_registry_add_listener(ctx.registry, &registry_listener, &ctx);
    wl_display_roundtrip(ctx.display);

    if (!ctx.gamma_manager) {
        fprintf(stderr, "エラー: コンポジタが wlr-gamma-control-v1 をサポートしていません。\n");
        return 1;
    }

    // 対象の output オブジェクトを探索 (簡易実装: 最初のoutputを適用、実用時はxdg-output等で名前照合)
    struct output_info *target_info = ctx.outputs; 

    struct zwlr_gamma_control_v1 *control =
        zwlr_gamma_control_manager_v1_get_gamma_control(ctx.gamma_manager, target_info->output);

    zwlr_gamma_control_v1_add_listener(control, &gamma_control_listener, &ctx);
    wl_display_roundtrip(ctx.display); // gamma_size の取得

    if (ctx.gamma_size == 0 || ctx.failed) {
        fprintf(stderr, "ガンマ制御の初期化に失敗しました。\n");
        return 1;
    }

    printf("Vega iGPU LUTサイズ: %u エントリ\n", ctx.gamma_size);

    // ハードウェアLUTへの書き込みと適用
    apply_calibration(&ctx, control);
    wl_display_flush(ctx.display);

    printf("キャリブレーション値をハードウェアLUTに適用しました。(Ctrl+Cで終了すると復元します)\n");

    // Waylandクライアントとして常駐（プロセスが切れるとコンポジタがLUTをリセットするため）
    while (wl_display_dispatch(ctx.display) != -1 && !ctx.failed) {
        // イベントループ
    }

    zwlr_gamma_control_v1_destroy(control);
    wl_display_disconnect(ctx.display);
    return 0;
}
