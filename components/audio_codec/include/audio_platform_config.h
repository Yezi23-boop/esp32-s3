#pragma once

/*
 * 音频平台静态参数
 * - HW_*: codec 与 I2S 总线工作的硬件采样参数
 * - SR_*: 语音识别链路使用的目标采样参数
 */

// 硬件链路默认采样率（Hz），影响 codec 与 I2S 总线初始化。
#define AUDIO_PLATFORM_HW_SAMPLE_RATE 24000
// 硬件链路样本位宽（bit）。
#define AUDIO_PLATFORM_HW_BITS_PER_SAMPLE 16
// 逻辑输入声道数（应用读取后看到的通道数）。
#define AUDIO_PLATFORM_HW_INPUT_CHANNELS 2
// 逻辑输出声道数（播放路径声道配置）。
#define AUDIO_PLATFORM_HW_OUTPUT_CHANNELS 1

// 语音识别侧目标采样率（Hz）。
#define AUDIO_PLATFORM_SR_SAMPLE_RATE 16000
// 语音识别侧样本位宽（bit）。
#define AUDIO_PLATFORM_SR_BITS_PER_SAMPLE 16

// ADC 逻辑通道格式标记（用于日志/上层策略识别）。
#define AUDIO_PLATFORM_ADC_CHANNEL_FORMAT "MR"
// 对话触发按钮 GPIO。
#define AUDIO_PLATFORM_CHAT_BUTTON_GPIO 0
