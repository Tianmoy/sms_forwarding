# 低成本短信转发器

> 当前分支为新方案，2022年的老方案请前往[luatos分支](https://github.com/chenxuuu/sms_forwarding/tree/old-luatos)。  
本项目**仅用于接收短信**与进行保号相关功能。  
当前重构版额外支持兼容 eUICC 的卡功能列表与人工切换；不提供通话、拨号或自动切卡。

[后台页面演示](https://sms.j2.cx/)

本项目旨在使用低成本的硬件设备，实现短信的自动转发功能，支持多种推送方式同时启用。

> 视频教程：[B站视频](https://www.bilibili.com/video/BV1cSmABYEiX)

<img src="assets/photo.png" width="200" />

## 功能

- 支持使用通用AT指令与模块进行通信
- 全新响应式 WEB 管理台，带 Cookie 会话登录、CSRF 防护与登录限流
- 实时查看设备状态与短信收件箱，最近 50 条短信持久化保存在 LittleFS
- 每条短信分别标明发送方与接收卡/Profile；新记录使用脱敏 ICCID 尾号区分同名 eSIM Profile
- 展示 SIM 拔出、插入、PIN/PUK 锁定状态；换卡后可在网页手动重新上电识别
- 支持短信搜索、未读、详情、标记已读、单条删除与全部清空
- 支持读取 eUICC 卡功能列表，并在二次确认后切换 Profile
- 支持查看 eUICC EID，并删除未启用的 Profile（二次确认，不可恢复）
- 支持异步扫描运营商、从扫描白名单中手动选网以及恢复自动选网
- **支持多达5个推送通道同时启用**，每个通道可独立配置
- 支持将收到的短信转发到指定的邮箱
- 支持长短信自动合并（30秒超时）
- 支持管理员短信远程发送短信和重启设备

## 管理台与安全

- 默认地址为设备从路由器获得的局域网 IP，例如 `http://192.168.x.x/`。
- 初始账号为 `admin`，初始密码为 `admin123`。首次登录后请立即在“系统设置”中修改密码。
- 管理接口只返回脱敏配置；推送密钥和 SMTP 密码不会回显。
- 当前设备使用局域网 HTTP，请不要直接映射到公网。需要远程访问时应在可信 VPN 后使用。

## SIM 热插拔说明

固件通过标准 `AT+CPIN?` 非阻塞轮询判断 SIM 状态，并连续确认两次后更新网页。拔卡时会撤销“已就绪”状态，清空旧卡功能、运营商和短信接收缓存。

部分 ML307R 核心板或卡槽不会在运行中可靠识别新插入的卡。换卡后请打开“SIM / eSIM”，点击“重新识别 SIM”；设备会重新上电 4G 模组，约 30–60 秒后恢复短信配置和网络注册。当前接线未将 ML307R 的 `USIM_DET` 引脚接到 ESP32，因此该按钮是换卡后的可靠操作入口；运营商扫描或 eSIM 切换期间请等待任务结束后再执行。

## eSIM / eUICC 说明

卡功能管理使用 GSMA SGP.22 ES10c，通过 APDU 读取及启用 Profile。通道优先使用原生逻辑通道（AT+CCHO/CGLA），失败时自动降级为 CSIM 手动逻辑通道，再降级为 CSIM 基础通道，兼容锁死 CCHO 的模组固件。后端只接受刚从 eUICC 枚举得到的 Profile AID，不开放任意切卡 APDU。EID 查询与 Profile 删除同样只接受枚举结果；正在启用的 Profile 不允许删除。切换会重启 4G 模组、恢复自动选网并等待完整注册结果，短信与蜂窝网络通常中断 30–180 秒，漫游注册通常需要 1–5 分钟，异常恢复最长约 8 分钟；任务状态会持久化，并在重启后继续核对实际结果。

此功能只适用于能返回标准 ISD-R Profile 列表的 eUICC。普通实体 SIM 不会凭空获得多 Profile 能力。建议先刷新列表确认目标卡功能，再由管理员手工切换。

## 运营商选网说明

运营商选网与 APN、eSIM Profile 是三种不同功能。网页通过 `AT+COPS=?` 扫描当前 Profile 可见的 PLMN，只允许选择本次扫描中状态为“可用”的数字 PLMN，并使用带自动回退的手动模式；恢复自动选网使用 `AT+COPS=0`。扫描到网络不代表漫游协议一定允许注册，最终以 `AT+CEREG?` 返回本地注册或漫游注册为成功条件。

一次完整扫描或手动注册可能耗时 1–3 分钟，期间蜂窝网络和短信接收可能暂时中断。页面会立即返回后台任务并持续显示进度，不会用长 HTTP 请求阻塞管理台。任何时候都可在任务结束后选择“恢复自动选网”。

> **现场踩坑：** `AT+CEREG?` 显示已注册，只能证明 EPS 网络注册成功，不能证明 MT 短信可达。2026-07-12 实测 3HK Profile 自动驻留 `46011` 时，ML307R-DC 无法收到短信；手动切换到中国联通 `46001` 后立即恢复。完整证据、判断方法和防复发事项见[排障记录](dev_doc/troubleshooting.md)。

## 推送通道支持

支持以下10种推送方式，可同时启用多个通道：

| 推送方式 | 说明 | 需要配置 |
|---------|------|---------|
| **POST JSON** | 通用HTTP POST | URL |
| **Bark** | iOS推送服务 | Bark服务器URL |
| **GET请求** | URL参数方式 | URL |
| **钉钉机器人** | 企业群通知 | Webhook URL，可选Secret加签 |
| **PushPlus** | 微信公众号推送 | Token |
| **Server酱** | 微信推送服务 | SendKey |
| **自定义模板** | 灵活的JSON模板 | URL + 请求体模板 |
| **飞书机器人** | 自定义通知 | Webhook URL |
| **Gotify** | 自建推送服务 | 服务地址 + Token |
| **Telegram Bot** | Telegram 通知 | Bot Token + Chat ID |

### 推送格式说明

- **POST JSON**: `{"sender":"发送者号码","message":"短信内容","timestamp":"时间戳"}`
- **Bark**: `{"title":"发送者号码","body":"短信内容"}`
- **GET请求**: `URL?sender=xxx&message=xxx&timestamp=xxx`（自动URL编码）
- **钉钉机器人**: 文本消息格式，支持加签验证
- **PushPlus**: 使用Token推送，支持HTML格式
- **Server酱**: 使用SendKey推送，支持Markdown格式
- **自定义模板**: 使用`{sender}`、`{message}`、`{timestamp}`占位符
- **飞书机器人**: 文本消息格式，支持加签验证

|状态信息|主动ping|
|-|-|
|![](assets/status.png)|![](assets/ping.png)|

## 硬件搭配

若没有焊接能力，希望直接使用成品，可选直接购以下套件（我看过了，和自己做的成本一样）  
支持**移动/联通/电信卡**：

- [小蓝鲸WIFI短信宝](https://item.taobao.com/item.htm?id=1003711355912)（找客服问）
- [4G FPC天线](https://item.taobao.com/item.htm?id=1003711355912&skuId=6162872574943)，与开发板同购

如果希望自行焊接硬件，参考下面的硬件搭配，总成本约¥27.8（会有浮动，可按实际自行组合搭配）  
仅支持**移动/联通卡**：

- ESP32C3开发板，实测选用[ESP32C3 Super Mini](https://item.taobao.com/item.htm?id=852057780489&skuId=5813710390565)，¥9.5包邮
- ML307R-DC开发板，实测选用[小蓝鲸ML307R-DC核心板](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥16.3包邮
- [4G FPC天线](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥2，与核心板同购


## 硬件连接

ESP32C3 与 ML307R-DC 通过串口（UART）连接，接线如下：

```
┌───────────────────────────────────────────────┐
|                                               |
|   ESP32C3 Super Mini      ML307R-DC核心板     |
| ┌───────────────────┐    ┌─────────────────┐ |
└─┼─ GPIO5 (MODEM_EN) │    │                 │ |
  │       GPIO3 (TX) ─┼───►│ RX              │ |
  │                   │    │             EN ─┼─┘
  │       GPIO4 (RX) ◄┼────┤ TX              │ 
  │                   │    │                 │ 
  │              GND ─┼────┤ GND             │ 
  │                   │    │                 │ 
  │               5V ─┼────┤ VCC (5V)        |
  │                   │    │                 │
  └───────────────────┘    └─────────────────┘
                           │                 │
                           │  SIM卡槽        │
                           │  (插入Nano SIM) │
                           │                 │
                           │  天线接口       │
                           │  (连接4G天线)   │
                           └─────────────────┘
```

可通过USB连接ESP32C3进行编程和供电，正常工作时，可通过网页与模组进行AT通信，方便调试。

## 软件组成

- ESP32C3运行自己的`Arduino`固件，负责连接WiFi和接收ML307R-DC发送过来的短信数据，然后转发到指定HTTP接口或邮箱
- ML307R-DC运行默认的AT固件，不用动

需要在`Arduino IDE`中单独安装这些库：

- **ReadyMail** by Mobizt
- **pdulib** by David Henry

需要在`Arduino IDE`中安装ESP32开发板支持，参考[官方文档](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)，版型选`MakerGO ESP32 C3 SuperMini`。

重构版的嵌入式管理台超过默认应用分区容量，4MB ESP32-C3 请使用 `PartitionScheme=no_ota`（2MB 应用 + 2MB LittleFS）编译。当前固件不包含 OTA 更新功能。

Wi-Fi 通过网页配置（系统设置 → WiFi 网络），支持 1 主用 + 3 备用，无需编译期凭据。首次烧录或全部网络连接失败时，设备自动开启开放配置热点 `sms-forwarder`，连接后访问 `http://192.168.4.1` 完成配置。
