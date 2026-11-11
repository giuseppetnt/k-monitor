// ==================================================
// KERBEROS
// Monitor server su display touch
//
// Hardware : Waveshare ESP32-S3-Touch-LCD-5B
// Ambiente : Arduino IDE
// Grafica  : LVGL 9 + EEZ Studio
//
// Funzionalita':
//  1. Client HTTPS persistente con keep-alive
//  2. Task rete dedicato su core 0
//  3. Orologio NTP tramite lv_timer (dashboard + tamagochi)
//  4. Dashboard server e sicurezza
//  5. Monitor temperature e ventola
//  6. Tre grafici storici da 60 campioni
//  7. Meteo Open-Meteo
//  8. Configurazione WiFi con reti salvate
//  9. Upload GIF del tamagochi da browser (http://IP-ESP/)
// 10. Screenshot del display da browser (http://IP-ESP/screenshot)
// 11. Indicatori diagnostici sempre visibili (WiFi + stato ESP32)
// ==================================================

// ==================================================
// 1. LIBRERIE
// ==================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdint.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <esp_sntp.h>
#include <math.h>

#include <esp_err.h>
#include "driver/temperature_sensor.h"
#include <esp_heap_caps.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>

#include "esp_lv_adapter_arduino.h"
#include <KerberosUI.h>
#include "ui/images.h"

// Dati sensibili (URL server, credenziali web): file locale NON versionato.
// Copia secrets.example.h in secrets.h e compila i valori.
#include "secrets.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;


// ==================================================
// 2. CONFIGURAZIONE
// ==================================================

// Server Python: SERVER_URL e' definito in secrets.h

// Open-Meteo
static const char *WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=45.0703"
    "&longitude=7.6869"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,weather_code,wind_speed_10m,wind_direction_10m,surface_pressure"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
    "&forecast_days=5"
    "&timezone=Europe%2FRome";

// Intervalli di aggiornamento (default, modificabili dalle Impostazioni)
// Server: da 1 a 60 secondi, passo 1 secondo
static const unsigned long DEFAULT_POLL_INTERVAL_MS = 5000;
static const unsigned long POLL_MIN_MS  = 1000;
static const unsigned long POLL_MAX_MS  = 60000;
static const unsigned long POLL_STEP_MS = 1000;

// Meteo: da 1 a 120 minuti, passo 1 minuto
static const unsigned long DEFAULT_WEATHER_INTERVAL_MS = 600000;
static const unsigned long WEATHER_MIN_MS  = 60000;
static const unsigned long WEATHER_MAX_MS  = 7200000;
static const unsigned long WEATHER_STEP_MS = 60000;

// Namespace Preferences
static const char *SETTINGS_NAMESPACE = "kerberos_cfg";
static const char *WIFI_NAMESPACE     = "kerberos_wifi";

// Task rete
static const uint32_t    NET_TASK_STACK    = 12 * 1024;
static const UBaseType_t NET_TASK_PRIORITY = 1;
static const BaseType_t  NET_TASK_CORE     = 0;

// Widget BTOP
#define ENABLE_STATUS_LED  1
#define ENABLE_BTOP_GAUGES 1

// Soglie
static const int TEMP_MAX_C  = 90;
static const int TEMP_WARN_C = 55;
static const int TEMP_CRIT_C = 70;
static const int FAN_MAX_RPM = 3000;

// Numero campioni grafici
static const uint16_t CHART_POINTS = 60;

// Colori
static const uint32_t COLOR_OK   = 0x00E676;
static const uint32_t COLOR_WARN = 0xFFA000;
static const uint32_t COLOR_BAD  = 0xFF1744;

// WiFi
static const int MAX_SAVED_NETWORKS = 10;

// Web server (upload GIF / screenshot): WEB_USER e WEB_PASS sono in secrets.h
static const char *GIF_PATH     = "/images/kenny.gif";
static const char *GIF_TMP_PATH = "/images/upload.tmp";

// Ritardo tra connessione automatica e apertura dashboard
static const unsigned long AUTO_DASHBOARD_DELAY_MS = 2000;

// Posizione etichetta SSID/IP nella schermata WiFi
static const int IP_LABEL_X = 33;
static const int IP_LABEL_Y = 430;


// ==================================================
// 3. ALIAS EEZ STUDIO
// ==================================================

#define status_label     objects.lbl_status
#define wifi_dropdown    objects.dd_wifi
#define password_area    objects.ta_password
#define scan_button      objects.btn_scan
#define connect_button   objects.btn_connect
#define dashboard_button objects.btn_dashboard
#define btop_button      objects.btn_go_btop


// ==================================================
// 4. STATO GLOBALE
// ==================================================

static Board *board = nullptr;

// Screenshot seriale
static volatile bool screenshot_requested = false;
static volatile bool net_task_paused = false;

// Widget WiFi
static lv_obj_t *keyboard = nullptr;
static lv_obj_t *ip_label = nullptr;

// Memoria persistente
static Preferences wifiPrefs;
static Preferences settingsPrefs;

static String pending_wifi_ssid;
static String pending_wifi_password;

// Stato WiFi
static volatile bool scanning = false;
static volatile bool connecting = false;
static int network_count = 0;
static unsigned long last_wifi_check = 0;
static unsigned long connection_started_at = 0;

// Auto-connessione all'avvio
static volatile bool auto_connect_pending = false;  // scansione di avvio in corso
static bool auto_dashboard_pending = false;         // connessione automatica in corso
static bool dashboard_scheduled = false;            // dashboard da aprire tra 2 s
static unsigned long dashboard_at = 0;

// Intervalli correnti (modificabili a runtime)
static volatile unsigned long poll_interval_ms    = DEFAULT_POLL_INTERVAL_MS;
static volatile unsigned long weather_interval_ms = DEFAULT_WEATHER_INTERVAL_MS;

// Polling server
static volatile bool polling_active = false;
static volatile bool poll_now = false;
static unsigned long last_server_update = 0;

// Polling meteo
static volatile bool weather_screen_active = false;
static volatile bool weather_refresh_now = false;
static volatile bool weather_fetching = false;
static volatile unsigned long last_weather_update = 0;

// Orologio NTP
static bool ntp_configured = false;
static volatile uint32_t last_ntp_sync_ms = 0;
static volatile bool ntp_sync_seen = false;

// Client HTTPS persistente
static WiFiClientSecure server_client;
static bool server_client_ready = false;

// Web server upload GIF
static WebServer web(80);
static File upload_file;
static bool web_started = false;

// Serie grafici
static lv_chart_series_t *fan_rpm_series = nullptr;
static lv_chart_series_t *cpu_temp_series = nullptr;
static lv_chart_series_t *board_temp_series = nullptr;


// ==================================================
// 5. UTILITA'
// ==================================================

static void fatal_error(const char *message)
{
    Serial.println(message);

    while (true) {
        delay(1000);
    }
}


// Converte byte in GB con un decimale (arrotondato)
static void format_gb(uint64_t bytes, unsigned long *whole, unsigned long *decimal)
{
    const uint64_t tenths = (bytes * 10ULL + 536870912ULL) / 1073741824ULL;

    *whole   = (unsigned long)(tenths / 10ULL);
    *decimal = (unsigned long)(tenths % 10ULL);
}


static unsigned long clamp_ul(unsigned long value, unsigned long lo, unsigned long hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;

    return value;
}


// Somma un delta a un valore e lo limita tra lo e hi
static unsigned long step_value(unsigned long value, long delta, unsigned long lo, unsigned long hi)
{
    long result = (long)value + delta;

    if (result < (long)lo) result = (long)lo;
    if (result > (long)hi) result = (long)hi;

    return (unsigned long)result;
}


// ==================================================
// 6. HELPER INTERFACCIA
// ==================================================

static void set_status(const char *message)
{
    lv_label_set_text(status_label, message);
}


static void apply_server_state(bool online)
{
    lv_label_set_text(objects.lbl_status_srv_1, online ? "ONLINE" : "OFFLINE");

#if ENABLE_STATUS_LED
    lv_led_set_color(objects.led_server, lv_color_hex(online ? COLOR_OK : COLOR_BAD));
    lv_led_on(objects.led_server);
#endif
}


static void show_server_offline()
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    apply_server_state(false);
    esp_lv_adapter_unlock();
}


// Chiude la sessione HTTPS e segnala il server offline
static void server_fail()
{
    server_client.stop();
    show_server_offline();
}


// ==================================================
// 7. CONTATORI CIRCOLARI BTOP
// ==================================================

#if ENABLE_BTOP_GAUGES

