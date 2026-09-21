# 面向当前项目的新手学习路径

目标不是一次看完全部代码，而是先建立“边界感”。

## 第 1 步：先认识模块地图

先读：

- `docs/context/knowledge/project/repo-overview.md`

要回答的问题：

- 当前项目有哪些主要模块
- UI、服务、组件分别在哪里
- 网络主链路大致经过哪些层

这一阶段不要急着读细节实现。

## 第 2 步：先理解网络主线为什么这样分层

先读：

- `docs/context/knowledge/project/network-provisioning-custom-upper-architecture.md`

要回答的问题：

- 为什么旧 `wifi_provision` 不再适合做长期主线
- 为什么拆成 `network_manager / wifi_control / network_provisioning_adapter / ap_portal_adapter`
- 当前谁是网络 owner

## 第 3 步：再看 UI 语义和产品动作

先读：

- `docs/context/knowledge/project/wifi-management-ui-behavior.md`

要回答的问题：

- 主界面 Wi-Fi / Bluetooth 图标真实表达什么
- Wi-Fi 管理页的动作是谁处理的
- 为什么 UI 不直接控制底层 Wi-Fi

## 第 4 步：再学状态机和时序

先读：

- `docs/context/knowledge/project/softap-captive-portal-auto-popup.md`
- 当前配网相关日志和上下文卡

要回答的问题：

- 配网为什么是状态机问题
- 为什么“AP 能起来”不等于“配网真的成功”
- 为什么成功后页面断开不一定是失败

## 第 5 步：最后再学习怎么做架构级改动

这时再练习下面几类问题：

- 新功能应该落在哪层
- 某个模块是不是越权了
- 该补接口还是该新建模块
- 这次修改怎么验证才算真的对

推荐练习题：

- “给 Wi-Fi 管理页加一个新动作，应该落在哪层？”
- “一个联网问题为什么该改 `network_manager` 而不是 `wifi_control`？”
- “一个门户页面问题为什么不该先改 `network_service`？”

## 学习时的默认心法

- 先学 owner，不先学细节
- 先学边界，不先学技巧
- 先学验证，不先学优化
- 先能回答“为什么落在这层”，再去写代码
