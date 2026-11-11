#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_WIFI_SCREEN = 1,
    SCREEN_ID_DASHBOARD_SCREEN = 2,
    SCREEN_ID_BTOP_SCREEN = 3,
    SCREEN_ID_WEATHER_SCREEN = 4,
    SCREEN_ID_SETTINGS_SCREEN = 5,
    SCREEN_ID_TAMAGOCHI = 6,
    _SCREEN_ID_LAST = 6
};

typedef struct _objects_t {
    lv_obj_t *wifi_screen;
    lv_obj_t *dashboard_screen;
    lv_obj_t *btop_screen;
    lv_obj_t *weather_screen;
    lv_obj_t *settings_screen;
    lv_obj_t *tamagochi;
    lv_obj_t *info;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_subtitle;
    lv_obj_t *dd_wifi;
    lv_obj_t *ta_password;
    lv_obj_t *btn_scan;
    lv_obj_t *btn_connect;
    lv_obj_t *lbl_status;
    lv_obj_t *btn_dashboard;
    lv_obj_t *btn_settings;
    lv_obj_t *btn_tamagochi;
    lv_obj_t *obj0;
    lv_obj_t *btn_wifi;
    lv_obj_t *lbl_cpu;
    lv_obj_t *lbl_ram;
    lv_obj_t *lbl_disk;
    lv_obj_t *bar_cpu;
    lv_obj_t *bar_ram;
    lv_obj_t *bar_disk;
    lv_obj_t *lbl_uptime;
    lv_obj_t *lbl_server_status;
    lv_obj_t *img_logo;
    lv_obj_t *lbl_hostname;
    lv_obj_t *lbl_status_srv_1;
    lv_obj_t *lbl_status_srv_2;
    lv_obj_t *btn_go_btop;
    lv_obj_t *led_server;
    lv_obj_t *btn_weather;
    lv_obj_t *obj1;
    lv_obj_t *lbl_title_1;
    lv_obj_t *lbl_subtitle_1;
    lv_obj_t *lbl_security_status;
    lv_obj_t *lbl_sec_ssh_failed;
    lv_obj_t *lbl_sec_banned;
    lv_obj_t *lbl_sec_recidive;
    lv_obj_t *lbl_sec_firewall;
    lv_obj_t *lbl_subtitle_5;
    lv_obj_t *lbl_subtitle_6;
    lv_obj_t *lbl_subtitle_7;
    lv_obj_t *lbl_subtitle_8;
    lv_obj_t *lbl_subtitle_9;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *btn_btop_back;
    lv_obj_t *arc_cpu_temp;
    lv_obj_t *label_cpu;
    lv_obj_t *lbl_btop_cpu_temp;
    lv_obj_t *arc_temp1;
    lv_obj_t *label_cpu_1;
    lv_obj_t *lbl_btop_temp1;
    lv_obj_t *arc_fan1;
    lv_obj_t *label_cpu_2;
    lv_obj_t *lbl_btop_fan1;
    lv_obj_t *chart_fan_rpm;
    lv_obj_t *lbl_title_2;
    lv_obj_t *lbl_subtitle_2;
    lv_obj_t *chart_board;
    lv_obj_t *chart_cpu;
    lv_obj_t *obj6;
    lv_obj_t *label_live;
    lv_obj_t *led_ntp;
    lv_obj_t *btn_weather_back;
    lv_obj_t *label_timer_weather;
    lv_obj_t *label_pressure_title;
    lv_obj_t *label_pressure_value;
    lv_obj_t *label_rain_title;
    lv_obj_t *label_rain_value;
    lv_obj_t *label_wind_title;
    lv_obj_t *label_humidity_title;
    lv_obj_t *label_humidity_value;
    lv_obj_t *label_wind_value;
    lv_obj_t *label_feels_like;
    lv_obj_t *label_condition;
    lv_obj_t *label_temperature;
    lv_obj_t *png_weather;
    lv_obj_t *label_date;
    lv_obj_t *label_time;
    lv_obj_t *lbl_title_4;
    lv_obj_t *lbl_subtitle_4;
    lv_obj_t *label_rain_title_1;
    lv_obj_t *obj7;
    lv_obj_t *lbl_poll_value;
    lv_obj_t *btn_poll_dec;
    lv_obj_t *btn_poll_inc;
    lv_obj_t *lbl_weather_value;
    lv_obj_t *btn_weather_dec;
    lv_obj_t *btn_weather_inc;
    lv_obj_t *btn_settings_reset;
    lv_obj_t *btn_settings_back;
    lv_obj_t *lbl_title_3;
    lv_obj_t *lbl_subtitle_3;
    lv_obj_t *obj8;
    lv_obj_t *btn_exit;
    lv_obj_t *obj_kerby;
    lv_obj_t *clock_tamagochi;
    lv_obj_t *obj9;
} objects_t;

extern objects_t objects;

void create_screen_wifi_screen();
void tick_screen_wifi_screen();

void create_screen_dashboard_screen();
void tick_screen_dashboard_screen();

void create_screen_btop_screen();
void tick_screen_btop_screen();

void create_screen_weather_screen();
void tick_screen_weather_screen();

void create_screen_settings_screen();
void tick_screen_settings_screen();

void create_screen_tamagochi();
void tick_screen_tamagochi();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/