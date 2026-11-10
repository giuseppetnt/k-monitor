#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_vento;
extern const lv_img_dsc_t img_umidita;
extern const lv_img_dsc_t img_pressione;
extern const lv_img_dsc_t img_pioggia;
extern const lv_img_dsc_t img_cloud;
extern const lv_img_dsc_t img_sun;
extern const lv_img_dsc_t img_storm;
extern const lv_img_dsc_t img_rain;
extern const lv_img_dsc_t img_snow;
extern const lv_img_dsc_t img_cloudy;
extern const lv_img_dsc_t img_logo;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[11];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/