static void setup_gauge(lv_obj_t *arc, int max_value)
{
    lv_arc_set_range(arc, 0, max_value);
    lv_arc_set_value(arc, 0);

    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
}

#endif


static void init_btop_gauges()
{
#if ENABLE_BTOP_GAUGES
    setup_gauge(objects.arc_cpu_temp, TEMP_MAX_C);
    setup_gauge(objects.arc_temp1, TEMP_MAX_C);
    setup_gauge(objects.arc_fan1, FAN_MAX_RPM);
#endif
}


// ==================================================
// 8. GRAFICI STORICI
// ==================================================

static lv_chart_series_t *init_chart(lv_obj_t *chart, int max_value)
{
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, max_value);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

    lv_chart_series_t *series = lv_chart_add_series(
        chart,
        lv_obj_get_style_line_color(chart, LV_PART_ITEMS),
        LV_CHART_AXIS_PRIMARY_Y
    );

    lv_chart_set_all_values(chart, series, 0);

    return series;
}


static void init_charts()
{
    fan_rpm_series    = init_chart(objects.chart_fan_rpm, FAN_MAX_RPM);
    cpu_temp_series   = init_chart(objects.chart_cpu,     TEMP_MAX_C);
    board_temp_series = init_chart(objects.chart_board,   TEMP_MAX_C);
}


// Aggiunge un campione al grafico, limitandolo a 0..max_value
static void push_chart_value(lv_obj_t *chart, lv_chart_series_t *series, int value, int max_value)
{
    if (series == nullptr) {
        return;
    }

    if (value < 0) value = 0;
    if (value > max_value) value = max_value;

    lv_chart_set_next_value(chart, series, value);
}


static void update_btop_gauges(int cpu_temp, int board_temp, int fan_rpm)
{
#if ENABLE_BTOP_GAUGES

    // Temperatura CPU
    lv_arc_set_value(objects.arc_cpu_temp, cpu_temp);
    lv_label_set_text_fmt(objects.lbl_btop_cpu_temp, "%d °C", cpu_temp);

    // Temperatura scheda
    lv_arc_set_value(objects.arc_temp1, board_temp);
    lv_label_set_text_fmt(objects.lbl_btop_temp1, "%d °C", board_temp);

    // Ventola
    lv_arc_set_value(objects.arc_fan1, fan_rpm);
    lv_label_set_text_fmt(objects.lbl_btop_fan1, "%d", fan_rpm);

    // Storici
    push_chart_value(objects.chart_fan_rpm, fan_rpm_series,    fan_rpm,    FAN_MAX_RPM);
    push_chart_value(objects.chart_cpu,     cpu_temp_series,   cpu_temp,   TEMP_MAX_C);
    push_chart_value(objects.chart_board,   board_temp_series, board_temp, TEMP_MAX_C);

#else
    (void)cpu_temp;
    (void)board_temp;
    (void)fan_rpm;
#endif
}


// ==================================================
// 9. RETI WIFI SALVATE
// ==================================================

static void save_wifi_network(const char *ssid, const char *password)
{
    if (!wifiPrefs.begin(WIFI_NAMESPACE, false)) {
        return;
    }

    int count = wifiPrefs.getInt("count", 0);
    int slot = -1;

    // Cerca se la rete esiste gia'
    for (int i = 0; i < count; i++) {

        const String key = "ssid" + String(i);

        if (wifiPrefs.getString(key.c_str(), "") == ssid) {
            slot = i;
            break;
        }
    }

    // Altrimenti usa un nuovo slot, se disponibile
    if (slot == -1 && count < MAX_SAVED_NETWORKS) {

        slot = count;
        count++;

        wifiPrefs.putInt("count", count);
    }

    if (slot >= 0) {

        const String ssidKey = "ssid" + String(slot);
        const String passKey = "pass" + String(slot);

        wifiPrefs.putString(ssidKey.c_str(), ssid);
        wifiPrefs.putString(passKey.c_str(), password);
    }

    wifiPrefs.end();
}


// Cerca tra le reti scansionate quella salvata col segnale migliore
static bool find_saved_network(int scan_count, String &out_ssid, String &out_pass)
{
    if (!wifiPrefs.begin(WIFI_NAMESPACE, true)) {
        return false;
    }

    const int saved = wifiPrefs.getInt("count", 0);

    int best_rssi = -1000;
    bool found = false;

    for (int i = 0; i < scan_count; i++) {

        const String scanned = WiFi.SSID(i);
        const int rssi = WiFi.RSSI(i);

        if (scanned.length() == 0 || rssi <= best_rssi) {
            continue;
        }

        for (int j = 0; j < saved; j++) {

            const String ssidKey = "ssid" + String(j);
            const String passKey = "pass" + String(j);

            if (wifiPrefs.getString(ssidKey.c_str(), "") == scanned) {

                out_ssid = scanned;
                out_pass = wifiPrefs.getString(passKey.c_str(), "");

                best_rssi = rssi;
                found = true;

                break;
            }
        }
    }

    wifiPrefs.end();

    return found;
}


// Avvia la connessione automatica a una rete salvata
static void start_auto_connect(const String &ssid, const String &pass)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_label_set_text_fmt(status_label, "Rete salvata: %s\nConnessione...", ssid.c_str());
    lv_label_set_text(ip_label, "");

    esp_lv_adapter_unlock();

    pending_wifi_ssid = ssid;
    pending_wifi_password = pass;

    WiFi.begin(ssid.c_str(), pass.c_str());

    connecting = true;
    auto_dashboard_pending = true;

    connection_started_at = millis();
    last_wifi_check = millis();
}


// ==================================================
// 10. IMPOSTAZIONI (SALVATAGGIO E CARICAMENTO)
// ==================================================

static void load_settings()
{
    // Lettura/scrittura: crea il namespace al primo avvio
    if (!settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        return;
    }

    poll_interval_ms = clamp_ul(
        settingsPrefs.getULong("poll_ms", DEFAULT_POLL_INTERVAL_MS),
        POLL_MIN_MS,
        POLL_MAX_MS
    );

    weather_interval_ms = clamp_ul(
        settingsPrefs.getULong("wx_ms", DEFAULT_WEATHER_INTERVAL_MS),
        WEATHER_MIN_MS,
        WEATHER_MAX_MS
    );

    settingsPrefs.end();
}


static void save_settings()
{
    if (!settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        return;
    }

    settingsPrefs.putULong("poll_ms", (unsigned long)poll_interval_ms);
    settingsPrefs.putULong("wx_ms", (unsigned long)weather_interval_ms);

    settingsPrefs.end();
}


// ==================================================
// 10b. WEB SERVER - UPLOAD GIF
// ==================================================

