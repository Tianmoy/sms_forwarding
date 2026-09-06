# SMS FWD 固件 API 文档

> 适用固件:Tianmoy/sms_forwarding
> 设备地址:局域网 IP(路由器分配),配置热点模式:`192.168.4.1`

## 认证模型

| 项 | 说明 |
|---|---|
| 方式 | `Authorization: Bearer <token>` 请求头,两类令牌任选其一 |
| 会话令牌 | `POST /api/login` 返回的 `token`,8 小时绝对超时 / 30 分钟闲置超时,绑定来源 IP |
| API 令牌 | 设置页"API 令牌"生成/自定义,长期有效,适合定时任务访问本机或外部脚本 |
| 限流 | 登录每满 5 次失败锁定,时长 30s 起逐级翻倍至 15 分钟封顶,返回 429 + `retryAfter` |
| 错误码 | 401 未认证 / 405 写操作用 GET / 409 设备忙 / 429 限流或锁定 / 503 模组不可用 |

```bash
# 登录取会话令牌(把 192.168.1.100 换成设备实际 IP)
TOKEN=$(curl -s -X POST http://192.168.1.100/api/login \
  -d "username=admin" -d "password=******" | jq -r .token)

curl -H "Authorization: Bearer $TOKEN" http://192.168.1.100/api/status
curl -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.100/api/reboot
```

---

## 一、会话与设备操作

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/session` | — | 查询会话:`{"authenticated":true,"user":"admin","token":"...","mustChangePassword":false}` |
| POST | `/api/login` | `username` `password` | 登录:`{"ok":true,"token":"<32hex>","mustChangePassword":false}` |
| POST | `/api/logout` | — | 注销当前会话令牌 |
| GET | `/api/brand` | — | **公开**,无需认证:`{ok,title,sub}` |
| POST | `/api/sms/send` | `phone` `content` | 用设备卡发短信;号码仅允许数字与前置 `+`(3–20 位),内容非空 |
| GET/POST | `/api/reboot` | — | 整机重启(~40–60s) |
| GET/POST | `/api/factory/reset` | — | 恢复出厂:清 NVS + 短信,重启进配置热点 |
| POST | `/api/push/test` | `channel` | 单通道测试:`channel` = 0–9 通道编号或 `email`;目标未配置返回 400 |

## 二、状态与短信

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/status` | — | 总状态(结构见下) |
| GET | `/api/sms` | `rev`(可选) | 短信列表,最近 50 条持久化(LittleFS),满时淘汰最旧;带 `?rev=N` 且数据未变时省略 `messages` 数组(~50B) |
| POST | `/api/sms/read` | `id` | 标记已读 |
| POST | `/api/sms/delete` | `id` | 删除单条 |
| POST | `/api/sms/clear` | — | 清空全部 |

`/api/status`:
```json
{
  "ok":true,"uptime":3600,"heap":112640,"epoch":1788700000,
  "wifi":{"connected":true,"ssid":"Home","rssi":-52,"ip":"192.168.1.100"},
  "modem":{"ready":true,"model":"ML307R","operator":"中国联通","busy":false,
           "registration":"已注册","rsrp":-97,"rsrq":-9,
           "signal":{"known":true,"metric":"RSRP","dbm":-97,"rsrq":-9}},
  "sim":{"state":"ready","known":true,"present":true,"ready":true,"smsReady":true,
         "message":"SIM 已就绪","generation":3,"changedAt":91979,
         "profile":"eSIM.gg","phone":"+8613800138000","iccidTail":"0034",
         "operator":"中国联通","eid":"8904...32位"},
  "sms":{"stored":3,"unread":0,"capacity":50},
  "push":{"enabled":2},
  "job":{"active":false,"phase":"idle","progress":0,"message":""}
}
```

> `sim.phone` 来自 `AT+CNUM`,卡内未写入时为空;`sim.eid` 仅 eUICC 卡返回,普通 SIM 为空串;`sim.state` ∈ `unknown/detecting/absent/pin_required/puk_required/ready/error`。

`/api/sms`:
```json
{"ok":true,"rev":7,"count":3,"unread":1,"capacity":50,
 "messages":[{"id":7,"sender":"10086","receiver":"eSIM.gg","body":"...",
              "timestamp":"2026-09-06 12:00:00","profile":"eSIM.gg",
              "read":false,"complete":true}]}
```
`rev` 在任何 增/标读/删/清 后自增;`receiver` 与 `profile` 同值,`profile` 为兼容别名。

## 三、eSIM / eUICC

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/esim/profiles` | — | Profile 列表;非 eUICC 普通卡返回空列表(不是错误) |
| POST | `/api/esim/refresh` | — | 强制刷新列表 |
| POST | `/api/esim/switch` | `id` | 切换 Profile(32 位 hex AID,仅接受当前枚举结果) |
| GET | `/api/esim/eid` | — | 读 EID(带缓存):`{"ok":true,"eid":"8904..."}` |
| POST | `/api/esim/delete` | `id` | 删除停用 Profile;启用中的拒绝 |

```json
{"ok":true,"activeId":"A0000005591010...","switching":false,
 "profiles":[{"id":"A000...1200","iccid":"898603...完整19-20位",
              "operator":"giffgaff","name":"giffgaff","type":"eSIM",
              "status":"当前使用中|可切换","active":true,"available":false}],
 "job":{...同 status.job}}
