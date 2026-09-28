/*
 * TallyPad — custom firmware for the Talli baby-tracker button pad.
 *
 * Buttons: U3 = I2C keypad expander at 0x20, SDA=GPIO22 SCL=GPIO23 (recovered
 * from the stock firmware disassembly; map proven live on hardware:
 * reg0 bits 8..15 = buttons 1..8 in reading order, active HIGH; sync button
 * is hardware reset and never reaches U3).
 *
 * Wi-Fi: credentials never transit chat or firmware. With no stored creds the
 * pad opens AP "BabyPad-Setup"; a phone at http://192.168.4.1 submits
 * SSID/password straight into pad NVS, then it reboots into station mode.
 *
 * Presses POST {"button": N} to the listener (listener/ingest.py, :4180),
 * which turns them into whatever you want. Console window keeps 'dl' (wire reflash),
 * 'ping', and 'wificlear' (drop creds, back to setup mode).
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rmt_tx.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/lp_aon_reg.h"

static const char *TAG = "talli-pad";

#define SDA_PIN 22
#define SCL_PIN 23
#define I2C_PORT I2C_NUM_0
#define U3_ADDR 0x20
#define BOOT_WINDOW_MS 6000
#define INGEST_URL "http://192.168.1.50:4180/press"  /* CHANGE ME: your listener's LAN address */
#define LED_PIN 10               /* stock: pinMode(10,OUTPUT)+low = WS2812 chain */
#define LED_COUNT 9

static nvs_handle_t g_nvs;
static bool g_nvs_ok;
static bool g_wifi_up;
static volatile bool g_want_reconnect;
static esp_err_t u3_wr16(uint8_t reg, uint16_t val);
static esp_err_t u3_rd16(uint8_t reg, uint16_t *out);
static void status_green(void);
static void status_red(void);
static void status_off(void);

/* ------------------------------- console ---------------------------------- */

static void reboot_to_download_mode(void)
{
    ESP_LOGW(TAG, "entering download mode");
    vTaskDelay(pdMS_TO_TICKS(120));
    REG_SET_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void console_set_nonblocking(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

static void poll_console(void)
{
    static char line[16];
    static size_t len = 0;
    uint8_t ch;
    if (read(STDIN_FILENO, &ch, 1) != 1) return;
    if (ch == '\r' || ch == '\n') {
        line[len] = '\0';
        len = 0;
        if (strcmp(line, "dl") == 0) reboot_to_download_mode();
        else if (strcmp(line, "ping") == 0) printf("alive\n");
        else if (line[0] == 'w' && strlen(line) == 7) {
            unsigned reg=0, val=0;
            if (sscanf(line+1, "%2x%4x", &reg, &val) == 2) {
                esp_err_t e = u3_wr16((uint8_t)reg, (uint16_t)val);
                uint16_t rb = 0;
                u3_rd16((uint8_t)reg, &rb);
                printf("poke reg 0x%02x <- 0x%04x (%s, rb 0x%04x)\n",
                       reg, val, e == ESP_OK ? "ok" : "err", rb);
            }
        }
        else if (line[0] == 'r' && strlen(line) == 3) {
            unsigned reg=0;
            if (sscanf(line+1, "%2x", &reg) == 1) {
                uint16_t rb = 0;
                esp_err_t e = u3_rd16((uint8_t)reg, &rb);
                printf("reg 0x%02x = 0x%04x (%s)\n", reg, rb,
                       e == ESP_OK ? "ok" : "err");
            }
        }
        else if (strcmp(line, "wificlear") == 0) {
            if (g_nvs_ok) {
                nvs_erase_key(g_nvs, "ssid");
                nvs_erase_key(g_nvs, "pass");
                nvs_commit(g_nvs);
            }
            printf("wifi creds cleared — rebooting to setup\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        return;
    }
    if (len < sizeof(line) - 1) line[len++] = (char)ch;
    else len = 0;
}

static void boot_console_window(void)
{
    printf("console window open %d ms — 'dl' flash, 'ping', 'wificlear'\n",
           BOOT_WINDOW_MS);
    int64_t end = esp_timer_get_time() + (int64_t)BOOT_WINDOW_MS * 1000;
    while (esp_timer_get_time() < end) {
        poll_console();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("window closed\n\n");
}

/* --------------------------------- U3 -------------------------------------- */

static esp_err_t u3_wr16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xff), (uint8_t)(val >> 8) };
    return i2c_master_write_to_device(I2C_PORT, U3_ADDR, buf, 3,
                                      pdMS_TO_TICKS(50));
}

static esp_err_t u3_rd16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = {0};
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, U3_ADDR, &reg, 1,
                                                 buf, 2, pdMS_TO_TICKS(50));
    *out = (uint16_t)(buf[0] | (buf[1] << 8));
    return err;
}

