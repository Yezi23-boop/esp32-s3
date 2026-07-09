#pragma once

#include <string>

#include <nvs_flash.h>

namespace official_chat {

/**
 * @brief NVS KV 数据存储与读取助手类。
 *
 * 封装 ESP-IDF NVS 底层机制，将键值读写封装成 C++ string/int/bool 高层操作，
 * 默认遵循延迟或析构时 Commit 策略。该类实例不包含跨线程防护，应在一处作用域内用完即释放。
 */
class Settings {
 public:
  /**
   * @brief 初始化指定命名空间的 NVS 配置读写句柄。
   *
   * @param[in] ns 需要打开/建表的命名空间，如 "mqtt", "websocket" 隔离不同业务的 Key。
   * @param[in] read_write true 表示以可写方式挂载；false 只读（写行为会被静默丢弃）。
   */
  explicit Settings(const std::string &ns, bool read_write = false);

  /**
   * @brief 自动析构。如果处于可写模式下且发生了修改(dirty)，则默认在析构时提交 (Commit) 给 Flash。
   */
  ~Settings();

  /**
   * @brief 从 NVS 获取长字符串参数。
   * @param[in] key 用于查表的 NVS Key。受限于 NVS，Key 不能超过 15 字节。
   * @param[in] default_value 当键值丢失或无值时给出的兜底项，避免外层处理空串崩溃。
   * @return 拉取出的字符副本；如果读取发生 IO 异常，同样返回兜底值。
   */
  std::string GetString(const std::string &key,
                        const std::string &default_value = "") const;

  /**
   * @brief 定向写入字符串，但并不会立即进行物理擦写。
   */
  void SetString(const std::string &key, const std::string &value);

  /**
   * @brief 读取持久化的整型参数，通常为配置数字如心跳周期。
   */
  int32_t GetInt(const std::string &key, int32_t default_value = 0) const;
  void SetInt(const std::string &key, int32_t value);

  /**
   * @brief 读取开关或标志位，例如 AEC 使能记录。
   */
  bool GetBool(const std::string &key, bool default_value = false) const;
  void SetBool(const std::string &key, bool value);

  /**
   * @brief 从该命名空间内定向删掉某个配置。
   */
  void EraseKey(const std::string &key);

  /**
   * @brief 擦除该命名空间下所有的键值对；通常在恢复出厂设置或覆盖重载配网凭据时使用。
   */
  void EraseAll();

 private:
  std::string ns_;
  nvs_handle_t nvs_handle_ = 0;
  bool read_write_ = false;
  bool dirty_ = false; /**< 表示内存写改动尚未 commit 到 Flash。在析构或特定情况下触发 IO。 */
};

}  // namespace official_chat
