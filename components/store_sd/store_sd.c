#include "store_sd.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "store_sd";

#define MOUNT_POINT "/sdcard"
#define QUEUE_PATH  MOUNT_POINT "/uplinkq.dat" /* 8.3 — FatFS LFN may be off */
#define META_PATH   MOUNT_POINT "/uplinkq.met"
#define TMP_PATH    MOUNT_POINT "/uplinkq.tmp"
#define COMPACT_HEAD_BYTES (256u * 1024u)

static SemaphoreHandle_t s_spi_mu;
static SemaphoreHandle_t s_q_mu;
static sdmmc_card_t *s_card;
static bool s_mounted;
static uint64_t s_head;
static uint32_t s_count;
static char s_last_err[80];

static void set_err(const char *msg)
{
    snprintf(s_last_err, sizeof(s_last_err), "%s", msg ? msg : "");
}

esp_err_t store_sd_spi_lock_init(void)
{
    if (s_spi_mu == NULL) {
        s_spi_mu = xSemaphoreCreateMutex();
        if (s_spi_mu == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_q_mu == NULL) {
        s_q_mu = xSemaphoreCreateMutex();
        if (s_q_mu == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void store_sd_spi_cs_idle_high(void)
{
#if CONFIG_STORE_SD_ENABLE
    const int sd_cs = CONFIG_STORE_SD_CS_GPIO;
#else
    const int sd_cs = 18;
#endif
#ifndef CONFIG_CAN_OBD_SPI_CS_GPIO
    const int mcp_cs = 20;
#else
    const int mcp_cs = CONFIG_CAN_OBD_SPI_CS_GPIO;
#endif

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sd_cs) | (1ULL << mcp_cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) == ESP_OK) {
        gpio_set_level(sd_cs, 1);
        gpio_set_level(mcp_cs, 1);
        ESP_LOGI(TAG, "SPI CS idle-high: SD=GPIO%d MCP=GPIO%d", sd_cs, mcp_cs);
    }
}

esp_err_t store_sd_spi_lock(uint32_t timeout_ms)
{
    if (s_spi_mu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreTake(s_spi_mu, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

void store_sd_spi_unlock(void)
{
    if (s_spi_mu) {
        xSemaphoreGive(s_spi_mu);
    }
}

static esp_err_t meta_save_unlocked(void)
{
    FILE *f = fopen(META_PATH, "w");
    if (!f) {
        set_err("meta write open fail");
        return ESP_FAIL;
    }
    int n = fprintf(f, "v1\nhead=%llu\ncount=%u\n",
                    (unsigned long long)s_head, (unsigned)s_count);
    fclose(f);
    if (n <= 0) {
        set_err("meta write fail");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t meta_load_unlocked(void)
{
    s_head = 0;
    s_count = 0;
    FILE *f = fopen(META_PATH, "r");
    if (!f) {
        return ESP_OK; /* first boot */
    }
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long h = 0;
        unsigned c = 0;
        if (sscanf(line, "head=%llu", &h) == 1) {
            s_head = h;
        } else if (sscanf(line, "count=%u", &c) == 1) {
            s_count = c;
        }
    }
    fclose(f);
    return ESP_OK;
}

static uint64_t file_size_unlocked(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

static esp_err_t line_len_at_unlocked(uint64_t offset, size_t *out_len)
{
    FILE *f = fopen(QUEUE_PATH, "r");
    if (!f) {
        return ESP_FAIL;
    }
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        n++;
        if (c == '\n') {
            break;
        }
        if (n > 4096) {
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    fclose(f);
    if (n == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_len = n;
    return ESP_OK;
}

static esp_err_t drop_oldest_unlocked(void)
{
    size_t len = 0;
    esp_err_t err = line_len_at_unlocked(s_head, &len);
    if (err != ESP_OK) {
        /* Corrupt / empty — reset queue. */
        s_head = 0;
        s_count = 0;
        unlink(QUEUE_PATH);
        return meta_save_unlocked();
    }
    s_head += len;
    if (s_count > 0) {
        s_count--;
    }
    return meta_save_unlocked();
}

static esp_err_t compact_unlocked(void)
{
    if (s_head == 0) {
        return ESP_OK;
    }
    FILE *in = fopen(QUEUE_PATH, "r");
    if (!in) {
        return ESP_FAIL;
    }
    if (fseek(in, (long)s_head, SEEK_SET) != 0) {
        fclose(in);
        return ESP_FAIL;
    }
    FILE *out = fopen(TMP_PATH, "w");
    if (!out) {
        fclose(in);
        return ESP_FAIL;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            unlink(TMP_PATH);
            return ESP_FAIL;
        }
    }
    fclose(in);
    fclose(out);
    unlink(QUEUE_PATH);
    if (rename(TMP_PATH, QUEUE_PATH) != 0) {
        return ESP_FAIL;
    }
    s_head = 0;
    return meta_save_unlocked();
}

#if CONFIG_STORE_SD_ENABLE
static esp_err_t mount_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    /* Dedicated SPI2 for SD (MCP2515 uses soft-SPI on 21/22/23 — C6 has one GPSPI). */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = CONFIG_STORE_SD_SPI_HZ / 1000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_STORE_SD_MOSI_GPIO,
        .miso_io_num = CONFIG_STORE_SD_MISO_GPIO,
        .sclk_io_num = CONFIG_STORE_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        set_err("spi_bus_initialize fail");
        return err;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_STORE_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "mounting SD on SPI2 SCK=%d MOSI=%d MISO=%d CS=%d",
             CONFIG_STORE_SD_SCK_GPIO, CONFIG_STORE_SD_MOSI_GPIO,
             CONFIG_STORE_SD_MISO_GPIO, CONFIG_STORE_SD_CS_GPIO);

    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        set_err("sd mount fail");
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
#endif

esp_err_t store_sd_init(void)
{
    esp_err_t err = store_sd_spi_lock_init();
    if (err != ESP_OK) {
        return err;
    }

#if !CONFIG_STORE_SD_ENABLE
    set_err("disabled");
    s_mounted = false;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mounted) {
        return ESP_OK;
    }

    err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (store_sd_spi_lock(5000) != ESP_OK) {
            set_err("spi lock timeout");
            return ESP_ERR_TIMEOUT;
        }
        err = mount_card();
        store_sd_spi_unlock();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "mount attempt %d failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (err != ESP_OK) {
        s_mounted = false;
        return err;
    }

    if (store_sd_spi_lock(5000) != ESP_OK) {
        set_err("spi lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    meta_load_unlocked();
    /* Sanity: if file shorter than head, reset. */
    uint64_t sz = file_size_unlocked(QUEUE_PATH);
    if (s_head > sz) {
        ESP_LOGW(TAG, "meta head past EOF — resetting queue");
        s_head = 0;
        s_count = 0;
        unlink(QUEUE_PATH);
        meta_save_unlocked();
    }
    store_sd_spi_unlock();

    s_mounted = true;
    set_err("");
    ESP_LOGI(TAG, "mounted depth=%u head=%llu", (unsigned)s_count,
             (unsigned long long)s_head);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
#endif
}

bool store_sd_is_mounted(void)
{
    return s_mounted;
}

esp_err_t store_sd_get_status(store_sd_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->mounted = s_mounted;
    snprintf(out->last_error, sizeof(out->last_error), "%s", s_last_err);
    if (!s_mounted) {
        return ESP_OK;
    }
    if (s_q_mu && xSemaphoreTake(s_q_mu, pdMS_TO_TICKS(500)) == pdTRUE) {
        out->depth = s_count;
        out->head_offset = s_head;
        if (store_sd_spi_lock(1000) == ESP_OK) {
            uint64_t sz = file_size_unlocked(QUEUE_PATH);
            out->pending_bytes = (sz > s_head) ? (sz - s_head) : 0;
            store_sd_spi_unlock();
        }
        xSemaphoreGive(s_q_mu);
    }
    return ESP_OK;
}

esp_err_t store_sd_enqueue_line(const char *line, size_t line_len)
{
    if (!s_mounted || line == NULL || line_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_q_mu == NULL || xSemaphoreTake(s_q_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (store_sd_spi_lock(5000) != ESP_OK) {
        xSemaphoreGive(s_q_mu);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
#if CONFIG_STORE_SD_ENABLE
    const uint64_t max_bytes = (uint64_t)CONFIG_STORE_SD_MAX_QUEUE_BYTES;
    for (int i = 0; i < 64; i++) {
        uint64_t sz = file_size_unlocked(QUEUE_PATH);
        uint64_t pending = (sz > s_head) ? (sz - s_head) : 0;
        if (pending + line_len + 1 <= max_bytes) {
            break;
        }
        ESP_LOGW(TAG, "queue full — drop oldest");
        err = drop_oldest_unlocked();
        if (err != ESP_OK) {
            break;
        }
        if (s_head >= COMPACT_HEAD_BYTES) {
            compact_unlocked();
        }
    }

    if (err == ESP_OK) {
        FILE *f = fopen(QUEUE_PATH, "a");
        if (!f) {
            snprintf(s_last_err, sizeof(s_last_err), "queue append open fail errno=%d", errno);
            ESP_LOGW(TAG, "%s path=%s", s_last_err, QUEUE_PATH);
            err = ESP_FAIL;
        } else {
            size_t w = fwrite(line, 1, line_len, f);
            int fe = ferror(f);
            if (w == line_len) {
                if (fputc('\n', f) == EOF) {
                    fe = 1;
                }
            }
            fflush(f);
            fclose(f);
            if (w != line_len || fe) {
                snprintf(s_last_err, sizeof(s_last_err),
                         "queue append short w=%u errno=%d", (unsigned)w, errno);
                ESP_LOGW(TAG, "%s", s_last_err);
                err = ESP_FAIL;
            } else {
                s_count++;
                err = meta_save_unlocked();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "meta_save after append failed");
                } else {
                    set_err("");
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "enqueue pre-check err=%s", esp_err_to_name(err));
    }
    if (s_head >= COMPACT_HEAD_BYTES) {
        compact_unlocked();
    }
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif

    store_sd_spi_unlock();
    xSemaphoreGive(s_q_mu);
    return err;
}

esp_err_t store_sd_peek_lines(char *buf, size_t buf_len, size_t max_lines,
                              size_t *out_lines, size_t *out_byte_span)
{
    if (!s_mounted || buf == NULL || buf_len < 2 || out_lines == NULL ||
        out_byte_span == NULL || max_lines == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_lines = 0;
    *out_byte_span = 0;
    buf[0] = '\0';

    if (s_q_mu == NULL || xSemaphoreTake(s_q_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (store_sd_spi_lock(5000) != ESP_OK) {
        xSemaphoreGive(s_q_mu);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    FILE *f = fopen(QUEUE_PATH, "r");
    if (!f) {
        store_sd_spi_unlock();
        xSemaphoreGive(s_q_mu);
        return ESP_OK; /* empty */
    }
    if (fseek(f, (long)s_head, SEEK_SET) != 0) {
        fclose(f);
        store_sd_spi_unlock();
        xSemaphoreGive(s_q_mu);
        return ESP_FAIL;
    }

    size_t off = 0;
    size_t lines = 0;
    size_t span = 0;
    while (lines < max_lines) {
        if (off + 2 >= buf_len) {
            break;
        }
        char line[1536];
        if (!fgets(line, sizeof(line), f)) {
            break;
        }
        size_t llen = strlen(line);
        if (llen == 0) {
            break;
        }
        /* Strip newline for buffer copy; span includes it. */
        size_t copy_len = llen;
        if (line[copy_len - 1] == '\n') {
            copy_len--;
        }
        if (copy_len == 0) {
            span += llen;
            continue;
        }
        if (off + copy_len + 2 > buf_len) {
            break;
        }
        if (lines > 0) {
            buf[off++] = '\n';
        }
        memcpy(buf + off, line, copy_len);
        off += copy_len;
        buf[off] = '\0';
        span += llen;
        lines++;
    }
    fclose(f);

    *out_lines = lines;
    *out_byte_span = span;
    store_sd_spi_unlock();
    xSemaphoreGive(s_q_mu);
    return err;
}

esp_err_t store_sd_ack_bytes(size_t byte_span, size_t lines)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (byte_span == 0 || lines == 0) {
        return ESP_OK;
    }
    if (s_q_mu == NULL || xSemaphoreTake(s_q_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (store_sd_spi_lock(5000) != ESP_OK) {
        xSemaphoreGive(s_q_mu);
        return ESP_ERR_TIMEOUT;
    }

    s_head += byte_span;
    if (s_count >= lines) {
        s_count -= (uint32_t)lines;
    } else {
        s_count = 0;
    }
    esp_err_t err = meta_save_unlocked();
    if (s_head >= COMPACT_HEAD_BYTES) {
        compact_unlocked();
    }
    /* If empty, truncate for cleanliness. */
    if (s_count == 0) {
        unlink(QUEUE_PATH);
        s_head = 0;
        meta_save_unlocked();
    }

    store_sd_spi_unlock();
    xSemaphoreGive(s_q_mu);
    return err;
}
