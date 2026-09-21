# 外部工程原则

本文件只收“为什么这样分层”的权威依据，不描述当前仓库实现细节。

## 参考来源

- ESP-IDF Hardware Abstraction  
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/hardware-abstraction.html
- Zephyr Introduction  
  https://docs.zephyrproject.org/latest/introduction/index.html
- Zephyr Device Driver Model  
  https://docs.zephyrproject.org/latest/kernel/drivers/index.html
- FreeRTOS kernel / portable layer manual  
  https://www.freertos.org/media/2018/161204_Mastering_the_FreeRTOS_Real_Time_Kernel-A_Hands-On_Tutorial_Guide.pdf
- FreeRTOS 官方论坛 HAL/BSP 讨论  
  https://forums.freertos.org/t/porting-amazon-freertos-to-different-stmicroelectronics-board-ucontroller/8919
- Arm Cortex-M 软件架构白皮书  
  https://assets.markallengroup.com/article-images/231736/Software%20Architecture%20for%20Arm%20Cortex-M%20Microcontrollers.pdf

## 1. 分层要能回答“谁负责什么”

嵌入式框架里的分层，不是为了图好看，而是为了让每一层都能回答：

- 它负责什么
- 它不负责什么
- 谁可以调用它
- 它依赖谁

如果一个模块同时处理“硬件细节 + 产品策略 + 页面语义”，通常就是边界过重的信号。

## 2. HAL / Driver / Service / App 不应混在一起

从 ESP-IDF、Zephyr、Arm 的常见分层可以抽出一条稳定原则：

- `HAL / Driver`：靠近硬件，负责设备能力与寄存器/总线抽象
- `Runtime control / Service`：负责某类能力的运行时控制与状态管理
- `Manager / Orchestrator`：负责产品动作编排与跨模块协作
- `UI / App`：负责用户意图与业务语义

判断上最实用的一条是：

- 底层层级越低，越不应该知道页面和产品动作语义
- 上层层级越高，越不应该直接碰底层设备细节

## 3. owner-first：先找 owner，再谈实现

一个成熟嵌入式项目最重要的架构判断，不是“怎么写”，而是“谁来负责”。

owner-first 的好处：

- 问题更容易定位
- 状态更容易收敛
- 新需求不容易越层
- 回归面更可控

如果没有先锁 owner，就很容易出现：

- UI 偷偷做底层逻辑
- runtime control 承担产品策略
- 兼容 shim 继续演化成新主线

## 4. 状态机比 if/else 堆叠更重要

联网、配网、音频、低功耗这类功能，本质上都是异步系统。

官方和行业资料都强调：

- 要把系统看成状态迁移，而不是散落的函数调用
- 要定义谁负责状态，谁负责事件，谁只消费结果

对项目判断来说，这意味着：

- 如果问题本质是“状态如何迁移”，优先找状态 owner
- 如果问题本质是“某个动作怎么执行”，再找具体实现层

## 5. 资源约束会反过来影响架构

Zephyr 和 FreeRTOS 的文档都在强调一点：嵌入式架构不只是抽象问题，还要考虑：

- RAM / Flash
- 任务与栈
- 队列与缓冲区
- 中断上下文
- 时序与功耗

因此一个“理论上更优雅”的分层，如果会让资源生命周期更混乱，未必就是更好的嵌入式架构。

## 6. 抽象要服务于可验证性

好的抽象不是把所有细节藏起来，而是让验证路径更清楚。

一个模块如果设计得好，通常能做到：

- 日志和错误码更明确
- 构建和单测能聚焦到该层
- 真机问题更容易缩小范围

因此判断某个改动是不是“架构上更好”时，一个很实用的标准是：

- 改完之后，这类问题会更容易定位吗？
- 改完之后，责任边界会更清楚吗？

## 7. 对新手最重要的不是“学很多”，而是“先学会归类”

学习嵌入式框架时，优先掌握这 4 个问题：

1. 这是哪一层的问题？
2. 谁是 owner？
3. 哪一层不应该动？
4. 这次怎么验证我的判断？

如果这 4 个问题能稳定回答，后面的实现细节会容易很多。
