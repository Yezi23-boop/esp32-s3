# 当前项目真实边界

本文件只描述当前仓库已经成立的模块边界，不讨论通用理论。

主要依据：

- `docs/context/knowledge/project/project-framework.md`
- `docs/context/knowledge/project/runtime-owner-contract.md`
- `docs/context/knowledge/project/layering-boundary-map.md`
- `docs/context/knowledge/project/network-provisioning-custom-upper-architecture.md`
- `docs/context/knowledge/project/low-power-management-baseline.md`
- `docs/context/knowledge/project/low-power-framework-architecture.md`

## 统一判断原则

- 先找 owner，再找调用链。
- 如果某层只负责状态编排，就不要往里塞底层设备细节。
- 如果某层只负责 runtime control，就不要往里塞产品策略。
- 如果某层只是兼容 shim，就不要把新功能继续堆进去。

## 当前重点模块

### `network_manager`

定位：

- 当前项目唯一正式网络统一门面

负责：

- 开机尝试 latest Wi-Fi
- recent 失败后的 provisioning 入口编排
- 提供 `Use Saved Wi-Fi / Disconnect / Reprovision / BLE 总开关` 这类产品级动作
- 收敛 `BLE / SoftAP / latest / idle / connected / error` 这些网络状态语义

不负责：

- 直接实现 STA 驱动细节
- 直接处理 SoftAP 门户 HTTP 细节
- 持有页面逻辑

适合修改的场景：

- 网络动作语义变更
- 默认 transport 选择策略
- recent / latest / reprovision 的编排边界
- UI 要调用的新网络门面能力

### `wifi_control`

定位：

- 纯 `Wi-Fi STA runtime control`

负责：

- Wi-Fi 初始化
- 指定 SSID/密码连接
- 主动断开
- 自动重连开关
- STA 连接状态和 IP 查询

不负责：

- BLE / SoftAP provisioning transport
- recent Wi-Fi 列表
- UI 语义
- 复杂产品策略

适合修改的场景：

- STA 连接、断开、重连、IP 获取
- Wi-Fi power save
- 与 STA 运行时直接相关的错误恢复

### `network_provisioning_adapter`

定位：

- 官方 `network_provisioning` 的项目适配层

负责：

- `BLE / SoftAP` transport 的统一生命周期
- 对接官方 provisioning manager
- 在 SoftAP 路径准备官方 endpoint 运行所需的底层条件

不负责：

- 页面外观
- Wi-Fi 管理页文案
- recent Wi-Fi 语义
- UI 入口策略

适合修改的场景：

- provisioning transport 切换
- 官方 `prov-*` 生命周期接缝
- SoftAP / BLE 底层配网承载问题

### `ap_portal_adapter`

定位：

- 自定义 AP 门户网页壳 + SoftAP HTTPD 复用层

负责：

- 门户静态资源
- 门户 HTTP 路由壳
- 把 HTTPD handle 交给官方 SoftAP provisioning

不负责：

- Wi-Fi latest / recent 策略
- STA runtime control
- 主界面网络状态语义

适合修改的场景：

- 门户页面资源挂载
- 门户 HTTPD 生命周期
- SoftAP 门户前端与官方 `prov-*` 的接缝

### `network_service`

定位：

- 兼容 shim + service-ready 探测层

负责：

- 调用 `network_manager_start()`
- 后台探测服务是否 ready
- 兼容旧消费者

不负责：

- 新的网络产品语义设计
- 新的 provisioning 主流程

判断提示：

- 如果是新功能，优先不要继续往 `network_service` 堆主逻辑。

### `main/ui`

定位：

- 页面和交互语义层

负责：

- 展示当前状态
- 触发 `network_manager` 暴露的动作
- 处理用户可见反馈

不负责：

- 直接做 Wi-Fi runtime control
- 直接做 provisioning transport 生命周期控制

判断提示：

- UI 能决定“用户想做什么”，但不应该自己决定“底层怎么联网”。

### `power_policy`

