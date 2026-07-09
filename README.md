# AI Memory Watch

基于 ESP32-S3 的智能手表固件，集成 AI 语音助手、危险检测、表盘动画等功能。

## 功能特性

- **AI 语音助手 (Hermes)**：按住说话，支持语音转文字、AI 对话、记忆存储
- **危险声音检测**：实时检测烟雾报警器、喇叭等危险声音，本地 AI 推理
- **跌倒检测**：6 轴 IMU 数据 + CNN 模型，实时跌倒检测与告警
- **天气时间**：心知天气 API 实时天气，PCF85063TL RTC 精准时钟
- **表盘动画**：5 种状态动画（空闲/思考/工作/消息/错误）
- **小游戏**：内置小游戏娱乐功能
- **BLE/WiFi 配网**：SoftAP + BLE Provisioning 配网方式
- **低功耗管理**：AXP2101 PMIC 电源管理，支持 Light Sleep

## 硬件平台

| 组件 | 型号 |
|------|------|
| 主控 | ESP32-S3 (N16R8) |
| 屏幕 | CO5300 AMOLED 410×502 圆角 |
| 触摸 | FT5x06 电容触摸 |
| IMU | QMI8658C 6 轴 |
| RTC | PCF85063TL |
| 音频 | I2S 麦克风 + 扬声器 |
| 电源 | AXP2101 PMIC |
| 存储 | 16MB Flash + 8MB PSRAM |

## 目录结构

```
├── main/                    # 主应用代码
│   ├── app/                 # 入口和硬件初始化
│   ├── services/            # 后台服务（网络、音频、存储等）
│   ├── features/            # 功能模块（天气、游戏、危险检测等）
│   └── ui/                  # LVGL UI 界面
│       ├── generated/       # GUI Guider 生成的页面代码
│       └── custom/          # 自定义 UI 和字体
├── components/              # ESP-IDF 组件
│   ├── co5300_panel/        # CO5300 AMOLED 屏幕驱动
│   ├── touch_ft5x06/        # FT5x06 触摸驱动
│   ├── audio_codec/         # 音频编解码
│   ├── network_manager/     # 网络管理（WiFi/BLE）
│   ├── imu_sensor/          # IMU 传感器
│   ├── official_chat/       # 小智 AI 对话
│   ├── espdl_inference/     # ESP-DL 推理引擎
│   ├── traffic_inference/   # Edge Impulse 推理
│   └── fall_detection_inference/ # 跌倒检测
├── assets/                  # AI 字体资源
└── scripts/                 # 构建脚本
```

## 快速开始

### 环境准备

1. 安装 [ESP-IDF v5.5+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
2. 克隆本仓库

### 编译烧录

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录（替换为实际串口）
idf.py -p /dev/ttyUSB0 app-flash

# 查看日志
idf.py -p /dev/ttyUSB0 monitor
```

### 配置

首次使用需要通过 SoftAP 配网：

1. 长按表冠进入配网模式
2. 手机连接手表热点 `ESP32S3-Watch`
3. 浏览器访问 `192.168.4.1` 完成 WiFi 配置
4. 可选配置 Hermes 语音助手服务端地址

## 许可证

本项目采用 [MIT 许可证](LICENSE)。

## 致谢

- [ESP-IDF](https://github.com/espressif/esp-idf) - Espressif IoT 开发框架
- [LVGL](https://github.com/lvgl/lvgl) - 轻量级图形库
- [Edge Impulse](https://www.edgeimpulse.com/) - 嵌入式机器学习平台
- [LXGW WenKai](https://github.com/lxgw/LxgwWenKai) - 霞鹜文楷字体
