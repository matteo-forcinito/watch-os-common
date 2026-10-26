/*
 * Development console (CONFIG_WATCH_DEV_CONSOLE, off in normal builds).
 *
 * Commands typed on the USB/serial console, one per line, to test a device
 * from the PC without touching it (tools/devcon.py sends them and turns the
 * screenshots into PNG files):
 *
 *   help                 this list
 *   shot                 screenshot of the screen (+ dialogs on top), base64 lines
 *   apps                 the registered app ids
 *   open <id>            open an app            back | home
 *   knob <n>             turn the knob n detents (negative = left)
 *   swipe x1 y1 x2 y2    a finger moving across the screen (never a tap)
 *   wake | sleep         screen on / off
 *   gpio <n>             level of a GPIO (input pins only, read without changing them)
 *   pmu                  battery, USB, charging as the board sees them
 *   health               the health report (RAM, stacks, slowest UI pass)
 *
 * Output lines start with '@' so they can be told apart from the log.
 */

#include "core/devcon.h"

#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/recovery.h"
#include "core/sys.h"
#include "core/settings.h"

#include "hardware/pmu.h"
#include "hardware/microphone.h"

#include "sdkconfig.h"

static const char *TAG = "devcon";

/* Optional diagnostic hook: core does not depend on network services. */
extern void wifi_diagnostic_status(void) __attribute__((weak));
extern void wifi_diagnostic_drop(void) __attribute__((weak));

#if CONFIG_WATCH_DEV_CONSOLE

#include "driver/gpio.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#include "esp_heap_caps.h"
static heap_trace_record_t *s_trace;
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

#include <stdio.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define TASK_STACK      6144
#define LINE_LEN        128
#define B64_CHUNK       57      /* bytes per output line: 76 base64 characters */

/* ------------------------------------------------- run in the lvgl task */

typedef struct
{
    void (*fn)(void *arg);
    void *arg;
    SemaphoreHandle_t done;
} ui_call_t;

static void ui_call_cb(void *arg)
{
    ui_call_t *call = arg;
    call->fn(call->arg);
    xSemaphoreGive(call->done);
}

/*
 * Run fn(arg) in the lvgl task and wait for it (this task may block).
 * No timeout: `call` and `arg` live on this stack, so returning before the
 * lvgl task ran it would leave it a dangling pointer (crash seen with a 5 s
 * timeout while a slow screen opened). A stuck lvgl task is the watchdog's job.
 */
static void run_in_ui(void (*fn)(void *arg), void *arg)
{
    ui_call_t call = {.fn = fn, .arg = arg, .done = xSemaphoreCreateBinary()};

    if (call.done == NULL)
    {
        return;
    }

    if (ui_async(ui_call_cb, &call))
    {
        xSemaphoreTake(call.done, portMAX_DELAY);
    }

    vSemaphoreDelete(call.done);
}

/* ------------------------------------------------------------ base64 */