// Stile comune a tutte le pagine web
static const char WEB_STYLE[] = R"rawliteral(
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{margin:0;min-height:100vh;display:flex;flex-direction:column;align-items:center;
justify-content:center;gap:16px;background:#0b0f14;color:#e6edf3;font-family:system-ui,sans-serif}
h1{margin:0 0 8px;font-size:28px;letter-spacing:4px;color:#00e676}
form{display:flex;flex-direction:column;align-items:center;gap:16px}
a.btn,input[type=submit]{display:block;box-sizing:border-box;width:280px;padding:18px;
border:2px solid #00e676;border-radius:12px;background:transparent;color:#00e676;
font-size:20px;text-align:center;text-decoration:none;cursor:pointer}
a.btn:hover,input[type=submit]:hover{background:#00e676;color:#0b0f14}
input[type=file]{color:#e6edf3;max-width:280px}
a.back{color:#8b949e;font-size:14px;text-decoration:none}
</style>
)rawliteral";


// Costruisce una pagina completa attorno al contenuto
static String web_page(const String &body)
{
    String page = F("<!doctype html><html><head><meta charset='utf-8'><title>KERBEROS</title>");

    page += WEB_STYLE;
    page += F("</head><body>");
    page += body;
    page += F("</body></html>");

    return page;
}


// Pagina principale: due pulsanti, Upload GIF e Screenshot
static void web_handle_root()
{
    if (!web.authenticate(WEB_USER, WEB_PASS)) {
        web.requestAuthentication();
        return;
    }

    web.send(
        200,
        "text/html",
        web_page(
            "<h1>KERBEROS</h1>"
            "<a class='btn' href='/upload'>Upload GIF</a>"
            "<a class='btn' href='/screenshot'>Screenshot</a>"
        )
    );
}


// Pagina con il modulo di caricamento della GIF
static void web_handle_upload_page()
{
    if (!web.authenticate(WEB_USER, WEB_PASS)) {
        web.requestAuthentication();
        return;
    }

    web.send(
        200,
        "text/html",
        web_page(
            "<h1>UPLOAD GIF</h1>"
            "<form method='POST' action='/upload' enctype='multipart/form-data'>"
            "<input type='file' name='f' accept='.gif'>"
            "<input type='submit' value='Carica'>"
            "</form>"
            "<a class='back' href='/'>&larr; Indietro</a>"
        )
    );
}


// Risposta dopo il caricamento
static void web_upload_done()
{
    if (!web.authenticate(WEB_USER, WEB_PASS)) {
        web.requestAuthentication();
        return;
    }

    web.send(
        200,
        "text/html",
        web_page(
            "<h1>FATTO</h1>"
            "<p>GIF caricata. Riavvia KERBEROS per vederla.</p>"
            "<a class='btn' href='/'>Home</a>"
        )
    );
}


// Riceve il file a blocchi e lo scrive sulla SD.
// Scrive in un file temporaneo e lo rinomina a upload completato:
// se il trasferimento si interrompe, la GIF precedente resta intatta.
static void web_handle_upload()
{
    if (!web.authenticate(WEB_USER, WEB_PASS)) {
        return;
    }

    HTTPUpload &up = web.upload();

    if (up.status == UPLOAD_FILE_START) {

        SD.mkdir("/images");
        SD.remove(GIF_TMP_PATH);

        upload_file = SD.open(GIF_TMP_PATH, FILE_WRITE);

    } else if (up.status == UPLOAD_FILE_WRITE) {

        if (upload_file) {
            upload_file.write(up.buf, up.currentSize);
        }

    } else if (up.status == UPLOAD_FILE_END) {

        if (upload_file) {

            upload_file.close();

            SD.remove(GIF_PATH);
            SD.rename(GIF_TMP_PATH, GIF_PATH);
        }

    } else if (up.status == UPLOAD_FILE_ABORTED) {

        if (upload_file) {
            upload_file.close();
        }

        SD.remove(GIF_TMP_PATH);
    }
}


// Screenshot del display, scaricabile dal browser: http://IP-ESP/screenshot
// Il framebuffer RGB565 viene convertito al volo in un BMP a 24 bit
// (altezza negativa = righe dall'alto verso il basso).
static void web_handle_screenshot()
{
    if (!web.authenticate(WEB_USER, WEB_PASS)) {
        web.requestAuthentication();
        return;
    }

    LCD *lcd = (board != nullptr) ? board->getLCD() : nullptr;

    if (lcd == nullptr) {
        web.send(500, "text/plain", "LCD non disponibile");
        return;
    }

    const uint32_t width = lcd->getFrameWidth();
    const uint32_t height = lcd->getFrameHeight();
    const size_t frameSize = (size_t)width * height * 2;

    auto *snapshot = static_cast<uint8_t *>(
        heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );

    if (snapshot == nullptr) {
        web.send(500, "text/plain", "Memoria insufficiente");
        return;
    }

    // Copia del framebuffer sotto lock LVGL
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(snapshot);
        web.send(500, "text/plain", "LVGL occupato");
        return;
    }

    auto *frame = static_cast<uint8_t *>(esp_lv_adapter_get_active_frame_buffer());

    if (frame == nullptr) {
        esp_lv_adapter_unlock();
        heap_caps_free(snapshot);
        web.send(500, "text/plain", "Framebuffer non disponibile");
        return;
    }

    memcpy(snapshot, frame, frameSize);

    esp_lv_adapter_unlock();

    // Header BMP (14 byte file + 40 byte info), 24 bit per pixel
    const uint32_t rowSize = (width * 3 + 3) & ~3U;
    const uint32_t imageSize = rowSize * height;
    const uint32_t fileSize = 54 + imageSize;
    const uint32_t dataOffset = 54;
    const uint32_t infoSize = 40;
    const int32_t bmpHeight = -(int32_t)height;

    uint8_t header[54] = {0};

    header[0] = 'B';
    header[1] = 'M';

    memcpy(header + 2, &fileSize, 4);
    memcpy(header + 10, &dataOffset, 4);
    memcpy(header + 14, &infoSize, 4);
    memcpy(header + 18, &width, 4);
    memcpy(header + 22, &bmpHeight, 4);

    header[26] = 1;    // piani
    header[28] = 24;   // bit per pixel

    memcpy(header + 34, &imageSize, 4);

    // Buffer di invio con piu' righe per volta: meno scritture TCP, piu' velocita'
    const uint32_t rowsPerChunk = 16;

    auto *row = static_cast<uint8_t *>(
        heap_caps_malloc(rowSize * rowsPerChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );

    if (row == nullptr) {
        heap_caps_free(snapshot);
        web.send(500, "text/plain", "Memoria insufficiente");
        return;
    }

    // Invio: lunghezza nota, corpo scritto direttamente sul client
    web.setContentLength(fileSize);
    web.sendHeader("Content-Disposition", "inline; filename=\"kerberos.bmp\"");
    web.send(200, "image/bmp", "");

    auto &client = web.client();

    // Niente attesa di Nagle: i dati partono subito
    client.setNoDelay(true);

    client.write(header, sizeof(header));

    for (uint32_t y = 0; y < height; y += rowsPerChunk) {

        const uint32_t rows = min(rowsPerChunk, height - y);

        for (uint32_t r = 0; r < rows; r++) {

            const uint16_t *src =
                reinterpret_cast<const uint16_t *>(snapshot) + (size_t)(y + r) * width;

            uint8_t *dst = row + (size_t)r * rowSize;

            for (uint32_t x = 0; x < width; x++) {

                const uint16_t p = src[x];

                const uint8_t r5 = (p >> 11) & 0x1F;
                const uint8_t g6 = (p >> 5) & 0x3F;
                const uint8_t b5 = p & 0x1F;

                // BMP e' in ordine B, G, R
                dst[x * 3]     = (b5 << 3) | (b5 >> 2);
                dst[x * 3 + 1] = (g6 << 2) | (g6 >> 4);
                dst[x * 3 + 2] = (r5 << 3) | (r5 >> 2);
            }

            // Padding a multiplo di 4 byte (vuoto con larghezza 1024)
            for (uint32_t k = width * 3; k < rowSize; k++) {
                dst[k] = 0;
            }
        }

        const size_t bytes = (size_t)rows * rowSize;

        if (client.write(row, bytes) != bytes) {
            break;   // il browser ha chiuso la connessione
        }
    }

    heap_caps_free(row);
    heap_caps_free(snapshot);
}


static void start_web_server()
{
    web.on("/", HTTP_GET, web_handle_root);
    web.on("/screenshot", HTTP_GET, web_handle_screenshot);
    web.on("/upload", HTTP_GET, web_handle_upload_page);
    web.on("/upload", HTTP_POST, web_upload_done, web_handle_upload);
    web.begin();

    web_started = true;
}


// ==================================================
// 11. SERVER PYTHON
// ==================================================

static void get_server_status()
{
    if (WiFi.status() != WL_CONNECTED) {
        server_fail();
        return;
    }

    if (!server_client_ready) {

        // TEMPORANEO: non verifica il certificato SSL
        server_client.setInsecure();

        server_client_ready = true;
    }

    HTTPClient http;

    http.setReuse(true);
    http.setConnectTimeout(3000);
    http.setTimeout(4000);

    if (!http.begin(server_client, SERVER_URL)) {
        server_fail();
        return;
    }

    if (http.GET() != HTTP_CODE_OK) {
        http.end();
        server_fail();
        return;
    }

    const String payload = http.getString();

    http.end();

    JsonDocument doc;

    if (deserializeJson(doc, payload)) {
        show_server_offline();
        return;
    }

    // --------------------------------------------------
    // LETTURA JSON
    // --------------------------------------------------

    const char *hostname = doc["hostname"] | "Server";

    const float cpu = doc["cpu"] | 0.0f;

    const uint64_t ram_total  = doc["ram_total"]  | 0ULL;
    const uint64_t ram_used   = doc["ram_used"]   | 0ULL;
    const uint64_t disk_total = doc["disk_total"] | 0ULL;
    const uint64_t disk_used  = doc["disk_used"]  | 0ULL;
    const uint64_t uptime     = doc["uptime"]     | 0ULL;

    const float cpu_temp = doc["cpu_temp"] | 0.0f;
    const float temp1    = doc["temp1"]    | 0.0f;
    const float fan1     = doc["fan1"]     | 0.0f;

    // Security
    JsonObject security = doc["security"].as<JsonObject>();

    const char *security_level = security["level"] | "UNKNOWN";

    const int ssh_failed    = security["ssh_failed"]    | -1;
    const int banned_ips    = security["banned_ips"]    | -1;
    const int recidive_bans = security["recidive_bans"] | -1;

    const bool firewall_active = security["firewall_active"] | false;
    const bool fail2ban_active = security["fail2ban_active"] | false;

    // --------------------------------------------------
    // CALCOLI
    // --------------------------------------------------

    const int cpu_percent = (int)(cpu + 0.5f);

    const int ram_percent = ram_total
        ? (int)((100ULL * ram_used + ram_total / 2) / ram_total)
        : 0;

    const int disk_percent = disk_total
        ? (int)((100ULL * disk_used + disk_total / 2) / disk_total)
        : 0;

    unsigned long ram_used_w,  ram_used_d;
    unsigned long ram_total_w, ram_total_d;
    unsigned long disk_used_w,  disk_used_d;
    unsigned long disk_total_w, disk_total_d;

    format_gb(ram_used,   &ram_used_w,   &ram_used_d);
    format_gb(ram_total,  &ram_total_w,  &ram_total_d);
    format_gb(disk_used,  &disk_used_w,  &disk_used_d);
    format_gb(disk_total, &disk_total_w, &disk_total_d);

    const unsigned long days    = uptime / 86400;
    const unsigned long hours   = (uptime % 86400) / 3600;
    const unsigned long minutes = (uptime % 3600) / 60;

    const int cpu_temp_i = (int)(cpu_temp + 0.5f);
    const int temp1_i    = (int)(temp1 + 0.5f);
    const int fan1_i     = (int)(fan1 + 0.5f);

    const bool protection_ok = firewall_active && fail2ban_active;

    lv_color_t security_color = lv_color_hex(0x00FF88);

    if (strcmp(security_level, "WARNING") == 0) {
        security_color = lv_color_hex(0xFFAA00);
    } else if (strcmp(security_level, "CRITICAL") == 0) {
        security_color = lv_color_hex(0xFF3355);
    } else if (strcmp(security_level, "UNKNOWN") == 0) {
        security_color = lv_color_hex(0x888888);
    }

    // --------------------------------------------------
    // AGGIORNAMENTO DISPLAY
    // --------------------------------------------------

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    // Hostname
    lv_label_set_text(objects.lbl_hostname, hostname);

    // CPU
    lv_label_set_text_fmt(objects.lbl_cpu, "CPU: %d%%", cpu_percent);
    lv_bar_set_value(objects.bar_cpu, cpu_percent, LV_ANIM_OFF);

    // RAM
    lv_label_set_text_fmt(
        objects.lbl_ram,
        "RAM: %lu.%lu / %lu.%lu GB (%d%%)",
        ram_used_w, ram_used_d, ram_total_w, ram_total_d, ram_percent
    );
    lv_bar_set_value(objects.bar_ram, ram_percent, LV_ANIM_OFF);

    // Contatori BTOP e grafici
    update_btop_gauges(cpu_temp_i, temp1_i, fan1_i);

    // Disco
    lv_label_set_text_fmt(
        objects.lbl_disk,
        "DISK: %lu.%lu / %lu.%lu GB (%d%%)",
        disk_used_w, disk_used_d, disk_total_w, disk_total_d, disk_percent
    );
    lv_bar_set_value(objects.bar_disk, disk_percent, LV_ANIM_OFF);

    // Uptime
    lv_label_set_text_fmt(objects.lbl_uptime, "%lu giorni %lu ore %lu min", days, hours, minutes);

    // Security
    lv_label_set_text(objects.lbl_security_status, security_level);
    lv_obj_set_style_text_color(objects.lbl_security_status, security_color, LV_PART_MAIN);

    lv_label_set_text_fmt(objects.lbl_sec_ssh_failed, "%d", ssh_failed);
    lv_label_set_text_fmt(objects.lbl_sec_banned, "%d", banned_ips);
    lv_label_set_text_fmt(objects.lbl_sec_recidive, "%d", recidive_bans);

    lv_label_set_text(objects.lbl_sec_firewall, protection_ok ? "ACTIVE" : "INACTIVE");
    lv_obj_set_style_text_color(
        objects.lbl_sec_firewall,
        lv_color_hex(protection_ok ? 0x00FF88 : 0xFF3355),
        LV_PART_MAIN
    );

    // Stato server
    apply_server_state(true);

    esp_lv_adapter_unlock();
}


// ==================================================
// 12. METEO - DESCRIZIONE CODICI WMO
// ==================================================

static const char *weather_description(int code)
{
    switch (code) {

        case 0:  return "Sereno";
        case 1:  return "Prevalentemente sereno";
        case 2:  return "Diarrea in cielo";
        case 3:  return "Nuvole del cazzo!";

        case 45:
        case 48: return "Nebbia";

        case 51:
        case 53:
        case 55: return "Bagnetta di merda";

        case 56:
        case 57: return "Pioviggine gelata";

        case 61:
        case 63:
        case 65: return "Piove merda!";

        case 66:
        case 67: return "Piove merda fredda";

        case 71:
        case 73:
        case 75: return "Cazzo di freddo, neve!";

        case 77: return "Diarrea di neve";

        case 80:
        case 81:
        case 82: return "Piove merda!";

        case 85:
        case 86: return "Rovesci di merda dura";

        case 95: return "Bestemmioni tempestosi";

        case 96:
        case 99: return "Temporale con grandine";

        default: return "Non si capisce un cazzo";
    }
}


// ==================================================
// 13. METEO - ICONA PNG
// ==================================================

static void update_weather_icon(int code)
{
    const lv_image_dsc_t *icon = &img_cloud;

    switch (code) {

        case 0:
            icon = &img_sun;
            break;

        case 51: case 53: case 55:
        case 56: case 57:
        case 61: case 63: case 65:
        case 66: case 67:
        case 80: case 81: case 82:
            icon = &img_rain;
            break;

        case 71: case 73: case 75: case 77:
        case 85: case 86:
            icon = &img_snow;
            break;

        case 95: case 96: case 99:
            icon = &img_storm;
            break;

        default:
            // 1, 2, 3, 45, 48 e codici sconosciuti
            break;
    }

    lv_image_set_src(objects.png_weather, icon);
}


// ==================================================
// 14. METEO - RECUPERO DATI OPEN-METEO
// ==================================================

// Formatta un valore x10 con segno e un decimale: 215 -> "21.5"
static void format_tenths(char *out, size_t size, int value10)
{
    snprintf(out, size, "%s%d.%d", value10 < 0 ? "-" : "", abs(value10) / 10, abs(value10) % 10);
}


static void get_weather_data()
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    // Libera la sessione TLS del server Python
    server_client.stop();

    WiFiClientSecure client;

    client.setInsecure();

    HTTPClient http;

    http.setConnectTimeout(3000);
    http.setTimeout(5000);

    if (!http.begin(client, WEATHER_URL)) {
        return;
    }

    if (http.GET() != HTTP_CODE_OK) {
        http.end();
        return;
    }

    const String payload = http.getString();

    http.end();

    JsonDocument doc;

    if (deserializeJson(doc, payload)) {
        return;
    }

    JsonObjectConst current = doc["current"];

    if (current.isNull()) {
        return;
    }

    // --------------------------------------------------
    // LETTURA DATI
    // --------------------------------------------------

    const float temperature = current["temperature_2m"]       | 0.0f;
    const float feels_like  = current["apparent_temperature"] | 0.0f;
    const int   humidity    = current["relative_humidity_2m"] | 0;
    const float wind        = current["wind_speed_10m"]       | 0.0f;
    const float pressure    = current["surface_pressure"]     | 0.0f;
    const float rain        = current["precipitation"]        | 0.0f;
    const int   code        = current["weather_code"]         | -1;

    // --------------------------------------------------
    // FORMATTAZIONE TESTI
    // --------------------------------------------------

    char temp_text[16];
    char feels_text[16];
    char wind_text[16];
    char rain_text[16];

    format_tenths(temp_text,  sizeof(temp_text),  (int)lroundf(temperature * 10.0f));
    format_tenths(feels_text, sizeof(feels_text), (int)lroundf(feels_like * 10.0f));
    format_tenths(wind_text,  sizeof(wind_text),  (int)lroundf(wind * 10.0f));
    format_tenths(rain_text,  sizeof(rain_text),  (int)lroundf(rain * 10.0f));

    const int pressure_i = (int)lroundf(pressure);

    // --------------------------------------------------
    // AGGIORNAMENTO INTERFACCIA
    // --------------------------------------------------

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_label_set_text_fmt(objects.label_temperature, "%s°", temp_text);
    lv_label_set_text(objects.label_condition, weather_description(code));
    update_weather_icon(code);

    lv_label_set_text_fmt(objects.label_feels_like, "Percepita %s°", feels_text);
    lv_label_set_text_fmt(objects.label_humidity_value, "%d%%", humidity);
    lv_label_set_text_fmt(objects.label_wind_value, "%s km/h", wind_text);
    lv_label_set_text_fmt(objects.label_pressure_value, "%d hPa", pressure_i);
    lv_label_set_text_fmt(objects.label_rain_value, "%s mm", rain_text);

    esp_lv_adapter_unlock();
}


// ==================================================
// 15. TASTIERA VIRTUALE
// ==================================================

static void password_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {

        lv_keyboard_set_textarea(keyboard, password_area);
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}


static void keyboard_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}


static void create_keyboard()
{
    keyboard = lv_keyboard_create(objects.wifi_screen);

    lv_obj_set_size(keyboard, 900, 300);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_CANCEL, nullptr);

    lv_obj_add_event_cb(password_area, password_event, LV_EVENT_FOCUSED, nullptr);
}


// ==================================================
// 16. ETICHETTA INDIRIZZO IP
// ==================================================

static void create_ip_label()
{
    ip_label = lv_label_create(objects.wifi_screen);

    lv_obj_set_pos(ip_label, IP_LABEL_X, IP_LABEL_Y);
    lv_obj_set_style_text_color(ip_label, lv_color_hex(0x00FF99), LV_PART_MAIN);
    lv_obj_set_style_text_font(ip_label, &lv_font_montserrat_20, LV_PART_MAIN);

    lv_label_set_text(ip_label, "");
}


// ==================================================
// 17. PAGINA IMPOSTAZIONI
// ==================================================

enum SettingsAction {
    ACT_OPEN,
    ACT_POLL_DEC,
    ACT_POLL_INC,
    ACT_WEATHER_DEC,
    ACT_WEATHER_INC,
    ACT_RESET,
    ACT_BACK
};


static void update_settings_labels()
{
    lv_label_set_text_fmt(objects.lbl_poll_value, "%lu s", (unsigned long)(poll_interval_ms / 1000UL));
    lv_label_set_text_fmt(objects.lbl_weather_value, "%lu min", (unsigned long)(weather_interval_ms / 60000UL));
}


static void settings_button_event(lv_event_t *e)
{
    const SettingsAction action = (SettingsAction)(intptr_t)lv_event_get_user_data(e);

    switch (action) {

        // Apre la pagina impostazioni
        case ACT_OPEN:
            update_settings_labels();
            loadScreen(SCREEN_ID_SETTINGS_SCREEN);
            break;

        // Intervallo server
        case ACT_POLL_DEC:
            poll_interval_ms = step_value(poll_interval_ms, -(long)POLL_STEP_MS, POLL_MIN_MS, POLL_MAX_MS);
            update_settings_labels();
            break;

        case ACT_POLL_INC:
            poll_interval_ms = step_value(poll_interval_ms, (long)POLL_STEP_MS, POLL_MIN_MS, POLL_MAX_MS);
            update_settings_labels();
            break;

        // Intervallo meteo
        case ACT_WEATHER_DEC:
            weather_interval_ms = step_value(weather_interval_ms, -(long)WEATHER_STEP_MS, WEATHER_MIN_MS, WEATHER_MAX_MS);
            update_settings_labels();
            break;

        case ACT_WEATHER_INC:
            weather_interval_ms = step_value(weather_interval_ms, (long)WEATHER_STEP_MS, WEATHER_MIN_MS, WEATHER_MAX_MS);
            update_settings_labels();
            break;

        // Valori di default
        case ACT_RESET:
            poll_interval_ms = DEFAULT_POLL_INTERVAL_MS;
            weather_interval_ms = DEFAULT_WEATHER_INTERVAL_MS;
            update_settings_labels();
            break;

        // Salva e torna alla schermata WiFi
        case ACT_BACK:
            save_settings();
            loadScreen(SCREEN_ID_WIFI_SCREEN);
            break;
    }
}


// Collega un pulsante EEZ a un'azione.
// repeat = true: tenendo premuto il valore continua a cambiare (+ e -)
static void bind_settings_button(lv_obj_t *btn, int action, bool repeat)
{
    void *user_data = (void *)(intptr_t)action;

    if (repeat) {
        lv_obj_add_event_cb(btn, settings_button_event, LV_EVENT_SHORT_CLICKED, user_data);
        lv_obj_add_event_cb(btn, settings_button_event, LV_EVENT_LONG_PRESSED_REPEAT, user_data);
    } else {
        lv_obj_add_event_cb(btn, settings_button_event, LV_EVENT_CLICKED, user_data);
    }
}


static void init_settings_page()
{
    // Pulsante nella schermata WiFi
    bind_settings_button(objects.btn_settings, ACT_OPEN, false);

    // Server
    bind_settings_button(objects.btn_poll_dec, ACT_POLL_DEC, true);
    bind_settings_button(objects.btn_poll_inc, ACT_POLL_INC, true);

    // Meteo
    bind_settings_button(objects.btn_weather_dec, ACT_WEATHER_DEC, true);
    bind_settings_button(objects.btn_weather_inc, ACT_WEATHER_INC, true);

    // Ripristina e salva
    bind_settings_button(objects.btn_settings_reset, ACT_RESET, false);
    bind_settings_button(objects.btn_settings_back, ACT_BACK, false);

    update_settings_labels();
}


// ==================================================
// 18. CONTO ALLA ROVESCIA METEO
// ==================================================

// Chiamata ogni secondo dal timer LVGL
static void weather_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!weather_screen_active) {
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(objects.label_timer_weather, "Nessuna connessione");
        return;
    }

    if (weather_fetching || weather_refresh_now) {
        lv_label_set_text(objects.label_timer_weather, "Aggiornamento in corso...");
        return;
    }

    const unsigned long interval = weather_interval_ms;
    const unsigned long elapsed = millis() - last_weather_update;

    unsigned long remaining_s = 0;

    if (elapsed < interval) {
        remaining_s = (interval - elapsed + 999UL) / 1000UL;
    }

    lv_label_set_text_fmt(
        objects.label_timer_weather,
        "Prossimo aggiornamento tra %02lu:%02lu",
        remaining_s / 60UL,
        remaining_s % 60UL
    );
}


