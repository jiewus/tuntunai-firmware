#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

/**
 * @file device_state.h
 * @brief 设备业务生命周期中的离散状态。
 */

/**
 * @brief Application 和显示/LED 共同使用的状态枚举。
 *
 * 状态之间不能任意跳转，合法路径由 DeviceStateMachine::IsValidTransition()
 * 集中定义。
 */
enum DeviceState {
    kDeviceStateUnknown,          ///< 尚未初始化状态机。
    kDeviceStateStarting,         ///< 正在初始化硬件和资源。
    kDeviceStateWifiConfiguring,  ///< 正在运行热点配网。
    kDeviceStateIdle,             ///< 云端就绪，等待唤醒或按键。
    kDeviceStateConnecting,       ///< 正在连接网络或云端协议。
    kDeviceStateListening,        ///< 正在采集并上传用户语音。
    kDeviceStateSpeaking,         ///< 正在播放云端 TTS 音频。
    kDeviceStateUpgrading,        ///< 正在下载或安装 OTA。
    kDeviceStateActivating,       ///< 正在执行设备激活。
    kDeviceStateAudioTesting,     ///< 正在进行录放音测试。
    kDeviceStateFatalError        ///< 不可恢复错误，只允许提示或重启。
};

#endif // _DEVICE_STATE_H_ 