static void print_base64(uint32_t row, const uint8_t *data, size_t length)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[(B64_CHUNK / 3) * 4 + 8];

    while (length > 0)
    {
        size_t n = length > B64_CHUNK ? B64_CHUNK : length;
        size_t o = 0;

        for (size_t i = 0; i < n; i += 3)
        {
            uint32_t v = (uint32_t)data[i] << 16;
            v |= (i + 1 < n) ? (uint32_t)data[i + 1] << 8 : 0;
            v |= (i + 2 < n) ? (uint32_t)data[i + 2] : 0;

            out[o++] = table[(v >> 18) & 63];
            out[o++] = table[(v >> 12) & 63];
            out[o++] = (i + 1 < n) ? table[(v >> 6) & 63] : '=';
            out[o++] = (i + 2 < n) ? table[v & 63] : '=';
        }

        out[o] = '\0';
        /* The stdout VFS may discard a full USB TX buffer. Snapshot payloads
         * need backpressure, otherwise missing chunks corrupt entire rows. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        char line[112];
        int len = snprintf(line, sizeof(line), "@D %u %s\n", (unsigned)row, out);
        fflush(stdout);
        usb_serial_jtag_write_bytes(line, (size_t)len, portMAX_DELAY);
#else
        printf("@D %u %s\n", (unsigned)row, out);
#endif
        data += n;
        length -= n;
    }
}

/* WAV-only file inspection for local hardware validation. Never dumps
 * settings, credentials or arbitrary files; absent from production builds. */
static void cmd_wav(const char *path)
{
    size_t length = strlen(path);
    if ((strncmp(path, "/sdcard/", 8) != 0 && strncmp(path, "/data/", 6) != 0) ||
        strstr(path, "..") != NULL || strchr(path, '\\') != NULL ||
        length < 12 || length > 100 || strcmp(path + length - 4, ".wav") != 0)
    {
        printf("@ERR WAV path required\n");
        return;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
    {
        if (file != NULL)
        {
            fclose(file);
        }
        printf("@ERR WAV not readable\n");
        return;
    }
    long size = ftell(file);
    if (size < 44 || size > 2000000 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        printf("@ERR WAV size\n");
        return;
    }
    printf("@WAV %ld\n", size);
    uint8_t buffer[B64_CHUNK];
    uint32_t offset = 0;
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        print_base64(offset, buffer, count);
        offset += count;
    }
    fclose(file);
    printf("@ENDWAV %u\n", (unsigned)offset);
    fflush(stdout);
}

/* ----------------------------------------------------------- screenshot */

typedef struct
{
    lv_draw_buf_t *screen;
    lv_draw_buf_t *top;
} shot_t;

static void take_shot(void *arg)
{
    shot_t *shot = arg;

    shot->screen = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);

    /* Dialogs and toasts live on the top layer: a second picture with transparency. */
    bool overlay_visible = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); i++)
    {
        lv_obj_t *child = lv_obj_get_child(lv_layer_top(), (int32_t)i);
        if (!lv_obj_is_hidden(child) && lv_obj_get_style_opa(child, 0) != LV_OPA_TRANSP)
            overlay_visible = true;
    }
    if (overlay_visible)
    {
        shot->top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    }
}

static void free_shot(void *arg)
{
    shot_t *shot = arg;

    if (shot->screen != NULL)
    {
        lv_draw_buf_destroy(shot->screen);
    }

    if (shot->top != NULL)
    {
        lv_draw_buf_destroy(shot->top);
    }
}

