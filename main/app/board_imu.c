#include "app/board_imu.h"

static const board_imu_config_t k_board_imu_config = {
    .qmi_i2c_addr_7bit = 0x6B,
    .qmi_int1_gpio = GPIO_NUM_21,
    .face_axis = BOARD_IMU_FACE_AXIS_NEG_Z,
};

const board_imu_config_t *board_imu_get_config(void)
{
    return &k_board_imu_config;
}