```

Profile 切换为原子任务:断电重启模组 → 恢复配置 → 核对 → 自动选网,期间 `status.job` 实时汇报 `phase/progress/message`,任务状态持久化,重启后继续。

## 四、运营商选网

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/operator` | — | 当前驻留运营商、手动/自动模式、最近扫描结果 |
| POST | `/api/operator/scan` | — | 扫描可见 PLMN(1–3 分钟,后台任务) |
| POST | `/api/operator/select` | `numeric`(如 46001) `act`(可选) | 手动选网;仅接受本次扫描到的可用 PLMN,带自动回退 |
| POST | `/api/operator/auto` | — | 恢复自动选网 `AT+COPS=0` |

## 五、配置

### GET `/api/config`
```json
{
  "webUser":"admin","mustChangePassword":false,
  "adminPhone":"","numberBlackList":"",
  "smtp":{"server":"smtp.example.com","port":465,"user":"...","recipient":"...","passwordSet":true},
  "push":[{"enabled":true,"type":10,"name":"TG","urlSet":true,
           "key1Set":true,"key2Set":true,"customBody":""}],
  "wifi":{"nets":[{"ssid":"Home","passSet":true}],"apMode":false},
  "brand":{"title":"SMS FWD","sub":"..."},
  "tasks":[{"type":1,"name":"每日重启","mode":1,"intervalSeconds":0,
            "hour":1,"minute":30,"weekday":1,"dayOfMonth":1,"param":"",
            "httpMethod":0,"headers":"","body":""}],
  "pollSeconds":5,"apiTokenSet":true
}
```
所有密钥/密码/令牌字段只返回 `*Set` 布尔,不回显明文。push 共 10 槽、wifi 共 5 槽、tasks 共 10 槽。

### POST `/api/config`(表单编码)

| 参数 | 说明 |
|---|---|
| `webUser` `webPass` `adminPhone` `numberBlackList` | 账号/远程控制/黑名单(改密码≥8 位,改后全体会话失效) |
| `smtpServer` `smtpPort` `smtpUser` `smtpPass` `smtpSendTo` | 邮件通知(空=不变) |
| `pushCount` + `push{i}en/type/name/url/k1/k2/body` | 推送通道;`pushCount` 之后的槽位视为已删除;type:1=POST JSON 2=Bark 3=GET 4=钉钉 5=PushPlus 6=Server酱 7=自定义模板 8=飞书 9=Gotify 10=Telegram |
| `brandTitle`(≤24) `brandSub`(≤40) | 界面品牌 |
| `taskCount` + `task{i}type/name/mode/param/intervalSeconds/hour/minute/weekday/mday` | 定时任务;type:1=重启 2=AT 3=HTTP;mode:0=间隔 1=每天 2=每周 3=每月;间隔模式用 `intervalSeconds`(≤31536000),日历模式用 时/分/星期(0=周日)/几号(1-28) |
| `task{i}hm`(0=GET 1=POST) `task{i}hdrs`(每行 `Name: value`) `task{i}tbody` | HTTP 任务:方法/自定义头/请求体 |
| `pollSeconds`(1–60) | 页面状态刷新间隔 |
| `apiToken`(≤64,空=关闭) | API 访问令牌 |

### WiFi

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/api/wifi` | `wifiSsid0..4` + `wifiPass0..4`;`clearAll=1` 清空 | 保存后重启;名称空=删该项;同名不填密码保留旧密码 |

## 六、诊断接口(管理台诊断页使用)

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/at` | `cmd` | AT 透传(5s 超时),返回原始响应 |
| GET/POST | `/modem` | `action`=`signal\|operator\|imei\|restart\|hardreset` | 查询(GET 可)或控制模组;`restart`/`hardreset` 仅 POST 且需模组空闲 |
| GET | `/log` | `rev`(可选) | 环形日志 120 行:`{"ok":true,"rev":N,"lines":[...]}`;带 `?rev=N` 且未变化时仅返回 `{"ok":true,"rev":N}` |
| POST | `/ping` | — | 临时激活 PDP 并 Ping 8.8.8.8(最长 35s,同步返回结果) |

诊断接口返回统一为 `{"success":bool,"message":"..."}` 结构,与 `/api/*` 的 `ok` 结构不同。

## 七、管理员短信命令(非 HTTP)

配置 `adminPhone` 后,用该号码给设备卡发短信可远程控制:

| 命令 | 作用 |
|---|---|
| `SMS:手机号:内容` | 用设备卡向指定号码发送短信 |
| `RESET` | 重启 ESP32 与模组 |

## 八、设备行为要点

- **WiFi**:1 主 + 4 备;主用断开超 2 分钟自动轮换;全部失败开启开放热点 `sms-forwarder`(192.168.4.1)
- 全部 IP 流量走 WiFi,模组仅通过 AT 收发短信;Ping 诊断会临时激活 PDP
- **定时任务**:统一 60s 调度节拍;间隔模式按 `lastRun+intervalSeconds`,日历模式按本地 UTC+8;HTTP 任务可用 API 令牌调本机接口(如 `http://127.0.0.1/api/sms/clear`)
- **短信通知队列**:推送+邮件在独立任务按 FIFO 顺序发送(深度 8),连续到达逐条通知,不阻塞 Web
- **eSIM 通道降级**:CCHO 失败 → CSIM 基础通道,日志可见降级过程
- **推送重试**:每通道 3 次退避;业务体校验(钉钉 errcode:0、飞书 code:0、PushPlus code:200、Server酱 code:0、Telegram ok:true)