// ==================================================
// 18b. INDICATORE POTENZA WIFI (ANGOLO IN ALTO A SINISTRA)
// ==================================================

// Posizione: 2 px dai bordi
static const int WIFI_ICON_X = 2;
static const int WIFI_ICON_Y = 2;

// Aspetto
#define WIFI_SHOW_DBM 1                       // 1 = mostra anche il valore in dBm
static const int WIFI_BARS = 4;
static const int WIFI_BAR_W = 5;
static const int WIFI_BAR_GAP = 2;
static const int WIFI_BAR_MIN_H = 6;          // altezza della barra piu' bassa
static const int WIFI_BAR_STEP_H = 4;         // incremento per ogni barra
static const int WIFI_BOX_H = 24;
static const uint32_t WIFI_COLOR_OFF = 0x444444;

// Intervallo di aggiornamento
static const uint32_t WIFI_UPDATE_MS = 2000;

static lv_obj_t *wifi_bars[WIFI_BARS];
static lv_obj_t *wifi_dbm_label = nullptr;


// Converte l'RSSI in numero di barre accese (1..4)
static int wifi_level_from_rssi(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;

    return 1;
}


static void wifi_indicator_update_cb(lv_timer_t *timer)
{
    (void)timer;

    int level = 0;

    if (WiFi.status() == WL_CONNECTED) {

        const int rssi = WiFi.RSSI();

        level = wifi_level_from_rssi(rssi);

#if WIFI_SHOW_DBM
        lv_label_set_text_fmt(wifi_dbm_label, "%d dBm", rssi);
#endif

    } else {

#if WIFI_SHOW_DBM
        lv_label_set_text(wifi_dbm_label, "--");
#endif
    }

    // Colore in base alla qualita': 3-4 barre verde, 2 arancione, 1 rosso
    uint32_t color = COLOR_BAD;

    if (level >= 3) {
        color = COLOR_OK;
    } else if (level == 2) {
        color = COLOR_WARN;
    }

    for (int i = 0; i < WIFI_BARS; i++) {

        lv_obj_set_style_bg_color(
            wifi_bars[i],
            lv_color_hex(i < level ? color : WIFI_COLOR_OFF),
            LV_PART_MAIN
        );
    }
}