定位：

- 整机资源预算发布者

负责：

- 读取各 owner snapshot
- 聚合 activity、电源、网络、音频、后台和 alert facts
- 发布 `power_budget`、`sleep_permission` 和 `sleep_blockers`

不负责：

- 直接调 LVGL、屏幕亮度、Wi-Fi、音频、模型、PMIC 或 `esp_sleep_*`

判断提示：

- 低功耗策略应先变成预算或 blocker；具体动作由各资源 owner 消费预算执行。

### `ui_refresh_policy`

定位：

- UI 活跃度、刷新节奏和 runtime `STANDBY` 的 owner

负责：

- 触摸/交互 activity snapshot
- UI 降刷新和亮度策略
- 向 `power_policy` 暴露只读 activity fact

不负责：

- Wi-Fi、音频、PMIC、ESP sleep 或后台模型生命周期

### `sleep_coordinator`

定位：

- sleep 预算消费者和 dry-run 观测点

负责：

- 读取 `power_budget` 中的 sleep permission、blockers 和 interval hint
- 打印未来 sleep 条件是否具备

不负责：

- 手动调用 `esp_light_sleep_start()` 或 `esp_deep_sleep_start()`
- 逐个理解网络、音频、UI、后台任务内部状态
- 断网、重连或配置 Wi-Fi

判断提示：

- 当前项目已落地 runtime `STANDBY`，但未落地自动 Light Sleep / Deep Sleep。

### `runtime_coordinator`

定位：

- 跨 owner 协调协议 owner

负责：

- participant 注册
- transition generation、deadline、ACK
- 发布当前强前台 owner 事实

不负责：

- 直接 stop/deinit 业务资源
- 包含业务 service 头文件或解释业务状态
- 变成资源管家或通用任务调度器

### `safety_monitor_policy`

定位：

- Safety Monitor 用户开关和目标态 owner

负责：

- 保存用户意图
- 响应 power budget、音频 session 与 coordinator 变化
- 合成 `should_run`，通知 `safety_monitor_session`

不负责：

- 模型 runtime、提醒策略、音频仲裁或通用任务调度

### `safety_monitor_session`

定位：

- Safety Monitor 长期运行生命周期 owner

负责：

- 把 `should_run` 翻译成 start/stop、FAILED 恢复、退避和运行确认

不负责：

- UI 文案、power budget、风险状态或模型后处理

### `danger_detection_service`

定位：

- 危险识别风险状态和连续证据融合 owner

负责：

- `MONITORING / SUSPICIOUS / ALERTING / COOLDOWN`
- confirm/clear/hold/cooldown 等产品语义

不负责：

- 页面生命周期、后台授权、提醒动作编排或模型底层 runtime

### `app_alert_manager`

定位：

- 正式提醒动作编排 owner

负责：

- alert overlay、audio fallback、普通音频抢占等提醒动作

不负责：

- 模型阈值、连续窗口累计或 Safety Monitor 会话管理

## 快速 owner 判断

- 用户动作语义变化：先看 `network_manager`
- STA 连网/断网/自动重连问题：先看 `wifi_control`
- BLE / SoftAP transport 行为：先看 `network_provisioning_adapter`
- 门户前端资源、HTTP 路由、SoftAP 网页接缝：先看 `ap_portal_adapter`
- 旧接口兼容或 service-ready 探测：才看 `network_service`
- 页面交互、按钮、提示、状态展示：看 `main/ui`
- 整机低功耗预算：看 `power_policy`
- UI 空闲、降刷新、runtime `STANDBY`：看 `ui_refresh_policy`
- sleep readiness dry-run：看 `sleep_coordinator`
- 跨 owner 让路、交接或强前台事实：看 `runtime_coordinator`
- 后台安全监听开关和目标态：看 `safety_monitor_policy`
- Safety Monitor start/stop/retry：看 `safety_monitor_session`
- 危险风险状态和连续证据：看 `danger_detection_service`
- 提醒编排：看 `app_alert_manager`
