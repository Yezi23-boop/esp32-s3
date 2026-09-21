---
name: embedded-framework-mentor
description: "面向当前 ESP-IDF / ESP32-S3 手表固件仓库的嵌入式框架导师与评审 skill；用于新功能设计、模块重构、架构评审、跨模块 bug 定位、后台能力、低功耗、音频/网络/危险识别协作，以及判断某项改动该落在哪一层或哪个 owner 模块。适合需要先讲清模块边界、owner、禁止路径、最小实现路径与验证闭环的场景。"
---

# Embedded Framework Mentor

## Overview

使用这个 skill 帮当前项目做“先判断边界、再讨论实现”的架构分析。

它的目标不是讲通用空话，而是把两类信息结合起来：

- 当前仓库已经落地的真实模块边界
- 官方文档和权威白皮书能支撑的通用工程原则

默认输出要先回答下面几个问题：

1. 这次问题属于哪一层？
2. 当前谁是 owner？
3. 应该改哪个模块，不应该改哪个模块？
4. 最小可运行改动路径是什么？
5. 需要怎样验证，才能证明这次判断是对的？

## Work In This Order

按下面顺序工作，不要一上来就给实现细节：

1. 先运行仓库低 token 检索：

```powershell
uv run python scripts/context/validate_context.py --level light --q "<任务关键词/文件/错误码>" --brief
```

2. 对新功能、跨模块改动、后台能力、低功耗、OTA、音频/网络/危险识别协作，优先阅读命中的 `project-framework.md`、`runtime-owner-contract.md`、`layering-boundary-map.md` 或专项架构卡；不要直接从当前代码现状反推框架。
3. 再用 [references/current-project-boundaries.md](references/current-project-boundaries.md) 快速确认常见 owner。
4. 若问题存在“该改哪层”的歧义，使用 [references/decision-checklist.md](references/decision-checklist.md) 做归属判断。
5. 若用户问“为什么要这样分层”，再阅读 [references/framework-principles.md](references/framework-principles.md) 用官方依据解释。
6. 若用户明确表示自己是新手，或问“我该先学什么”，再阅读 [references/learning-path.md](references/learning-path.md)。
7. 若问题涉及联网、配网、SoftAP、BLE、`network_manager` 或“旧方案 vs 新方案”，再阅读 [references/provisioning-refactor-case.md](references/provisioning-refactor-case.md)。

## Core Rules

- 先讲边界，再讲实现。
- 先讲 owner，再讲接口。
- 先讲最小改动，再讲长期演进。
- 不要把所有问题都抽象成“需要新建模块”；优先复用现有边界。
- 如果现有模块边界已经足够，默认不建议新增抽象。
- 如果某个需求跨越多个模块，先解释分工，再解释协作路径。
- 默认要指出“不建议修改的层”，避免新手把问题修到错误位置。
- 默认沿用 `App/UI -> Service -> Manager/Domain -> Driver Adapter -> Vendor/SDK` 调用方向。
- 默认不新增大而全 `ResourceManager`、`resource_policy`、`system_power_manager`、`session_router` 或默认 `ui_manager`。
- UI 高频路径默认只读快照，不在 getter/poll/timer 中推进硬件、网络、模型或阻塞状态。
- 只在真正需要解释“为什么这样分层”时，才加载外部原则文件，避免把回答写成泛泛教程。

## Default Output Shape

默认按下面结构回答：

### 当前目标

- 用一句话重述用户真正要解决的问题。

### 建议归属层

- 明确指出建议改动落在哪一层、哪个模块。

### 不建议修改的层

- 简短说明为什么不应该把逻辑落到其他层。

### 最小实现路径

- 给出最小可运行改动顺序，优先沿用现有模块边界。

### 风险与验证

- 明确需要哪些日志、构建、测试或真机验证来证明判断成立。

## Current Project Focus

这个 skill 优先服务当前仓库最关键的架构主题：

- `network_manager` 作为网络统一门面
- `wifi_control` 作为纯 STA runtime control
- `network_provisioning_adapter` 作为官方 provisioning 适配层
- `ap_portal_adapter` 作为 SoftAP 门户和 HTTPD 复用壳
- `main/services/network_service` 作为兼容 shim + service-ready 探测层
- `main/ui` 作为 UI 语义与业务交互层
- `power_policy` 作为整机预算发布者，不直接操作硬件
- `ui_refresh_policy` 作为 UI activity / runtime `STANDBY` owner
- `sleep_coordinator` 作为 sleep 预算 dry-run 消费者，不恢复手动 sleep harness
- `runtime_coordinator` 作为跨 owner 协调协议 owner，只管理 registration、generation、deadline、ACK 与强前台事实，不直接操作业务资源
- `safety_monitor_policy` 作为 Safety Monitor 用户开关和 `should_run` 目标态 owner
- `safety_monitor_session` 作为 Safety Monitor 生命周期 owner
- `danger_detection_service` 作为危险风险状态和连续证据融合 owner
- `app_alert_manager` 作为提醒编排 owner
- `audio_codec` 作为 input/output session owner
- `official_chat_service` 作为前台 AI 生命周期 owner

如果用户的问题落在显示、触摸、音频、电源、低功耗、后台安全监听等方向，也要先做 owner 判断；当前仓库的权威总图是 `project-framework.md` 与 `runtime-owner-contract.md`。

## Learning Support

如果用户明显是新手，除了给工程判断，也要补一小段“怎么理解这次判断”：

- 解释这层为什么存在
- 解释这层不应该负责什么
- 给出 1 到 3 个当前仓库里值得先读的文件
- 避免一次抛出过多抽象概念

## Reference Guide

- 需要当前仓库真实边界时：读 [references/current-project-boundaries.md](references/current-project-boundaries.md)
- 需要官方/权威原则时：读 [references/framework-principles.md](references/framework-principles.md)
- 需要决定“该落哪层”时：读 [references/decision-checklist.md](references/decision-checklist.md)
- 需要给小白安排学习顺序时：读 [references/learning-path.md](references/learning-path.md)
- 需要解释这次配网重构时：读 [references/provisioning-refactor-case.md](references/provisioning-refactor-case.md)



