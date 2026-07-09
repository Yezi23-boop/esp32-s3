#pragma once

namespace official_chat {

/**
 * @brief 设备内部核心运行状态枚举。
 *
 * 这是一个强类型枚举，统一管理组件生命周期以及网络交互环节。
 * 所有影响到输入（麦克风采集）或者输出（扬声器播放）的前置检查都依赖此状态；
 * 跨线程（特别是网络接收线程与主事件分发线程）读写状态时需保证一致性，
 * 避免网络协议层刚挂断时却进入了 kSpeaking 而发生状态泄露或竞争挂机。
 */
enum class DeviceState {
    kUnknown = 0, /**< 初始默认状态；此时组件各模块尚未安全装载，不可被操作。 */
    kActivating,  /**< 激活或鉴权流程进行中（如拉取 Token 或 WiFi 配网刚结束），期间屏蔽语音输入动作。 */
    kUpgrading,   /**< 设备处在 OTA 固件或必须的模型资源下载阶段。为保障闪存烧写安全，严禁启动音频并发任务。 */
    kIdle,        /**< 系统处于空闲待命，网络正常或在重连；此时可随时响应唤醒词或按键产生 kListening 或 kConnecting。 */
    kConnecting,  /**< 正在与 WebSocket/MQTT 端点建立真实长连接握手；该阶段易受弱网导致超时。 */
    kListening,   /**< 麦克风音频正在采集且上传网络。此时应将外部其他播放挂起（保证上行无串扰）。 */
    kSpeaking,    /**< 正在通过扬声器播放来自服务端的下行响应流；该状态通常会互斥或抑制本地其它提示音。 */
};

}  // namespace official_chat
