# AI Memory Watch

基于 ESP32-S3 的智能手表固件：AI 语音助手、危险声音识别、跌倒检测、在线音乐、OTA 升级与 LVGL 界面，推理与交互全部在片内完成。

📺 **演示视频**：[【ESP32-S3智能手表】](https://www.bilibili.com/video/BV1oqhB6JE1t?vd_source=1374b94fcc0de11cf1dcaf19ac622262)

## 功能特性

| 方向 | 能力 |
|------|------|
| AI 语音助手（Hermes） | 按住说话，语音转文字、多轮对话、记忆存取与回放；对接自建服务端，地址可配 |
| 危险声音识别 | 片内 AI 推理识别警笛 / 喇叭 / 警报等危险声音并触发提醒（主线走 `espdl_inference`） |
| 跌倒检测 | QMI8658C 六轴数据 + CNN 模型，实时检测并告警 |
| 在线音乐 | 服务端曲库拉流、流式解码与播放 |
| OTA 升级 | 双 OTA 槽位，支持 OneNET 与自建 HTTPS 清单，可选增量差分升级 |
| 天气时间 | 心知天气 API 实时天气 + PCF85063 RTC 精准走时 |
| 配网 | SoftAP 门户 + BLE Provisioning 双通道，凭据落 NVS |
| 界面 | LVGL 9.5 + GUI Guider 生成页面，表盘状态动画 |
| 电源管理 | AXP2101 PMIC，支持 Light Sleep |

## 硬件平台

| 部件 | 型号 / 说明 |
|------|------|
| 主控 | ESP32-S3（N16R8，16MB Flash + 8MB PSRAM） |
| 屏幕 | CO5300 AMOLED，410×502 QSPI |
| 触摸 | FT5x06 电容触摸 |
| IMU | QMI8658C 六轴 |
| RTC | PCF85063 |
| 音频 | I2S 麦克风 + 扬声器 |
| 电源 | AXP2101 PMIC |
| 马达 | DS2413（1-Wire，GPIO18） |
| 存储 | 片内 Flash + microSD |

## 目录结构

```
├── main/
│   ├── app/          # 入口、板级初始化（board_*）、硬件初始化
│   ├── services/     # 后台服务：memory_watch / music / ota / power / runtime
│   │                 #          sensors / safety / time / weather / network ...
│   ├── features/     # 功能模块：danger_detection / alerts / mini_games
│   └── ui/           # LVGL 界面
│       ├── generated/# GUI Guider 生成的页面与图像
│       └── custom/   # 自定义页面、控制器与字库
├── components/       # 板级驱动与能力组件（co5300_panel / touch_ft5x06 / qmi8658c /
│                     #   axp2101 / pcf85063atl / sd_card / audio_codec / lvgl_port /
│                     #   network_manager / espdl_inference ...）
├── assets/           # AI 字库索引
├── scripts/          # 构建脚本
├── .agents/skills/   # AI Agent 技能（见文末）
├── partitions.csv    # 分区表
└── sdkconfig.defaults
```

## 快速开始

### 环境准备

- ESP-IDF **v5.5 及以上**
- 首次编译会从 ESP Component Registry 自动拉取依赖（LVGL 9.5、`esp_lcd_co5300`、`esp-dl`、`xiaozhi-fonts`、`littlefs`、`esp_delta_ota` 等，清单见 `main/idf_component.yml`），需要联网

### 编译与烧录

```bash
idf.py set-target esp32s3

# 必做：启用仓库自带的分区表，否则应用放不进默认的 1MB 应用分区
idf.py menuconfig     # Partition Table → Partition Table → Custom partition table CSV

idf.py build

idf.py -p <PORT> flash        # 首次烧录：写入 bootloader / 分区表 / 应用
idf.py -p <PORT> app-flash    # 后续仅更新应用
idf.py -p <PORT> monitor
```

`partitions.csv` 使用双 OTA 布局：`ota_0` / `ota_1` 各 12MB，另有 `assets`（AI 字库）、`resources`（运行时资源）、`model`（推理模型）三个数据分区。

### 需要自行提供的外部服务

固件本身可编译运行，但下列服务地址在仓库中是占位符，需要替换成你自己的：

| 配置项 | 位置 | 说明 |
|--------|------|------|
| 天气 API Key | `main/services/weather/weather_http_client.c` | 当前为 `<YOUR_SENIVERSE_API_KEY>`，替换为你的心知天气 Key |
| 手表服务端地址 | `menuconfig` → AI Memory Watch → Default watch endpoint base URL | 默认 `watch.example.com` 为占位符；运行时以 SoftAP 配网写入 NVS 的值为准 |
| 音乐服务 | 配网 / NVS 配置 `base_url` 与 `device_id` | 仓库不含音乐服务端，需自备兼容协议的曲库服务 |
| OTA 服务器 | `menuconfig` → Standalone HTTPS OTA | 可选 OneNET，或自建 HTTPS 清单并填写 URL 与允许的 host |

### 配网

1. 进入配网模式
2. 手机连接手表热点
3. 浏览器访问 `192.168.4.1`，填写 WiFi 与可选的服务端地址
4. 保存后重启生效

## 仓库内容边界

- `components/traffic_inference`（Edge Impulse 交通声音识别）仅作为实验源码保留，已由根 `CMakeLists.txt` 的 `EXCLUDE_COMPONENTS` 排除出构建；危险检测主线使用 `espdl_inference`。
- **运行时资源未包含在本仓库**：`resources/`（字库、天气图标）与表盘动画 `*.rawanim` 需自行准备，缺失时对应界面资源不可用。
- 板级测试源码（`*_board_test.*`）不在本仓库中。

## AI Agent 技能

`.agents/skills/` 下收录了本项目开发过程中沉淀的 7 个 AI Agent 技能，均为纯 Markdown，可被支持 skills 的 AI 编程工具直接读取：

| 技能 | 用途 |
|------|------|
| `embedded-framework-mentor` | 嵌入式框架评审：模块边界、owner、禁止路径与最小实现 |
| `esp32-s3-basics` | 芯片能力、外设选型、内存约束与 FreeRTOS 问题排查 |
| `esp-idf-gui-guider-lvgl93-bridge` | GUI Guider 生成代码与手写业务逻辑的衔接 |
| `gui-guider-lvgl-preview` | 基于 GUI Guider 结构的界面预览与截图 |
| `vue-lvgl-pixel-ui` | Vue 定稿 → LVGL 复刻 → 分层截图比对的像素级 UI 流程 |
| `lvgl-chinese-ui-fonts` | 中文 UI 的字体绑定、字符集与子集生成策略 |
| `esp-idf-project-context` | ESP-IDF 仓库的项目上下文体系设计与迁移 |

## 许可证

本项目采用 [MIT 许可证](LICENSE)。

## 致谢

- [ESP-IDF](https://github.com/espressif/esp-idf) — Espressif IoT 开发框架
- [LVGL](https://github.com/lvgl/lvgl) — 轻量级图形库
- [ESP-DL](https://github.com/espressif/esp-dl) — Espressif 深度学习推理库
- [xiaozhi-fonts](https://components.espressif.com/components/78/xiaozhi-fonts) — 内嵌中文字库资源（Noto Sans 子集）
- [Edge Impulse](https://www.edgeimpulse.com/) — 嵌入式机器学习平台（实验分支）
- [LXGW WenKai](https://github.com/lxgw/LxgwWenKai) — 霞鹜文楷字体
