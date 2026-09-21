# 配网重做案例

本文件用当前项目的配网重做，讲一个真实的架构演进案例。

## 旧方案的问题

旧思路更接近：

- `wifi_provision + network_service` 单体式演进

问题在于它容易把这些职责混在一起：

- BLE / SoftAP transport
- 部分 Wi-Fi runtime control
- 部分产品策略
- 部分 UI 语义

这种结构在早期能跑，但随着需求增长，会越来越难回答：

- 到底谁是网络 owner
- 某个问题到底该改哪层
- UI 为何能直接影响底层行为

## 新方案为什么成立

当前项目已经收敛到：

- 官方 `network_provisioning` 负责底层 provisioning 内核
- `network_manager` 负责编排与产品动作
- `wifi_control` 负责 STA runtime control
- `network_provisioning_adapter` 负责 BLE / SoftAP transport 适配
- `ap_portal_adapter` 负责 SoftAP 门户与 HTTPD 复用

这条路线的价值不是“更复杂”，而是：

- owner 更清楚
- 状态更清楚
- 问题更容易定位
- 新手更容易学会“这次该改哪层”

## 这次踩过的边界坑

下面这些问题都说明了“边界没想清楚，就会出非常具体的故障”：

### 1. SoftAP 起了，但门户主链路没真正工作

说明：

- AP 能起来，不等于 SoftAP 门户和 provisioning 主链路已经接好

启示：

- transport 生命周期和页面壳生命周期必须是同一条链上的问题

### 2. external HTTPD handle 传错一层指针会直接 panic

说明：

- adapter 层的接缝细节是架构的一部分，不只是实现细节

启示：

- 复用官方内核时，接缝层必须非常明确，不能靠猜

### 3. 只建了 STA netif，SoftAP official endpoint 会断链

说明：

- “能看到热点”不代表 AP 这层网络栈已经完整可用

启示：

- 嵌入式分层不仅是软件抽象，还要落到真实资源对象和底层承载

### 4. 成功后 AP 主动关闭，浏览器不能简单判失败

说明：

- UI 看到的“页面断开”可能是底层成功后的正常收尾

启示：

- UI 语义层不能脱离底层生命周期理解结果

## 这个案例能教会什么

如果你是新手，这次案例最值得学的不是某个 API，而是这 4 件事：

1. 旧方案为什么会变得难维护
2. 新方案为什么要先划 owner
3. 一个 bug 往往暴露的是边界问题，不只是实现错误
4. 好的架构不是更抽象，而是更容易定位、验证和演进
