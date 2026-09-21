# 架构判断清单

当用户问“这次改动该落在哪层”时，按这个顺序判断。

## Step 1: 先识别问题类别

先把问题归到下面 5 类之一：

1. 驱动 / 外设问题  
   例如 GPIO、I2C、SPI、Wi-Fi/BLE 底层承载、DMA、时序。
2. 运行时控制问题  
   例如连接、断开、重连、power save、状态查询。
3. 编排问题  
   例如 latest 失败后该做什么、什么时候从 BLE 切到 SoftAP、什么时候停 transport。
4. UI 语义问题  
   例如按钮该显示什么、哪个入口表达什么语义、提示文案与时序。
5. 产品策略问题  
   例如默认行为、优先级、用户体验、回退策略。
6. 资源预算 / 低功耗问题
   例如 `STANDBY`、Wi-Fi power save、sleep permission、blocker、CPU/后台预算。
7. 长期后台能力问题
   例如 Safety Monitor、危险识别后台运行、前台 AI 音频与后台麦克风冲突。

如果先分不清类别，就不要直接建议改代码。

## Step 2: 找 owner

用“谁天然应该维护这类状态/动作”来找 owner：

- 驱动 / transport 承载：优先看底层 adapter 或 driver
- 运行时控制：优先看 control 层
- 编排：优先看 manager
- UI 语义：优先看 UI/controller
- 产品策略：先找 manager 或服务编排层
- 资源预算 / 低功耗：先看 `power_policy`，具体动作回到资源 owner
- sleep readiness：先看 `sleep_coordinator` dry-run，不要先恢复手动 sleep harness
- 后台能力目标态：先看 `background_service_manager`
- 后台能力生命周期：先看具体 session owner，例如 `safety_monitor_session`
- 危险识别风险语义：先看 `danger_detection_service`
- 提醒动作：先看 `app_alert_manager`

## Step 3: 先排除不该改的层

在给出建议前，至少指出 1 到 2 个“不建议修改”的层。

常见排除规则：

- UI 不直接做底层 runtime control
- runtime control 不直接做产品策略
- adapter 不直接承接 UI 语义
- shim 不继续演化成新主逻辑
- `power_policy` 不直接操作硬件、LVGL、Wi-Fi、音频、模型或 ESP sleep API
- `sleep_coordinator` 不逐个理解各 owner 内部状态，不手动断网/重连维持 Wi-Fi
- `background_service_manager` 不变成模型 owner、提醒策略 owner、音频仲裁器或通用任务调度器
- UI 不直接 start/stop 长期后台 runtime

## Step 4: 判断是否能在现有边界内解决

默认优先级：

1. 在现有模块内修实现
2. 在现有模块内补最小接口
3. 仅当现有边界已经明显不成立时，才新增模块或抽象

如果问题只是“某层没暴露需要的信息”，优先补接口，不要先新建模块。

## Step 5: 明确验证闭环

每次都要说清至少一类验证：

- 静态验证：源码检查、结构检查
- 构建验证：`idf.py build`
- 行为验证：日志、单测
- 真机验证：联网、UI、音频、低功耗等实际链路

没有验证闭环的架构判断，只能算假设。

## 当前项目速用版

- “这个按钮做什么”  
  先看 `main/ui`
- “用户动作之后系统该怎么编排”  
  先看 `network_manager`
- “STA 到底怎么连、怎么断、怎么查状态”  
  先看 `wifi_control`
- “BLE / SoftAP provisioning transport 怎么起停”  
  先看 `network_provisioning_adapter`
- “SoftAP 门户和官方 `prov-*` 怎么接”  
  先看 `ap_portal_adapter`
- “STANDBY、预算、blocker 怎么算”
  先看 `power_policy`
- “屏幕空闲、低刷新、触摸唤醒”
  先看 `ui_refresh_policy`
- “什么时候能 Light Sleep / Deep Sleep”
  先看低功耗架构卡和 `sleep_coordinator` dry-run；当前不默认进入真实 sleep
- “安全监听后台该不该运行”
  先看 `background_service_manager`
- “危险识别 runtime 怎么起停恢复”
  先看 `safety_monitor_session`
- “危险状态为什么 ALERTING/COOLDOWN”
  先看 `danger_detection_service`