static void create_wifi_indicator()
{
    // Layer superiore: resta visibile su tutte le schermate
    lv_obj_t *box = lv_obj_create(lv_layer_top());

    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, WIFI_ICON_X, WIFI_ICON_Y);
    lv_obj_set_size(box, 140, WIFI_BOX_H);

    // Non intercetta il tocco
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Barre
    const int base_y = WIFI_BOX_H - 3;

    for (int i = 0; i < WIFI_BARS; i++) {

        const int h = WIFI_BAR_MIN_H + i * WIFI_BAR_STEP_H;

        lv_obj_t *bar = lv_obj_create(box);

        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, WIFI_BAR_W, h);
        lv_obj_set_pos(bar, i * (WIFI_BAR_W + WIFI_BAR_GAP), base_y - h);

        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(WIFI_COLOR_OFF), LV_PART_MAIN);

        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

        wifi_bars[i] = bar;
    }

    // Valore in dBm
    wifi_dbm_label = lv_label_create(box);

    lv_obj_set_pos(wifi_dbm_label, WIFI_BARS * (WIFI_BAR_W + WIFI_BAR_GAP) + 4, 0);
    lv_obj_set_style_text_font(wifi_dbm_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_dbm_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    lv_label_set_text(wifi_dbm_label, "");

#if !WIFI_SHOW_DBM
    lv_obj_add_flag(wifi_dbm_label, LV_OBJ_FLAG_HIDDEN);
#endif

    // Primo aggiornamento e timer periodico
    wifi_indicator_update_cb(nullptr);

    lv_timer_create(wifi_indicator_update_cb, WIFI_UPDATE_MS, nullptr);
}


