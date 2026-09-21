#include "main_screen_runtime.h"

#include <stdint.h>
#include <string.h>

#include "services/power/power_service.h"
#include "services/weather/weather_service.h"

static const void *main_screen_weather_icon_src_from_path(const char *icon_path)
{
    if (icon_path == NULL) {
        return &_weather_duoyun_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/sunny.png") == 0) {
        return &_weather_sunny_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/yintian.png") == 0) {
        return &_weather_yintian_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/rain.png") == 0) {
        return &_weather_rain_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/snow.png") == 0) {
        return &_weather_snow_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/wu.png") == 0) {
        return &_weather_wu_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/mai.png") == 0) {
        return &_weather_mai_RGB565A8_96x96;
    }
    if (strcmp(icon_path, "A:/weather/wind.png") == 0) {
        return &_weather_wind_RGB565A8_96x96;
    }
    return &_weather_duoyun_RGB565A8_96x96;
}

static void main_screen_refresh_battery(lv_ui *ui)
{
    if (!lv_obj_is_valid(ui->screen_main_battery_label) ||
        !lv_obj_is_valid(ui->screen_main_battery_fill)) {
        return;
    }

    board_power_state_t power = {0};
    uint8_t percent = 80;
    if (power_service_get_snapshot(&power) == ESP_OK &&
        power.battery_data_valid) {
        percent = power.battery_percent > 100 ? 100 : power.battery_percent;
    }

    static uint8_t last_percent = UINT8_MAX;
    if (percent == last_percent) {
        return;
    }

    lv_label_set_text_fmt(ui->screen_main_battery_label, "%d%%", percent);
    int fill_width = (24 * percent) / 100;
    if (fill_width < 1 && percent > 0) {
        fill_width = 1;
    }
    lv_obj_set_width(ui->screen_main_battery_fill, fill_width);
    last_percent = percent;
}

static void main_screen_refresh_weather(lv_ui *ui)
{
    if (!lv_obj_is_valid(ui->screen_main_weather_temp) ||
        !lv_obj_is_valid(ui->screen_main_weather_text) ||
        !lv_obj_is_valid(ui->screen_main_weather_icon)) {
        return;
    }

    weather_info_t info = {0};
    if (weather_service_get_info(&info) != ESP_OK || !info.is_valid) {
        return;
    }

    static int last_temp = -999;
    static char last_text[32] = {0};
    static char last_icon_path[64] = {0};
    if (info.temp == last_temp && strcmp(info.weather_text, last_text) == 0 &&
        strcmp(info.icon_path, last_icon_path) == 0) {
        return;
    }

    lv_label_set_text_fmt(ui->screen_main_weather_temp, "%d", info.temp);
    lv_label_set_text(ui->screen_main_weather_text, info.weather_text);
    lv_image_set_src(ui->screen_main_weather_icon,
                     main_screen_weather_icon_src_from_path(info.icon_path));
    lv_image_set_scale(ui->screen_main_weather_icon, LV_SCALE_NONE);

    last_temp = info.temp;
    strncpy(last_text, info.weather_text, sizeof(last_text) - 1);
    last_text[sizeof(last_text) - 1] = '\0';
    strncpy(last_icon_path, info.icon_path, sizeof(last_icon_path) - 1);
    last_icon_path[sizeof(last_icon_path) - 1] = '\0';
}

void main_screen_runtime_refresh(lv_ui *ui)
{
    if (ui == NULL) {
        return;
    }

    main_screen_refresh_battery(ui);
    main_screen_refresh_weather(ui);
}