static void print_buf(const char *name, const lv_draw_buf_t *buf)
{
    const lv_image_header_t *h = &buf->header;
    int bpp = h->cf == LV_COLOR_FORMAT_RGB565 ? 2 : 4;

    printf("@IMG %s %u %u %d\n", name, (unsigned)h->w, (unsigned)h->h, bpp);

    for (uint32_t y = 0; y < h->h; y++)
    {
        print_base64(y, buf->data + y * h->stride, (size_t)h->w * bpp);
        fflush(stdout);

        /* USB CDC is non-blocking: pace each row so a snapshot is not silently truncated. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void cmd_shot(bool wake)
{
    shot_t shot = {0};

    if (wake) {
        power_wake(POWER_WAKE_OTHER);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    run_in_ui(take_shot, &shot);

    if (shot.screen == NULL)
    {
        printf("@ERR snapshot failed (out of memory?)\n");
        return;
    }

    print_buf("screen", shot.screen);

    if (shot.top != NULL)
    {
        print_buf("top", shot.top);
    }

    printf("@END\n");
    run_in_ui(free_shot, &shot);
}

/* ------------------------------------------------------- other commands */

static void list_apps(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < app_count(); i++)
    {
        const app_t *app = app_at(i);
        printf("@APP %s \"%s\"%s\n", app->id, app->name, (app->flags & APP_FLAG_HIDDEN) ? " hidden" : "");
    }

    const app_t *current = app_current();
    printf("@CURRENT %s\n", current != NULL ? current->id : "home");
}

static void open_cb(void *arg)
{
    if (!app_open((const char *)arg, NULL))
    {
        printf("@ERR cannot open %s\n", (const char *)arg);
    }
}

static void back_cb(void *arg)
{
    (void)arg;
    app_back();
}

static void home_cb(void *arg)
{
    (void)arg;
    app_home();
}

static void swipe(int x1, int y1, int x2, int y2)
{
    const int steps = 10;

    power_wake(POWER_WAKE_OTHER);

    for (int i = 0; i <= steps; i++)
    {
        lv_port_fake_touch(true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    lv_port_fake_touch(false, x2, y2);
}

static void cmd_pmu(void)
{
    printf("@PMU present=%d battery=%d%% mv=%d usb=%d charging=%d battery_present=%d\n", pmu_present(),
           pmu_battery_percent(), pmu_battery_mv_fresh(), pmu_usb_present(), pmu_is_charging(),
           pmu_battery_present());
}

/* Development-only, synthetic 16 kHz PCM. Never overwrite a saved memo. */
static void wav_tone(void)
{
    char path[96];
    snprintf(path, sizeof(path), "/sdcard/memos/memo-%08lx-999.wav", (unsigned long)time(NULL));
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
    if (file == NULL)
    {
        if (fd >= 0)
        {
            close(fd);
            remove(path);
        }
        printf("@ERR test WAV could not be created\n");
        return;
    }
    const uint8_t header[44] = {
        'R','I','F','F', 0xA4,0xBB,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0,1,0, 0x80,0x3E,0,0,
        0,0x7D,0,0, 2,0,16,0, 'd','a','t','a',0x80,0xBB,0,0
    };
    bool ok = fwrite(header, 1, 44, file) == 44;
    int16_t samples[256];
    for (int offset = 0; ok && offset < 24000; offset += 256)
    {
        int count = 24000 - offset < 256 ? 24000 - offset : 256;
        for (int i = 0; i < count; i++)
        {
            int pos = offset + i;
            float envelope = pos < 800 ? pos / 800.0f : pos >= 23200 ? (23999 - pos) / 800.0f : 1.0f;
            samples[i] = (int16_t)(12000.0f * envelope * sinf(2.0f * 3.14159265f * 880.0f * pos / 16000.0f));
        }
        ok = fwrite(samples, sizeof(int16_t), count, file) == (size_t)count;
    }
    ok = fflush(file) == 0 && ok;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (!ok)
    {
        remove(path);
        printf("@ERR test WAV write failed\n");
    }
    else
    {
        printf("@TESTWAV %s\n", path + strlen("/sdcard/memos/"));
    }
}

static void navigation_button(void *arg)
{
    (void)arg;
    app_manager_button(BUTTON_BOOT, BUTTON_CLICK);
}

__attribute__((weak)) bool devcon_app_command(char *line)
{
    (void)line;
    return false;
}

static void handle(char *line)
{
    if (devcon_app_command(line)) return;
    char *cmd = strtok(line, " ");

    if (cmd == NULL)
    {
        return;
    }

    char *a1 = strtok(NULL, " ");
    char *a2 = strtok(NULL, " ");
    char *a3 = strtok(NULL, " ");
    char *a4 = strtok(NULL, " ");

    if (strcmp(cmd, "mic") == 0)
    {
        /* One second at 16 kHz: level and peak, to check the microphones. */
        static int16_t buf[256];
        esp_err_t err = microphone_start(16000, 1);
        double sum = 0;
        int peak = 0;
        size_t total = 0;
        while (err == ESP_OK && total < 16000)
        {
            size_t n = 0;
            err = microphone_read(buf, 256, &n);
            for (size_t i = 0; i < n; i++)
            {
                sum += (double)buf[i] * buf[i];
                peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
            }
            total += n;
        }
        microphone_stop();
        printf("@MIC %s samples=%u rms=%.0f peak=%d\n", esp_err_to_name(err), (unsigned)total,
               total ? sqrt(sum / total) : 0.0, peak);
    }
    else if (strcmp(cmd, "wifi-status") == 0)
    {
        if (wifi_diagnostic_status != NULL) wifi_diagnostic_status();
    }
    else if (strcmp(cmd, "wifi-drop") == 0)
    {
        if (wifi_diagnostic_drop != NULL)
        {
            wifi_diagnostic_drop();
            printf("@WIFI station disconnected for recovery QA\n");
        }
        else printf("@ERR Wi-Fi diagnostics unavailable\n");
    }
    else if (strcmp(cmd, "recovery") == 0)
    {
        bool enabled = a1 != NULL && strcmp(a1, "on") == 0;
        if (a1 != NULL && (enabled || strcmp(a1, "off") == 0))
        {
            printf("@RECOVERY request=%d accepted=%d\n", enabled, recovery_request(enabled));
        }
        else printf("@RECOVERY active=%d\n", recovery_active());
    }
    else if (strcmp(cmd, "shot") == 0 || strcmp(cmd, "shot-idle") == 0)
    {
        cmd_shot(strcmp(cmd, "shot") == 0);
        return;   /* @END already printed */
    }
    else if (strcmp(cmd, "apps") == 0)
    {
        run_in_ui(list_apps, NULL);
    }
#if CONFIG_HEAP_TRACING_STANDALONE
    else if (strcmp(cmd, "heapstart") == 0)
    {
        if (s_trace == NULL)
        {
            s_trace = heap_caps_calloc(1024, sizeof(*s_trace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        esp_err_t err = s_trace != NULL ? heap_trace_init_standalone(s_trace, 1024) : ESP_ERR_NO_MEM;
        if (err == ESP_OK)
        {
            err = heap_trace_start(HEAP_TRACE_LEAKS);
        }
        printf("@TRACE %s\n", esp_err_to_name(err));
    }
    else if (strcmp(cmd, "heapstop") == 0)
    {
        esp_err_t err = heap_trace_stop();
        heap_trace_dump_caps(MALLOC_CAP_SPIRAM);
        printf("@TRACE %s\n", esp_err_to_name(err));
    }
#endif
    else if (strcmp(cmd, "open") == 0 && a1 != NULL)
    {
        static char id[48];   /* app_open() may queue the request: keep the string alive */
        snprintf(id, sizeof(id), "%s", a1);
        power_wake(POWER_WAKE_OTHER);
        run_in_ui(open_cb, id);
    }
    else if (strcmp(cmd, "back") == 0)
    {
        run_in_ui(back_cb, NULL);
    }
    else if (strcmp(cmd, "home") == 0)
    {
        run_in_ui(home_cb, NULL);
    }
    else if (strcmp(cmd, "knob") == 0 && a1 != NULL)
    {
        lv_port_knob_inject(atoi(a1));
    }
    else if (strcmp(cmd, "swipe") == 0 && a4 != NULL)
    {
        swipe(atoi(a1), atoi(a2), atoi(a3), atoi(a4));
    }
    else if ((strcmp(cmd, "press") == 0 || strcmp(cmd, "press-idle") == 0) && a3 != NULL)
    {
        int ms = atoi(a3);
        if (ms < 50 || ms > 61000)
        {
            printf("@ERR press duration 50..61000 ms\n");
            return;
        }
        if (strcmp(cmd, "press") == 0) power_wake(POWER_WAKE_OTHER);
        lv_port_fake_touch(true, atoi(a1), atoi(a2));
        vTaskDelay(pdMS_TO_TICKS(ms));
        lv_port_fake_touch(false, atoi(a1), atoi(a2));
    }
    else if (strcmp(cmd, "touch") == 0 && a3 != NULL)
    {
        power_wake(POWER_WAKE_OTHER);
        lv_port_fake_touch(atoi(a3) != 0, atoi(a1), atoi(a2));
    }
    else if (strcmp(cmd, "button") == 0)
    {
        run_in_ui(navigation_button, NULL);
    }
    else if (strcmp(cmd, "loglevel") == 0 && a1 != NULL)
    {
        int level = atoi(a1);
        if (level < ESP_LOG_NONE || level > ESP_LOG_VERBOSE)
        {
            printf("@ERR loglevel 0..5\n");
            return;
        }
        esp_log_level_set("*", (esp_log_level_t)level);
    }
    else if (strcmp(cmd, "wavtone") == 0)
    {
        wav_tone();
    }
    else if (strcmp(cmd, "wavlist") == 0)
    {
        DIR *dir = opendir("/sdcard/memos");
        if (dir != NULL)
        {
            struct dirent *entry;
            int count = 0;
            while ((entry = readdir(dir)) != NULL && count++ < 256)
            {
                size_t length = strlen(entry->d_name);
                if (length > 9 && length < 48 && strncmp(entry->d_name, "memo-", 5) == 0 &&
                    strcmp(entry->d_name + length - 4, ".wav") == 0)
                {
                    printf("@MEMO %s\n", entry->d_name);
                }
            }
            closedir(dir);
        }
    }
    else if (strcmp(cmd, "wav") == 0 && a1 != NULL)
    {
        cmd_wav(a1);
        return;
    }
    else if (strcmp(cmd, "wake") == 0)
    {
        power_wake(POWER_WAKE_OTHER);
    }
    else if (strcmp(cmd, "sleep") == 0)
    {
        power_sleep_now();
    }
    else if (strcmp(cmd, "power-state") == 0)
    {
        printf("@POWER screen=%d tap=%d aod=%d\n", power_screen(), settings_get()->tap_to_wake, settings_get()->always_on);
    }
    else if (strcmp(cmd, "gpio") == 0 && a1 != NULL)
    {
        int pin = atoi(a1);
        printf("@GPIO %d %d\n", pin, GPIO_IS_VALID_GPIO(pin) ? gpio_get_level((gpio_num_t)pin) : -1);
    }
    else if (strcmp(cmd, "pmu") == 0)
    {
        cmd_pmu();
    }
    else if (strcmp(cmd, "perf") == 0)
    {
        lv_port_perf_t perf;
        lv_port_perf_snapshot(&perf, a1 != NULL && strcmp(a1, "reset") == 0);
        printf("@PERF passes=%lu over16=%lu over33=%lu max_ms=%lu internal=%u largest=%u psram=%u\n",
               (unsigned long)perf.passes, (unsigned long)perf.slow_16ms, (unsigned long)perf.slow_33ms,
               (unsigned long)perf.max_ms, (unsigned)sys_internal_free(), (unsigned)sys_internal_largest(),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    else if (strcmp(cmd, "tasks") == 0)
    {
        sys_task_info_t *tasks = heap_caps_calloc(48, sizeof(*tasks), MALLOC_CAP_SPIRAM);
        size_t n = tasks != NULL ? sys_tasks_snapshot(tasks, 48) : 0;
        for (size_t i = 0; i < n; i++)
            printf("@TASK %s state=%d stack=%lu free_min=%lu reason=%s\n", tasks[i].name,
                   tasks[i].state, (unsigned long)tasks[i].stack_bytes,
                   (unsigned long)tasks[i].stack_free_min, sys_task_purpose(tasks[i].name));
        free(tasks);
    }
    else if (strcmp(cmd, "health") == 0)
    {
        sys_health_log();
    }
    else if (strcmp(cmd, "help") == 0)
    {
        printf("@HELP shot apps open back home knob swipe press touch wav wavlist wavtone button loglevel wake sleep gpio pmu health\n");
    }
    else
    {
        printf("@ERR unknown command (help)\n");
        return;
    }

    printf("@OK %s\n", cmd);
}

/* ---------------------------------------------------------------- task */

static void open_console(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = 1024;
    config.tx_buffer_size = 4096;

    if (usb_serial_jtag_driver_install(&config) == ESP_OK)
    {
        usb_serial_jtag_vfs_use_driver();   /* blocking reads, and writes that wait for the PC */
    }
#elif CONFIG_ESP_CONSOLE_UART
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0) == ESP_OK)
    {
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    }
#endif

    setvbuf(stdin, NULL, _IONBF, 0);
}

static void devcon_task(void *arg)
{
    (void)arg;

    char line[LINE_LEN];

    open_console();
    ESP_LOGW(TAG, "Development console on: type 'help'");

    while (true)
    {
        if (fgets(line, sizeof(line), stdin) == NULL)
        {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';
        handle(line);
        fflush(stdout);
    }
}

void devcon_start(void)
{
    sys_task_create(devcon_task, "devcon", TASK_STACK, NULL, 2, SYS_CORE_ANY);
}

#else

void devcon_start(void)
{
    (void)TAG;
}

#endif
