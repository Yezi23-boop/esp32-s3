#pragma once

/*
 * 当前板级原理图将 QMI8658C 标注为 0x6B。地址由 SA0 绑法决定，
 * 若后续硬件版本变化，需要重新用 I2C scan 或原理图确认。
 */
#define QMI8658C_I2C_ADDR_7BIT 0x6B

/* 通用识别寄存器，来自 QMI8658C 手册 Register Map。 */
#define QMI8658C_REG_WHO_AM_I 0x00
#define QMI8658C_WHO_AM_I_EXPECTED 0x05
#define QMI8658C_REG_REVISION_ID 0x01

/* 当前 Rev A 路径只使用基础配置寄存器，不启用 FIFO。 */
#define QMI8658C_REG_CTRL1 0x02
#define QMI8658C_REG_CTRL2 0x03
#define QMI8658C_REG_CTRL3 0x04
#define QMI8658C_REG_CTRL5 0x06
#define QMI8658C_REG_CTRL7 0x08
#define QMI8658C_REG_CTRL8 0x09
#define QMI8658C_REG_CTRL9 0x0A

/* STATUS0 bit0/bit1 分别由 Waveshare 驱动用于 accel/gyro data-ready 判断。 */
#define QMI8658C_REG_STATUS0 0x2E

/* 24-bit timestamp + 温度 + 连续六轴原始数据窗口。 */
#define QMI8658C_REG_TIMESTAMP_L 0x30
#define QMI8658C_REG_TEMP_L 0x33
#define QMI8658C_REG_AX_L 0x35
#define QMI8658C_REG_GX_L 0x3B

/*
 * Waveshare 官方 qmi8658_init() 写 CTRL1=0x60。这里保留已验证的
 * 官方初始化口径，避免当前板上样本寄存器不刷新。
 */
#define QMI8658C_CTRL1_ADDR_AUTO_INCREMENT (1u << 6)
#define QMI8658C_CTRL1_WAVESHARE_DEFAULT 0x60
#define QMI8658C_CTRL5_WAVESHARE_DEFAULT 0x03

/* CTRL2/CTRL3 字段掩码；驱动会额外拒绝手册标为 N/A 的 ODR 编码。 */
#define QMI8658C_CTRL2_ACCEL_FS_MASK 0x07
#define QMI8658C_CTRL2_ACCEL_ODR_MASK 0x0F
#define QMI8658C_CTRL3_GYRO_FS_MASK 0x07
#define QMI8658C_CTRL3_GYRO_ODR_MASK 0x0F

/* 当前 Rev A 寄存器图中 CTRL7 只使用加速度计和陀螺仪使能位。 */
#define QMI8658C_CTRL7_ACCEL_ENABLE (1u << 0)
#define QMI8658C_CTRL7_GYRO_ENABLE (1u << 1)

/*
 * Rev A 中 0x0C 是 Tap 配置命令，不是旧 Rev0.6 文档中的 MoD 请求。
 * 保留该常量用于 source test 防止后续再次把它误当作 MoD。
 */
#define QMI8658C_CTRL9_CMD_CONFIGURE_TAP 0x0C