static void u3_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
    const struct { uint8_t reg; uint16_t val; } seq[] = {
        { 79, 1 }, { 6, 0xff00 }, { 70, 0xff00 },
        { 72, 0 }, { 74, 0xff00 }, { 2, 255 },
    };
    for (size_t i = 0; i < sizeof(seq)/sizeof(seq[0]); i++) {
        u3_wr16(seq[i].reg, seq[i].val);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    printf("U3 initialized\n");
}

/* ------------------------------ Wi-Fi + portal ----------------------------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_up = false;
        g_want_reconnect = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* no printing here: the sys_evt task stack is tiny and printf from
         * this context corrupted its neighbours (two crash dumps, 19:07) */
        g_wifi_up = true;
    }
}

static const char PORTAL_HTML[] =
    "<html><head><meta name=viewport content='width=device-width'>"
    "<title>BabyPad Setup</title></head><body style='font-family:sans-serif;"
    "max-width:24em;margin:2em auto'><h2>BabyPad Wi-Fi Setup</h2>"
    "<form method='POST' action='/save'>"
    "<p>Network name<br><input name='s' style='width:100%%'></p>"
    "<p>Password<br><input name='p' type='password' style='width:100%%'></p>"
    "<p><button style='font-size:1.2em'>Save &amp; connect</button></p>"
    "</form></body></html>";

static esp_err_t portal_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = 0;
}

static esp_err_t portal_save(httpd_req_t *req)
{
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return httpd_resp_send_500(req);
    char ssid[64] = {0}, pass[64] = {0};
    char *sp = strstr(body, "s=");
    char *pp = strstr(body, "p=");
    if (sp) sscanf(sp + 2, "%63[^&]", ssid);
    if (pp) sscanf(pp + 2, "%63[^&]", pass);
    url_decode(ssid);
    url_decode(pass);
    if (!ssid[0]) return httpd_resp_send_500(req);
    if (g_nvs_ok) {
        nvs_set_str(g_nvs, "ssid", ssid);
        nvs_set_str(g_nvs, "pass", pass);
        nvs_commit(g_nvs);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>Saved. BabyPad is connecting to your Wi-Fi — "
                    "you can close this page.</h2>", HTTPD_RESP_USE_STRLEN);
    printf("portal: creds saved for ssid '%s' — rebooting\n", ssid);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void start_setup_ap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    wifi_config_t ap = { 0 };
    strcpy((char *)ap.ap.ssid, "BabyPad-Setup");
    ap.ap.ssid_len = strlen("BabyPad-Setup");
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    httpd_uri_t uget = { .uri = "/", .method = HTTP_GET, .handler = portal_get };
    httpd_uri_t usave = { .uri = "/save", .method = HTTP_POST, .handler = portal_save };
    httpd_register_uri_handler(srv, &uget);
    httpd_register_uri_handler(srv, &usave);
    printf("SETUP MODE: join Wi-Fi 'BabyPad-Setup', open http://192.168.4.1\n");
    while (1) { poll_console(); vTaskDelay(pdMS_TO_TICKS(20)); }
}

static bool start_sta(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("joining '%s'...\n", ssid);
    for (int i = 0; i < 300 && !g_wifi_up; i++) {
        poll_console();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (g_wifi_up) printf("wifi up\n");
    return g_wifi_up;
}

/* ---------------------------- WS2812 button LEDs --------------------------- */

static rmt_channel_handle_t g_led_chan;
static rmt_encoder_handle_t g_led_enc;

static void leds_init(void)
{
    rmt_tx_channel_config_t cc = {
        .gpio_num = LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,      /* 10 MHz: 1 tick = 0.1 us */
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&cc, &g_led_chan) != ESP_OK) {
        printf("LED: rmt channel failed\n");
        return;
    }
    rmt_bytes_encoder_config_t be = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&be, &g_led_enc) != ESP_OK) {
        printf("LED: encoder failed\n");
        return;
    }
    rmt_enable(g_led_chan);
    gpio_reset_pin(GPIO_NUM_20);
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_20, 1);
    gpio_reset_pin(GPIO_NUM_21);
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    status_red();                  /* red until Wi-Fi is up */
    printf("LEDs ready: status LED active-low on 20/21\n");
}

static void leds_show(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_led_chan || !g_led_enc) return;
    uint8_t buf[LED_COUNT * 3] = {0};
    if (index >= 0 && index < LED_COUNT) {
        buf[index * 3 + 0] = g;    /* WS2812 is GRB */
        buf[index * 3 + 1] = r;
        buf[index * 3 + 2] = b;
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(g_led_chan, g_led_enc, buf, sizeof(buf), &tc) == ESP_OK) {
        rmt_tx_wait_all_done(g_led_chan, 100);
    }
    esp_rom_delay_us(300);         /* WS2812 latch gap */
}

/* VJ 20:16: GPIO20/21 drive the STATUS LED, ACTIVE LOW (both-low showed
 * steady red; driving high turned it off). Assume 21=green cathode, 20=red;
 * a color report the other way means swapping these two lines.
 * Button lamps are U3 registers and need a COMBINATION: on-bits in reg 2
 * high byte plus brightness in reg 0x48 (init leaves 0x48 = 0 = dark, which
 * is why single-register pokes lit nothing but the cumulative 19:43
 * sequence lit everything). */
