#pragma once

#include "device_state.h"

namespace official_chat {

/**
 * @brief 设备状态机追踪和流转管理器。
 *
 * 负责记录设备当前的核心交互状态，并拦截非法的并行状态流转。
 * 它的修改（TransitionTo）应当仅发生在一个被互斥锁保护的环境下或单线程主循环中，
 * 以确保不同异步事件（比如强制停机与网络返回结果）同时到达时能够正确取舍。
 */
class DeviceStateMachine {
 public:
  DeviceStateMachine() = default;

  /**
   * @brief 在状态许可的前提下迁移到新阶段。
   *
   * 在这里实现合规检查，例如 kUpgrading 期间是否禁止打断、或是否允许直接从 kConnecting 跃迁到 kSpeaking。
   *
   * @param[in] state 即将迁入的目标新状态。
   * @return true 表示顺利接纳并切换状态；false 表示该迁移被约束条件否决，未发生改变。
   */
  bool TransitionTo(DeviceState state);

  /**
   * @brief 获取当前确切机器状态。
   *
   * @return 当前状态（如果跨线程访问，务必确认是否有并发改写的风险）。
   */
  DeviceState GetState() const;

 private:
  DeviceState state_ = DeviceState::kUnknown;  /**< 保存的状态快照，默认为初始/未知模式。 */
};

}  // namespace official_chat
