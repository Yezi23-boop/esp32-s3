#include "ds2413.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "onewire_bus.h"
#include "onewire_cmd.h"
#include "onewire_device.h"

static const char *TAG = "ds2413";

static bool ds2413_is_supported_family(uint8_t family_code)
{
    return family_code == DS2413_BOARD_FAMILY_CODE;
}

static esp_err_t ds2413_find_by_index_impl(onewire_bus_handle_t bus,
                                           size_t index,
                                           ds2413_device_t *device);

static esp_err_t ds2413_select_device(const ds2413_device_t *device)
{
    ESP_RETURN_ON_FALSE(device != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device is null");
    ESP_RETURN_ON_FALSE(device->bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bus is null");

    ESP_RETURN_ON_ERROR(onewire_bus_reset(device->bus), TAG,
                        "reset bus failed");

    const uint8_t match_cmd = ONEWIRE_CMD_MATCH_ROM;
    ESP_RETURN_ON_ERROR(onewire_bus_write_bytes(device->bus, &match_cmd, 1),
                        TAG, "write MATCH ROM failed");
    ESP_RETURN_ON_ERROR(
        onewire_bus_write_bytes(device->bus, (const uint8_t *)&device->address,
                                sizeof(device->address)),
        TAG, "write ROM address failed");
    return ESP_OK;
}

static esp_err_t ds2413_decode_state(uint8_t raw, ds2413_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "state is null");

    // DS2413 状态高 4 位是低 4 位反码；不满足时通常是总线毛刺或时序读错。
    ESP_RETURN_ON_FALSE(((uint8_t)(~raw) & 0x0F) == (raw >> 4),
                        ESP_ERR_INVALID_CRC, TAG,
                        "state complement check failed: 0x%02X", raw);

    state->raw = raw;
    state->pioa_state = (raw & 0x01) != 0;
    state->pioa_latch = (raw & 0x02) != 0;
    state->piob_state = (raw & 0x04) != 0;
    state->piob_latch = (raw & 0x08) != 0;
    return ESP_OK;
}

esp_err_t ds2413_find_first(onewire_bus_handle_t bus, ds2413_device_t *device)
{
    return ds2413_find_by_index_impl(bus, 0, device);
}

esp_err_t ds2413_find_by_index(onewire_bus_handle_t bus, size_t index,
                               ds2413_device_t *device)
{
    return ds2413_find_by_index_impl(bus, index, device);
}

static esp_err_t ds2413_find_by_index_impl(onewire_bus_handle_t bus,
                                           size_t index,
                                           ds2413_device_t *device)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bus is null");
    ESP_RETURN_ON_FALSE(device != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device is null");

    onewire_device_iter_handle_t iter = NULL;
    ESP_RETURN_ON_ERROR(onewire_new_device_iter(bus, &iter), TAG,
                        "create device iterator failed");

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    esp_err_t search_ret = ESP_OK;
    size_t matched_index = 0; // 只统计 family code 匹配的 DS2413，忽略同总线上的其他 1-Wire 器件。
    onewire_device_t next_device = {0};

    while ((search_ret = onewire_device_iter_get_next(iter, &next_device)) ==
           ESP_OK)
    {
        uint8_t family_code = ((const uint8_t *)&next_device.address)[0];
        if (!ds2413_is_supported_family(family_code))
        {
            continue;
        }

        if (matched_index++ != index)
        {
            continue;
        }

        device->bus = bus;
        device->address = next_device.address;
        ret = ESP_OK;
        break;
    }

    if (ret != ESP_OK && search_ret != ESP_ERR_NOT_FOUND)
    {
        ret = search_ret;
    }

    esp_err_t del_ret = onewire_del_device_iter(iter);
    if (ret == ESP_OK)
    {
        ESP_RETURN_ON_ERROR(del_ret, TAG, "delete device iterator failed");
        return ESP_OK;
    }

    if (del_ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "delete device iterator returned 0x%x while no device matched",
                 (int)del_ret);
    }
    return ret;
}

esp_err_t ds2413_read_state(const ds2413_device_t *device,
                            ds2413_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "state is null");
    ESP_RETURN_ON_ERROR(ds2413_select_device(device), TAG,
                        "select DS2413 failed");

    const uint8_t read_cmd = DS2413_ACCESS_READ;
    uint8_t raw = 0;
    ESP_RETURN_ON_ERROR(onewire_bus_write_bytes(device->bus, &read_cmd, 1),
                        TAG, "write PIO_ACCESS_READ failed");
    ESP_RETURN_ON_ERROR(onewire_bus_read_bytes(device->bus, &raw, 1), TAG,
                        "read DS2413 state failed");

    esp_err_t ret = ds2413_decode_state(raw, state);
    (void)onewire_bus_reset(device->bus);
    return ret;
}

esp_err_t ds2413_write_latch(const ds2413_device_t *device,
                             const ds2413_latch_state_t *latch_state,
                             ds2413_state_t *verified_state)
{
    ESP_RETURN_ON_FALSE(latch_state != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "latch_state is null");
    ESP_RETURN_ON_ERROR(ds2413_select_device(device), TAG,
                        "select DS2413 failed");

    uint8_t state_byte = 0xFC; // 高 6 位按 DS2413 写协议保持 1，低 2 位承载 PIOA/PIOB latch。
    if (latch_state->pioa_release)
    {
        state_byte |= 0x01;
    }
    if (latch_state->piob_release)
    {
        state_byte |= 0x02;
    }

    const uint8_t write_cmd = DS2413_ACCESS_WRITE;
    const uint8_t inverted_state = (uint8_t)~state_byte; // 写协议要求状态字节后跟反码，用于器件侧校验。
    uint8_t ack = 0;
    uint8_t raw = 0;

    ESP_RETURN_ON_ERROR(onewire_bus_write_bytes(device->bus, &write_cmd, 1),
                        TAG, "write PIO_ACCESS_WRITE failed");
    ESP_RETURN_ON_ERROR(onewire_bus_write_bytes(device->bus, &state_byte, 1),
                        TAG, "write DS2413 state failed");
    ESP_RETURN_ON_ERROR(
        onewire_bus_write_bytes(device->bus, &inverted_state, 1), TAG,
        "write inverted DS2413 state failed");
    ESP_RETURN_ON_ERROR(onewire_bus_read_bytes(device->bus, &ack, 1), TAG,
                        "read DS2413 ack failed");

    if (ack != DS2413_ACK_SUCCESS)
    {
        (void)onewire_bus_reset(device->bus);
        ESP_LOGW(TAG, "unexpected DS2413 ack: 0x%02X", ack);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(onewire_bus_read_bytes(device->bus, &raw, 1), TAG,
                        "read DS2413 verify state failed");

    ds2413_state_t temp_state = {0};
    ds2413_state_t *out_state =
        verified_state != NULL ? verified_state : &temp_state;
    esp_err_t ret = ds2413_decode_state(raw, out_state);
    if (ret == ESP_OK)
    {
        bool pioa_ok = out_state->pioa_latch == latch_state->pioa_release;
        bool piob_ok = out_state->piob_latch == latch_state->piob_release;
        if (!pioa_ok || !piob_ok)
        {
            ret = ESP_ERR_INVALID_CRC;
        }
    }

    (void)onewire_bus_reset(device->bus);
    return ret;
}
