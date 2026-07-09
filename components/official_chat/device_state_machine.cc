#include "device_state_machine.h"

namespace official_chat {

bool DeviceStateMachine::TransitionTo(DeviceState state) {
  /*
   * 检查待定迁入状态是否与当前存在的状态完全一致。
   * 原因：状态发生跃跃往往触发大量的事件推演，比如 UI 图标切换，中断原有通信信道等。
   * 如果状态无变化则主动拦截，防止陷入不必要的资源重新拉起或雪崩式连锁回调。
   */
  if (state_ == state) {
    return false;
  }
  state_ = state;
  return true;
}

DeviceState DeviceStateMachine::GetState() const {
  return state_;
}

}  // namespace official_chat