// ==================================================
// 18c. DIAGNOSTICA ESP32 (ANGOLO IN ALTO A DESTRA)
// ==================================================

static const uint32_t DIAG_UPDATE_MS = 2000;
static lv_obj_t *diag_label = nullptr;

// Sensore termico interno dell'ESP32-S3
static temperature_sensor_handle_t cpu_temp_sensor = nullptr;
static bool cpu_temp_available = false;

static void init_esp32_temperature()
{
    // Il sensore interno misura la temperatura del die, non quella ambiente.
    temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 50);

    if (temperature_sensor_install(&config, &cpu_temp_sensor) != ESP_OK) {
        Serial.println("[TEMP] Installazione sensore interno non riuscita");
        return;
    }

    if (temperature_sensor_enable(cpu_temp_sensor) != ESP_OK) {
        Serial.println("[TEMP] Attivazione sensore interno non riuscita");
        temperature_sensor_uninstall(cpu_temp_sensor);
        cpu_temp_sensor = nullptr;
        return;
    }

    cpu_temp_available = true;
    Serial.println("[TEMP] Sensore interno ESP32-S3 attivo");
}

static void diagnostic_indicator_update_cb(lv_timer_t *timer)
{
    (void)timer;

    if (diag_label == nullptr) return;

    const uint32_t free_ram_kb = ESP.getFreeHeap() / 1024UL;
    const uint32_t free_psram_mb = ESP.getFreePsram() / (1024UL * 1024UL);
    const uint32_t flash_mb = ESP.getFlashChipSize() / (1024UL * 1024UL);
    const uint32_t cpu_mhz = ESP.getCpuFreqMHz();
    const uint32_t chip_rev = ESP.getChipRevision();

    float cpu_temperature = 0.0f;
    const bool temperature_ok = cpu_temp_available &&
        temperature_sensor_get_celsius(cpu_temp_sensor, &cpu_temperature) == ESP_OK;

    char temperature_text[12];
    if (temperature_ok) {
        snprintf(temperature_text, sizeof(temperature_text), "%.1fC", cpu_temperature);
    } else {
        snprintf(temperature_text, sizeof(temperature_text), "--");
    }

    const uint32_t uptime = millis() / 1000UL;
    char uptime_text[16];
    snprintf(
        uptime_text, sizeof(uptime_text),
        "%02lu:%02lu:%02lu",
        (unsigned long)(uptime / 3600UL),
        (unsigned long)((uptime % 3600UL) / 60UL),
        (unsigned long)(uptime % 60UL)
    );

    lv_label_set_text_fmt(
        diag_label,
        "CPU %luM  RAM %luK  PS %luM  FL %luM  R%lu  UP %s  T %s",
        (unsigned long)cpu_mhz,
        (unsigned long)free_ram_kb,
        (unsigned long)free_psram_mb,
        (unsigned long)flash_mb,
        (unsigned long)chip_rev,
        uptime_text,
        temperature_text
    );
}

static void create_diagnostic_indicator()
{
    // Layer superiore: resta visibile su tutte le schermate.
    // 10 px dal bordo superiore e 10 px dal bordo destro.
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 650, 26);
    lv_obj_align(box, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    diag_label = lv_label_create(box);
    lv_obj_set_width(diag_label, LV_PCT(100));
    lv_obj_set_pos(diag_label, 0, 0);
    lv_obj_set_style_text_font(diag_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(diag_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(diag_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(diag_label, LV_LABEL_LONG_CLIP);

    diagnostic_indicator_update_cb(nullptr);
    lv_timer_create(diagnostic_indicator_update_cb, DIAG_UPDATE_MS, nullptr);
}


// ==================================================
// 19. EVENTI WIFI
// ==================================================

static void scan_event(lv_event_t *e)
{
    (void)e;

    if (scanning || connecting) {
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    set_status("Ricerca reti Wi-Fi...");

    lv_dropdown_set_options(wifi_dropdown, "Ricerca in corso...");
    lv_obj_add_state(scan_button, LV_STATE_DISABLED);

    network_count = 0;
    scanning = true;

    WiFi.scanDelete();
    WiFi.scanNetworks(true);
}


static void connect_event(lv_event_t *e)
{
    (void)e;

    if (connecting || scanning) {
        return;
    }

    if (network_count == 0) {
        set_status("Prima cerca una rete Wi-Fi");
        return;
    }

    char ssid[128] = {0};

    lv_dropdown_get_selected_str(wifi_dropdown, ssid, sizeof(ssid));

    if (strlen(ssid) == 0) {
        set_status("Seleziona una rete Wi-Fi");
        return;
    }

    const String password = lv_textarea_get_text(password_area);

    pending_wifi_ssid = ssid;
    pending_wifi_password = password;

    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    set_status("Connessione in corso...");
    lv_label_set_text(ip_label, "");

    WiFi.begin(ssid, password.c_str());

    connecting = true;
    connection_started_at = millis();
    last_wifi_check = millis();
}


// Precompila la password se la rete selezionata e' salvata
static void wifi_select_event(lv_event_t *e)
{
    (void)e;

    char selected_ssid[128] = {0};

    lv_dropdown_get_selected_str(wifi_dropdown, selected_ssid, sizeof(selected_ssid));

    lv_textarea_set_text(password_area, "");

    if (!wifiPrefs.begin(WIFI_NAMESPACE, true)) {
        return;
    }

    const int count = wifiPrefs.getInt("count", 0);

    for (int i = 0; i < count; i++) {

        const String ssidKey = "ssid" + String(i);
        const String passKey = "pass" + String(i);

        if (wifiPrefs.getString(ssidKey.c_str(), "") == selected_ssid) {

            const String saved_password = wifiPrefs.getString(passKey.c_str(), "");

            lv_textarea_set_text(password_area, saved_password.c_str());

            break;
        }
    }

    wifiPrefs.end();
}


// ==================================================
// 20. EVENTI NAVIGAZIONE
// ==================================================

// Imposta lo stato di polling di server e meteo
static void set_polling(bool server, bool weather)
{
    polling_active = server;
    poll_now = server;

    weather_screen_active = weather;
    weather_refresh_now = weather;
}


static void go_dashboard_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_DASHBOARD_SCREEN);
    set_polling(true, false);
}


static void go_btop_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_BTOP_SCREEN);
    set_polling(true, false);
}


static void go_weather_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_WEATHER_SCREEN);
    set_polling(false, true);
}


static void go_weather_back_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_DASHBOARD_SCREEN);
    set_polling(true, false);
}


static void go_wifi_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_WIFI_SCREEN);
    set_polling(false, false);
}


static void go_tamagochi_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_TAMAGOCHI);
    set_polling(false, false);
}


// Uscita dal tamagochi verso la schermata WiFi
static void go_tamagochi_exit_event(lv_event_t *e)
{
    (void)e;

    loadScreen(SCREEN_ID_WIFI_SCREEN);
    set_polling(false, false);
}


// ==================================================
// 21. OROLOGIO NTP
// ==================================================

// Aggiorna l'ora su tutti i label orologio (dashboard e tamagochi)
static void set_clock_text(const char *text)
{
    lv_label_set_text(objects.label_time, text);
    lv_label_set_text(objects.clock_tamagochi, text);
}