static void status_green(void) { gpio_set_level(GPIO_NUM_21, 0); gpio_set_level(GPIO_NUM_20, 1); }
static void status_red(void)   { gpio_set_level(GPIO_NUM_20, 0); gpio_set_level(GPIO_NUM_21, 1); }
static void status_off(void)   { gpio_set_level(GPIO_NUM_20, 1); gpio_set_level(GPIO_NUM_21, 1); }

/* VJ 20:22: GPIO20/21 = the two-color notification LED at the sync position
 * (GPIO20 high = green, confirmed working). The per-button white backlight is
 * a separate U3 register, not yet pinned; the notification LED IS the press
 * confirmation. ok = one green blink then back to steady green (Wi-Fi up);
 * fail = two red blinks then whatever the Wi-Fi state shows. */
static void led_flash(int button, bool ok)
{
    if (ok) {
        /* White backlight: reg 2 bits are active-LOW per lamp (v28 with the
         * button's bits SET lit every lamp except the pressed one — VJ
         * 20:58), gated by reg 0x48 brightness. Clear only the pressed
         * button's bit in both bytes so its own lamp lights alone. */
        uint16_t bits = (uint16_t)~((1u << (button - 1)) | (1u << (button + 7)));
        u3_wr16(2, bits);
        u3_wr16(0x48, 0xffff);
        status_green();
        vTaskDelay(pdMS_TO_TICKS(350));
        u3_wr16(0x48, 0x0000);
        u3_wr16(2, 0x00ff);   /* stock init value; 0x48=0 keeps lamps dark */
        status_off();
        /* main loop restores steady green/red from wifi state */
    } else {
        for (int i = 0; i < 2; i++) {
            status_red(); vTaskDelay(pdMS_TO_TICKS(150));
            status_off(); vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
}

/* ------------------------------ press posting ------------------------------ */

static void post_press(int button)
{
    char body[32];
    snprintf(body, sizeof(body), "{\"button\": %d}", button);
    esp_http_client_config_t cfg = {
        .url = INGEST_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int code = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    printf("press %d -> POST %s (http %d)\n", button,
           err == ESP_OK ? "ok" : esp_err_to_name(err), code);
    esp_http_client_cleanup(c);
    led_flash(button, err == ESP_OK && code == 200);
}

/* ---------------------------------- main ----------------------------------- */

void app_main(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    console_set_nonblocking();
    esp_log_level_set("*", ESP_LOG_ERROR);

    printf("\n\n=== talli-pad v29 (per-button backlight, inverted bits) ===\n");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    g_nvs_ok = (err == ESP_OK &&
                nvs_open("talli", NVS_READWRITE, &g_nvs) == ESP_OK);

    boot_console_window();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[64] = {0}, pass[64] = {0};
    size_t sl = sizeof(ssid), pl = sizeof(pass);
    bool have = g_nvs_ok &&
                nvs_get_str(g_nvs, "ssid", ssid, &sl) == ESP_OK &&
                nvs_get_str(g_nvs, "pass", pass, &pl) == ESP_OK;

    if (!have) {
        start_setup_ap();       /* never returns */
    }
    u3_init();                  /* bring the keypad up before the radio */
    leds_init();
    leds_show(-1, 0, 0, 0);     /* all dark */
    if (!start_sta(ssid, pass)) {
        printf("could not join '%s' in 30 s — keeping trying in background\n",
               ssid);
    }

    printf("--- watching buttons ---\n");
    uint16_t prev = 0;
    int stable = 0;
    while (1) {
        uint16_t v = 0;
        if (u3_rd16(0, &v) == ESP_OK) {
            uint16_t keys = (v >> 8) & 0xff;          /* bits 8..15 -> 1..8 */
            uint16_t pkeys = (prev >> 8) & 0xff;
            uint16_t rising = keys & (uint16_t)~pkeys;
            if (rising) {
                for (int b = 0; b < 8; b++) {
                    if (rising & (1u << b)) {
                        printf("button %d pressed\n", b + 1);
                        if (g_wifi_up) post_press(b + 1);
                        else {
                            printf("  (wifi down — press dropped)\n");
                            led_flash(b + 1, false);
                        }
                    }
                }
            }
            prev = v;
            stable = 0;
        } else if (++stable == 100) {
            printf("I2C errors — reinitializing bus\n");
            i2c_driver_delete(I2C_PORT);
            u3_init();
            stable = 0;
        }
        static int shown_up = -1;
        if ((int)g_wifi_up != shown_up) {
            shown_up = (int)g_wifi_up;
            if (g_wifi_up) status_green(); else status_red();
        }
        if (g_want_reconnect) {
            g_want_reconnect = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_connect();
        }
        poll_console();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
