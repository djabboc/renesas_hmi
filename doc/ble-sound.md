# ble-sound：Apple Music 播放信息可行性说明

评估日期：2026-10-01。目标是手机蓝牙直连后，在 HMI 屏幕显示当前播放内容。当前未创建 `src/ble-sound.c`，未完成手机媒体协议实测。

## 结论

本工程当前不能交付已经验证的 Apple Music 显示例程。RW007 现有公开接口没有提供经典蓝牙 A2DP/AVRCP 功能，但这不代表“只显示歌曲信息”一定无法实现。

如果手机是 iPhone，Apple Media Service（AMS）提供基于 BLE 的媒体信息访问方式，不要求通过 A2DP 接收音频。RW007 已公开部分 GATT 客户端读写、通知订阅接口，存在值得验证的协议路径；其连接、配对与安全处理、服务可用性和订阅能否满足真实 iPhone 的要求，本轮没有验证。

因此本项状态为“文档评估完成，功能未实现，AMS 路径待验证”，不能写成“RW007 硬件绝对不可能显示 Apple Music”。用户本轮要求归档限制，本轮不开展新一轮手机交互实验。

## 音频与播放信息需要不同协议

| 目标 | 常见协议路径 | 对当前工程的判断 |
| --- | --- | --- |
| 像蓝牙音箱一样接收手机音乐 | 经典蓝牙 A2DP Sink；还需要解码及音频输出链路 | 当前公开接口未提供这条路径，本工程也未实现音频链路 |
| 从经典蓝牙连接获取歌曲信息、控制播放 | AVRCP，需对端和模块支持对应元数据/控制功能 | A2DP 本身不能替代 AVRCP；当前 RW007 接口未提供 AVRCP |
| 通过 BLE 读取 iPhone 当前媒体信息 | Apple AMS，配件充当 GATT 客户端 | 不需要 A2DP；理论协议路径存在，RW007 实机兼容性待验证 |
| 通过 BLE 读取 Android 上 Apple Music 信息 | 需另行确认系统/应用提供的服务，或开发手机侧转发程序 | 不能把 iOS 的 AMS 直接套用到 Android；本轮未评估具体手机方案 |

没有音频接收需求时，不应仅因缺少 A2DP 就否定媒体信息显示。反过来，支持 BLE 广播和一个 write 特性，也不能说明已具备媒体协议。

## AMS 提供什么

Apple 官方资料说明，iOS 设备发布 AMS，配件作为 GATT 客户端访问。这里的 GATT 客户端/服务端角色，与谁发起 BLE 连接的角色是不同概念，不能仅凭模块“外围模式”或“主机模式”的名字认定可用性。

AMS 服务 UUID 为 `89D3502B-0F36-433A-8EF4-C502AD55F8DC`，定义了以下信息与操作：

- Player：当前媒体应用、播放状态、音量等。
- Track：标题、艺术家、时长等。
- Queue：播放队列及随机、循环等状态。
- Remote Command：在手机当前播放器支持时请求播放、暂停、切歌等。

配件需要订阅 Entity Update，再写入想接收的实体/属性列表，才能获得初始值和后续变化。字符串为 UTF-8；被通知长度截断的属性需要通过 Entity Attribute 进一步读取。当前默认 LVGL 字体也不能据此假定已能显示所有中文歌曲名，若实施需一并处理字体和文字布局。

AMS 反映 iOS 当前活动媒体播放器的信息，不专属于 Apple Music，也不是音乐库查询或封面下载接口。Apple 明确指出 AMS 不保证始终存在，应关注 GATT Service Changed，以处理服务出现或消失。该资料为 Apple 官方归档规范，具体 iOS 版本的行为仍需实机确认。

## 为什么 FF01 测试不能否定 AMS

此次手机向模块 FF01 写入 `HMI_PROBE1`，验证的是“手机写本地服务，模块是否转发给 RA6M3”。测试未观察到转发，且官方说明当前外围功能仅支持配网。

AMS 的主要数据路径不同：iPhone 提供服务，模块作为 GATT 客户端访问远端特性，订阅手机通知并将事件交给主控。官方 RW007 接口已有按 UUID 读写远端特性及订阅通知的部分能力，这与缺失本地通用 GATT 服务端接口并不矛盾。

不过，接口存在不等于端到端已经可用。若后续恢复开发，需要依次验证：

1. 使用的手机是否为 iPhone，目标 iOS 版本是否提供可访问的 AMS。
2. RW007 是否能建立所需连接，以及满足实际连接中的配对、加密、授权和重连要求；目前公开接口中没有确认完整可控的安全配置流程。
3. 是否能正确访问 AMS 的 128 位 UUID、订阅通知、请求属性并把返回内容交给 RA6M3。
4. 是否能处理 AMS 发布/移除、通知截断、重新订阅、UTF-8 和中文显示。
5. 在真实 Apple Music 播放、暂停、切歌及断线重连时，屏幕信息是否正确更新。

其中任何模块固件能力缺口，都可能要求厂商提供扩展固件，或改用开放 BLE GATT 客户端和安全配置能力的模块。此时应根据所缺能力选型，不能认为任意“BLE 串口透传模块”都能支持 AMS。

## 当前任务处理

- 遥控和 ECHO：现有公开服务端收发接口不足，限制及手机诊断已归档，见 [RW007 能力说明](ble-capabilities.md)。
- ble-sound：保留本说明，不创建占位例程；不标记为媒体功能验收通过，也不宣称完成 A2DP、AVRCP 或 AMS 板测。
- 当前仍仅启用 `ble.c` 的初始化入口。文档归档不会改变板上例程或 RW007 模块固件。

## 参考资料

- [Apple Media Service 简介](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Introduction/Introduction.html)：BLE 链路、GATT 客户端角色、编码。
- [Apple Media Service 规范](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Specification/Specification.html)：服务及特性 UUID、媒体属性、订阅流程、服务可用性限制。
- [Bluetooth SIG：A2DP 1.4](https://www.bluetooth.com/specifications/specs/advanced-audio-distribution-profile-1-4/)：音频分发协议。
- [RW007 官方说明](https://github.com/RT-Thread-packages/rw007/blob/94df57f856bc2e8661022a379aa1c3d6a3bc5149/README_ZH.md)：现有 BLE 主机接口和外围配网限制。