static void set_(uint32_t color)
{
    lv_led_set_color(objects.led_ntp, lv_color_hex(color));
    lv_led_set_brightness(objects.led_ntp, 255);
}


// Chiamata dal client SNTP ogni volta che l'ora viene aggiornata davvero.
static void ntp_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    last_ntp_sync_ms = millis();
    ntp_sync_seen = true;
}


static void update_ntp_led_countdown()
{
    // Prima della prima sincronizzazione il LED resta rosso.
    if (!ntp_sync_seen) {
        set_ntp_led(0xFF0000);
        return;
    }

    uint32_t interval_ms = esp_sntp_get_sync_interval();
    if (interval_ms == 0) {
        interval_ms = 3600000UL; // fallback: intervallo predefinito di un'ora
    }

    const uint32_t elapsed_ms = (uint32_t)(millis() - last_ntp_sync_ms);
    const uint32_t remaining_ms = (elapsed_ms >= interval_ms)
        ? 0UL
        : (interval_ms - elapsed_ms);

    // Verde: piu' di 60 secondi.
    // Arancione: ultimi 60..31 secondi.
    // Rosso: ultimi 30 secondi, compreso lo zero.
    if (remaining_ms <= 30000UL) {
        set_ntp_led(0xFF0000);
    } else if (remaining_ms <= 60000UL) {
        set_ntp_led(0xFF8000);
    } else {
        set_ntp_led(0x00FF00);
    }
}


static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    // Configurazione NTP al primo collegamento
    if (!ntp_configured) {

        // Registriamo il callback prima di avviare SNTP, cosi' non perdiamo
        // la notifica della prima sincronizzazione.
        esp_sntp_set_time_sync_notification_cb(ntp_sync_notification_cb);

        configTzTime(
            "CET-1CEST,M3.5.0/2,M10.5.0/3",
            "pool.ntp.org",
            "time.google.com"
        );

        ntp_configured = true;
    }

    // Lettura ora
    time_t now;

    time(&now);

    struct tm timeinfo;

    localtime_r(&now, &timeinfo);

    const bool synced = timeinfo.tm_year >= (2020 - 1900);

    static int last_state = -1;
    static int last_minute = -1;

    // --------------------------------------------------
    // NON SINCRONIZZATO
    // --------------------------------------------------

    if (!synced) {

        if (last_state != 0) {

            last_state = 0;
            last_minute = -1;

            set_clock_text("--:--");
            lv_label_set_text(objects.label_date, "Sincronizzazione...");

            set_ntp_led(0xFF0000);
        }

        return;
    }

    // --------------------------------------------------
    // SINCRONIZZATO
    // --------------------------------------------------

    if (last_state != 1) {
        last_state = 1;
    }

    // Colore in base al tempo che manca alla prossima sincronizzazione NTP.
    update_ntp_led_countdown();

    // Aggiorna i testi solo quando cambia il minuto
    if (timeinfo.tm_min == last_minute) {
        return;
    }

    last_minute = timeinfo.tm_min;

    static const char *days[] = {
        "Domenica", "Lunedi", "Martedi", "Mercoledi",
        "Giovedi", "Venerdi", "Sabato"
    };

    static const char *months[] = {
        "Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno",
        "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"
    };

    char time_text[8];
    char date_text[48];

    snprintf(time_text, sizeof(time_text), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    snprintf(
        date_text,
        sizeof(date_text),
        "%s %d %s %d",
        days[timeinfo.tm_wday],
        timeinfo.tm_mday,
        months[timeinfo.tm_mon],
        timeinfo.tm_year + 1900
    );

    set_clock_text(time_text);
    lv_label_set_text(objects.label_date, date_text);
}


// ==================================================
// 22. INIZIALIZZAZIONE INTERFACCIA EEZ STUDIO
// ==================================================

