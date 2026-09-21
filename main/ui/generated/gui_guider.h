/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#ifndef GUI_GUIDER_H
#define GUI_GUIDER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"


typedef struct
{
  
	lv_obj_t *screen_main;
	bool screen_main_del;
	lv_obj_t *screen_main_Contorl_Con;
	lv_obj_t *screen_main_tileview_1;
	lv_obj_t *screen_main_tileview_1_main;
	lv_obj_t *screen_main_tileview_1_Function;
	lv_obj_t *screen_main_cont_1;
	lv_obj_t *screen_main_digital_clock_1;
	lv_obj_t *screen_main_Function_main;
	lv_obj_t *screen_main_option_1;
	lv_obj_t *screen_main_Heart_rate;
	lv_obj_t *screen_main_option_2;
	lv_obj_t *screen_main_Xiao_Zhi;
	lv_obj_t *screen_main_option_4;
	lv_obj_t *screen_main_Game;
	lv_obj_t *screen_main_option_5;
	lv_obj_t *screen_main_clock;
	lv_obj_t *screen_main_option_6;
	lv_obj_t *screen_main_Microphone;
	lv_obj_t *screen_main_option_7;
	lv_obj_t *screen_main_seting;
	lv_obj_t *screen_main_option_8;
	lv_obj_t *screen_main_user;
	lv_obj_t *screen_main_top_grab_area;
	lv_obj_t *screen_main_Dropdown_menu;
	lv_obj_t *screen_main_Wifi;
	lv_obj_t *screen_main_Wifi_label;
	lv_obj_t *screen_main_Bluetooth;
	lv_obj_t *screen_main_Bluetooth_label;
	lv_obj_t *screen_main_Brightness;
	lv_obj_t *screen_main_loudness;
	lv_obj_t *screen_main_music_button;
	lv_obj_t *screen_main_music_button_label;
	lv_obj_t *screen_main_img_1;
	lv_obj_t *screen_main_imgbtn_1;
	lv_obj_t *screen_main_imgbtn_1_label;
	lv_obj_t *screen_main_date_label;
	lv_obj_t *screen_main_battery_label;
	lv_obj_t *screen_main_battery_bar;
	lv_obj_t *screen_main_battery_fill;
	lv_obj_t *screen_main_weather_card;
	lv_obj_t *screen_main_weather_icon;
	lv_obj_t *screen_main_weather_temp;
	lv_obj_t *screen_main_weather_text;
	lv_obj_t *screen_main_weather_range;
	lv_obj_t *screen_wallpaper;
	bool screen_wallpaper_del;
	lv_obj_t *screen_wallpaper_img_qp1;
	lv_obj_t *screen_wallpaper_img_qp2;
	lv_obj_t *screen_wallpaper_img_qp3;
	lv_obj_t *screen_wallpaper_img_qp4;
	lv_obj_t *screen_wallpaper_cont_1;
	lv_obj_t *screen_wallpaper_img_1;
	lv_obj_t *screen_wallpaper_img_2;
	lv_obj_t *screen_wallpaper_img_3;
	lv_obj_t *screen_wallpaper_img_4;
	lv_obj_t *screen_wallpaper_label_1;
	lv_obj_t *screen_time;
	bool screen_time_del;
	lv_obj_t *screen_time_datetext_1;
	lv_obj_t *screen_time_imgbtn_1;
	lv_obj_t *screen_time_imgbtn_1_label;
}lv_ui;

typedef void (*ui_setup_scr_t)(lv_ui * ui);

void ui_init_style(lv_style_t * style);

void ui_load_scr_animation(lv_ui *ui, lv_obj_t ** new_scr, bool new_scr_del, bool * old_scr_del, ui_setup_scr_t setup_scr,
                           lv_screen_load_anim_t anim_type, uint32_t time, uint32_t delay, bool is_clean, bool auto_del);

void ui_animation(void * var, uint32_t duration, int32_t delay, int32_t start_value, int32_t end_value, lv_anim_path_cb_t path_cb,
                  uint32_t repeat_cnt, uint32_t repeat_delay, uint32_t playback_time, uint32_t playback_delay,
                  lv_anim_exec_xcb_t exec_cb, lv_anim_start_cb_t start_cb, lv_anim_completed_cb_t ready_cb, lv_anim_deleted_cb_t deleted_cb);


void init_scr_del_flag(lv_ui *ui);

void setup_bottom_layer(void);

void setup_ui(lv_ui *ui);

void video_play(lv_ui *ui);

void init_keyboard(lv_ui *ui);

extern lv_ui guider_ui;


void setup_scr_screen_main(lv_ui *ui);
void setup_scr_screen_main_function_page(lv_ui *ui);
void setup_scr_screen_wallpaper(lv_ui *ui);
void setup_scr_screen_time(lv_ui *ui);

LV_IMAGE_DECLARE(_5_RGB565A8_410x502);
LV_IMAGE_DECLARE(_heart_RGB565A8_70x70);
LV_IMAGE_DECLARE(_ai_RGB565A8_70x70);
LV_IMAGE_DECLARE(_game_RGB565A8_70x70);
LV_IMAGE_DECLARE(_alarm_clock_RGB565A8_70x70);
LV_IMAGE_DECLARE(_Microphone_RGB565A8_70x70);
LV_IMAGE_DECLARE(_set_RGB565A8_70x70);
LV_IMAGE_DECLARE(_me_RGB565A8_70x70);
LV_IMAGE_DECLARE(_WIFI4_RGB565A8_76x76);
LV_IMAGE_DECLARE(_WIFI2_RGB565A8_76x76);
LV_IMAGE_DECLARE(_langya4_RGB565A8_76x76);
LV_IMAGE_DECLARE(_langya2_RGB565A8_76x76);
LV_IMAGE_DECLARE(_liandu_RGB565A8_46x46);
LV_IMAGE_DECLARE(_shengyin_RGB565A8_46x46);
LV_IMAGE_DECLARE(_shengyin3_RGB565A8_46x46);
LV_IMAGE_DECLARE(_1_RGB565A8_410x502);
LV_IMAGE_DECLARE(_2_RGB565A8_410x502);
LV_IMAGE_DECLARE(_3_RGB565A8_410x502);
LV_IMAGE_DECLARE(_4_RGB565A8_410x502);
LV_IMAGE_DECLARE(_weather_sunny_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_duoyun_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_yintian_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_rain_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_snow_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_wu_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_mai_RGB565A8_96x96);
LV_IMAGE_DECLARE(_weather_wind_RGB565A8_96x96);
LV_IMAGE_DECLARE(_yuanjiao1_RGB565A8_180x180);
LV_IMAGE_DECLARE(_yuanjiao2_RGB565A8_180x180);
LV_IMAGE_DECLARE(_yuanjiao3_RGB565A8_180x180);
LV_IMAGE_DECLARE(_yuanjiao4_RGB565A8_180x180);

LV_FONT_DECLARE(lv_font_montserratMedium_46)
LV_FONT_DECLARE(lv_font_montserratMedium_16)
LV_FONT_DECLARE(lv_font_montserratMedium_12)
LV_FONT_DECLARE(lv_font_SourceHanSerifSC_Regular_22)
LV_FONT_DECLARE(lv_font_montserratMedium_27)
LV_FONT_DECLARE(lv_font_montserratMedium_58)


#ifdef __cplusplus
}
#endif
#endif