static void init_interface()
{
    // Oggetti EEZ
    ui_init();

#if LV_USE_GIF

    // Tamagochi: GIF Kerby
    lv_obj_t *gif_kerby = lv_gif_create(objects.obj_kerby);

    lv_obj_set_size(gif_kerby, 443, 394);
    lv_obj_center(gif_kerby);

    lv_gif_set_color_format(gif_kerby, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(gif_kerby, "S:/images/kenny.gif");  // = GIF_PATH con prefisso LVGL "S:"

#endif

    // Grafici e contatori
    init_charts();
    init_btop_gauges();

    // Eventi WiFi
    lv_obj_add_event_cb(wifi_dropdown, wifi_select_event, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(scan_button, scan_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(connect_button, connect_event, LV_EVENT_CLICKED, nullptr);

    lv_textarea_set_one_line(password_area, true);

    // Navigazione
    lv_obj_add_event_cb(dashboard_button, go_dashboard_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_btop_back, go_dashboard_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btop_button, go_btop_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_wifi, go_wifi_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_weather, go_weather_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_weather_back, go_weather_back_event, LV_EVENT_CLICKED, nullptr);

    // Tamagochi
    lv_obj_add_event_cb(objects.btn_tamagochi, go_tamagochi_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_exit, go_tamagochi_exit_event, LV_EVENT_CLICKED, nullptr);

    // Widget WiFi
    create_keyboard();
    create_ip_label();

    // Pagina impostazioni
    init_settings_page();

    // Indicatori sempre visibili: WiFi a sinistra, diagnostica ESP32 a destra.
    create_wifi_indicator();
    create_diagnostic_indicator();

    // Timer LVGL: conto alla rovescia meteo e orologio
    lv_label_set_text(objects.label_timer_weather, "");

    lv_timer_create(weather_timer_cb, 1000, nullptr);
    lv_timer_create(clock_timer_cb, 1000, nullptr);

    set_status("Pronto. Cerca una rete Wi-Fi");
}


// ==================================================
// 23. POLLING METEO
// ==================================================

static void handle_weather_polling()
{
    if (!weather_screen_active || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (weather_refresh_now ||
        millis() - last_weather_update >= weather_interval_ms) {

        weather_refresh_now = false;
        last_weather_update = millis();

        weather_fetching = true;
        get_weather_data();
        weather_fetching = false;
    }
}


// ==================================================
// 24. POLLING SERVER
// ==================================================

static void handle_server_polling()
{
    // Richiesta immediata
    if (poll_now) {

        poll_now = false;
        last_server_update = millis();

        get_server_status();

        return;
    }

    // Richiesta periodica
    if (polling_active &&
        millis() - last_server_update >= poll_interval_ms) {

        last_server_update = millis();

        get_server_status();
    }
}


// ==================================================
// 25. TASK DI RETE (CORE 0)
// ==================================================

static void net_task(void *param)
{
    (void)param;

    for (;;) {

        // Pausa richiesta per lo screenshot
        if (screenshot_requested) {

            net_task_paused = true;

            while (screenshot_requested) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            net_task_paused = false;
        }

        handle_server_polling();
        handle_weather_polling();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


// ==================================================
// 26. RISULTATI SCANSIONE WIFI
// ==================================================

static void handle_wifi_scan()
{
    if (!scanning) {
        return;
    }

    const int result = WiFi.scanComplete();

    // --------------------------------------------------
    // SCANSIONE COMPLETATA
    // --------------------------------------------------

    if (result >= 0) {

        // Scansione di avvio: prova la connessione automatica
        if (auto_connect_pending) {

            auto_connect_pending = false;

            String saved_ssid;
            String saved_pass;

            if (find_saved_network(result, saved_ssid, saved_pass)) {

                WiFi.scanDelete();
                scanning = false;

                start_auto_connect(saved_ssid, saved_pass);

                return;
            }
        }

        // Riempie il menu a tendina con le reti trovate
        String options = "";

        for (int i = 0; i < result; i++) {

            if (i > 0) {
                options += "\n";
            }

            options += WiFi.SSID(i);
        }

        ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

        network_count = result;

        if (result == 0) {
            options = "Nessuna rete trovata";
            set_status("Nessuna rete trovata");
        } else {
            set_status("Seleziona la tua rete Wi-Fi");
        }

        lv_dropdown_set_options(wifi_dropdown, options.c_str());
        lv_dropdown_set_selected(wifi_dropdown, 0);
        lv_obj_clear_state(scan_button, LV_STATE_DISABLED);

        esp_lv_adapter_unlock();

        WiFi.scanDelete();
        scanning = false;
    }

    // --------------------------------------------------
    // ERRORE SCANSIONE
    // --------------------------------------------------

    else if (result == WIFI_SCAN_FAILED) {

        auto_connect_pending = false;

        ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

        set_status("Errore durante la ricerca");
        lv_obj_clear_state(scan_button, LV_STATE_DISABLED);

        esp_lv_adapter_unlock();

        WiFi.scanDelete();
        scanning = false;
    }
}


// ==================================================
// 27. CONTROLLO CONNESSIONE WIFI
// ==================================================

static void handle_wifi_connection()
{
    if (!connecting || millis() - last_wifi_check < 500) {
        return;
    }

    last_wifi_check = millis();

    // --------------------------------------------------
    // CONNESSIONE RIUSCITA
    // --------------------------------------------------

    if (WiFi.status() == WL_CONNECTED) {

        // Salva le credenziali
        save_wifi_network(pending_wifi_ssid.c_str(), pending_wifi_password.c_str());

        pending_wifi_ssid = "";
        pending_wifi_password = "";

        ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

        set_status("CONNESSIONE RIUSCITA!");

        lv_label_set_text_fmt(
            ip_label,
            "SSID: %s\nIP: %s",
            WiFi.SSID().c_str(),
            WiFi.localIP().toString().c_str()
        );

        // Mostra il pulsante Dashboard
        lv_obj_clear_flag(dashboard_button, LV_OBJ_FLAG_HIDDEN);

        esp_lv_adapter_unlock();

        connecting = false;

        // Disattiva il risparmio energetico WiFi: risposte e trasferimenti piu' veloci
        WiFi.setSleep(false);

        // Avvia il web server per l'upload della GIF (una sola volta)
        if (!web_started) {
            start_web_server();
        }

        // Connessione automatica: apre la dashboard tra 2 secondi
        if (auto_dashboard_pending) {

            auto_dashboard_pending = false;

            dashboard_scheduled = true;
            dashboard_at = millis() + AUTO_DASHBOARD_DELAY_MS;
        }
    }

    // --------------------------------------------------
    // TIMEOUT CONNESSIONE
    // --------------------------------------------------

    else if (millis() - connection_started_at >= 30000) {

        const bool was_auto = auto_dashboard_pending;

        auto_dashboard_pending = false;
        connecting = false;

        ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

        if (was_auto) {
            // Auto-connessione fallita: ripiega sulla scansione manuale
            scan_event(nullptr);
        } else {
            set_status("Connessione non riuscita");
        }

        esp_lv_adapter_unlock();
    }
}


// ==================================================
// 28. APERTURA AUTOMATICA DASHBOARD
// ==================================================

static void handle_auto_dashboard()
{
    if (!dashboard_scheduled) {
        return;
    }

    if ((long)(millis() - dashboard_at) < 0) {
        return;
    }

    dashboard_scheduled = false;

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    loadScreen(SCREEN_ID_DASHBOARD_SCREEN);

    esp_lv_adapter_unlock();

    set_polling(true, false);
}


// ==================================================
// 29. SCREENSHOT RGB565 (COMANDO SERIALE 's')
// ==================================================

// Protocollo: "KMSHOT01" + larghezza (4 byte) + altezza (4 byte)
// + framebuffer RGB565 + "SCREENSHOT_DONE\n"
static void screenshotDisplay()
{
    if (board == nullptr) {
        return;
    }

    // Pausa del task di rete
    screenshot_requested = true;

    while (!net_task_paused) {
        delay(1);
    }

    LCD *lcd = board->getLCD();

    if (lcd == nullptr) {
        screenshot_requested = false;
        return;
    }

    const uint32_t width = lcd->getFrameWidth();
    const uint32_t height = lcd->getFrameHeight();
    const size_t frameSize = (size_t)width * height * 2;

    auto *snapshot = static_cast<uint8_t *>(
        heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );

    if (snapshot == nullptr) {
        screenshot_requested = false;
        return;
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(snapshot);
        screenshot_requested = false;
        return;
    }

    auto *frame = static_cast<uint8_t *>(esp_lv_adapter_get_active_frame_buffer());

    if (frame == nullptr) {
        esp_lv_adapter_unlock();
        heap_caps_free(snapshot);
        screenshot_requested = false;
        return;
    }

    memcpy(snapshot, frame, frameSize);

    esp_lv_adapter_unlock();

    // Header
    Serial.write("KMSHOT01", 8);
    Serial.write(reinterpret_cast<const uint8_t *>(&width), 4);
    Serial.write(reinterpret_cast<const uint8_t *>(&height), 4);

    // Framebuffer
    size_t sent = 0;

    while (sent < frameSize) {

        const size_t chunk = min((size_t)1024, frameSize - sent);
        const size_t written = Serial.write(snapshot + sent, chunk);

        if (written > 0) {
            sent += written;
        } else {
            delay(1);
        }
    }

    Serial.flush();

    heap_caps_free(snapshot);

    // Fine trasferimento
    Serial.println("SCREENSHOT_DONE");

    // Riprende la rete
    screenshot_requested = false;
}


// ==================================================
// 30. MICROSD
// ==================================================

// Monta la microSD (serve alla GIF del tamagochi)
static void init_sd_card()
{
    auto *io_expander = board->getIO_Expander();

    if (io_expander == nullptr) {
        return;
    }

    auto *expander = io_expander->getBase();

    if (expander == nullptr) {
        return;
    }

    // EXIO4 = SD_CS (gestito dal CH422G)
    if (!expander->digitalWrite(4, LOW)) {
        return;
    }

    // SPI hardware ESP32-S3
    SPI.setHwCs(false);
    SPI.begin(12, 13, 11, -1);

    SD.begin(0xFF, SPI, 4000000);
}


// ==================================================
// 31. SETUP
// ==================================================

void setup()
{
    Serial.begin(115200);

    delay(500);

    // Sensore termico interno ESP32-S3 per la diagnostica in alto a destra.
    init_esp32_temperature();

    // Impostazioni salvate (intervalli)
    load_settings();

    // --------------------------------------------------
    // INIZIALIZZAZIONE SCHEDA
    // --------------------------------------------------

    board = new Board();

    if (board == nullptr || !board->init()) {
        fatal_error("Board init failed");
    }

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;

    const esp_lv_adapter_tear_avoid_mode_t tear_mode =
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;

    const uint8_t frame_buffer_count =
        esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    LCD *lcd = board->getLCD();

    if (lcd == nullptr) {
        fatal_error("LCD unavailable");
    }

    auto *lcd_bus = lcd->getBus();

    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {

        lcd->configFrameBufferNumber(frame_buffer_count);

        // Bounce buffer RGB (20 righe: evita i glitch)
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(
            lcd->getFrameWidth() * 20
        );
    }

    if (!board->begin()) {
        fatal_error("Board begin failed");
    }

    // MicroSD
    init_sd_card();

    // --------------------------------------------------
    // ADAPTER LVGL
    // --------------------------------------------------

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();

    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.task_priority = 2;
    adapter_config.task_core_id = ARDUINO_RUNNING_CORE;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    // --------------------------------------------------
    // DISPLAY
    // --------------------------------------------------

    esp_lv_adapter_display_config_t disp_config =
        ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
            lcd,
            lcd->getFrameWidth(),
            lcd->getFrameHeight(),
            rotation
        );

    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);

    if (disp == nullptr) {
        fatal_error("Display registration failed");
    }

    // --------------------------------------------------
    // TOUCH
    // --------------------------------------------------

    if (board->getTouch() != nullptr) {

        esp_lv_adapter_touch_config_t touch_config =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, board->getTouch());

        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);

        if (touch == nullptr) {
            fatal_error("Touch registration failed");
        }
    }

    // --------------------------------------------------
    // AVVIO LVGL E INTERFACCIA
    // --------------------------------------------------

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    init_interface();

    set_status("Ricerca reti salvate...");

    esp_lv_adapter_unlock();

    // --------------------------------------------------
    // SCANSIONE AUTOMATICA ALL'AVVIO
    // --------------------------------------------------

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.scanDelete();
    WiFi.scanNetworks(true);

    scanning = true;
    auto_connect_pending = true;

    // --------------------------------------------------
    // TASK RETE SU CORE 0
    // --------------------------------------------------

    const BaseType_t net_ok = xTaskCreatePinnedToCore(
        net_task,
        "net",
        NET_TASK_STACK,
        nullptr,
        NET_TASK_PRIORITY,
        nullptr,
        NET_TASK_CORE
    );

    if (net_ok != pdPASS) {
        fatal_error("Network task creation failed");
    }
}


// ==================================================
// 32. LOOP PRINCIPALE
// ==================================================

void loop()
{
    // Comando screenshot da USB seriale
    if (Serial.available() > 0 && Serial.read() == 's') {
        screenshotDisplay();
    }

    handle_wifi_scan();
    handle_wifi_connection();
    handle_auto_dashboard();

    if (web_started) {
        web.handleClient();
    }

    delay(20);
}
