# SMS FWD 固件 API 文档

> 适用固件:Tianmoy/sms_forwarding
> 设备地址:局域网 IP(默认路由分配),配置热点模式:`192.168.4.1`

## 认证模型

| 项 | 说明 |
|---|---|
| 方式 | `Authorization: Bearer <token>` 请求头,两类令牌任选其一 |
| 会话令牌 | `POST /api/login` 返回的 `token`,8 小时绝对超时 / 30 分钟闲置超时,绑定来源 IP |
| API 令牌 | 设置页"API 令牌"生成/自定义,长期有效,适合定时任务访问本机或外部脚本 |
| 限流 | 登录每满 5 次失败锁定,时长 30s 起逐级翻倍至 15 分钟封顶,返回 429 + `retryAfter` |
| 错误码 | 401 未认证 / 405 写操作用 GET / 409 设备忙 / 429 限流 / 503 模组不可用 |

```bash
# 登录取会话令牌(把 192.168.1.100 换成设备实际 IP)
TOKEN=$(curl -s -X POST http://192.168.1.100/api/login \
  -d "username=admin" -d "password=******" | jq -r .token)

# 查询(GET 即可)
curl -H "Authorization: Bearer $TOKEN" http://192.168.1.100/api/status

# 写操作(POST)
curl -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.100/api/reboot

# 用 API 令牌调本机(定时任务场景)
# 任务 URL: http://127.0.0.1/api/sms/clear
# 任务请求头: Authorization: Bearer <API令牌>
```

---

## 一、认证 / 会话

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/session` | — | 查询当前会话状态 |
| POST | `/api/login` | `username` `password` | 登录,返回 `token` |
| POST | `/api/logout` | — | 注销会话 |
| GET | `/api/brand` | — | **公开**。品牌信息 `{ok,title,sub}` |
| POST | `/api/sms/send` | `phone` `content` | 用设备卡发短信(号码仅允许数字与前置 `+`) |
| GET/POST | `/api/reboot` | — | 整机重启(~40–60s) |
| GET/POST | `/api/factory/reset` | — | 恢复出厂(清 NVS+短信,重启进热点) |
| POST | `/api/push/test` | `channel`(0–9 或 `email`) | **单通道**测试:通道编号或邮件 |

## 二、状态与短信

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/status` | — | 总状态:uptime/heap/wifi/modem/sim/sms/push/job |
| GET | `/api/sms` | `rev`(可选) | 短信列表(最近 50 条,LittleFS 持久化);响应含 `rev` 修订号,任何增/读/删/清自增。带 `?rev=N` 且未变化时省略 `messages` 数组,仅约 50 字节 |
| POST | `/api/sms/read` | `id` | 标记已读 |
| POST | `/api/sms/delete` | `id` | 删除单条 |
| POST | `/api/sms/clear` | — | 清空全部 |

`/api/status` 关键字段:
```json
{
  "wifi":  {"connected":true,"ssid":"...","rssi":-52,"ip":"192.168.1.100"},
  "modem": {"ready":true,"operator":"中国联通","busy":false,
            "registration":"已注册","rsrp":-97,"rsrq":-9},
  "sim":   {"state":"ready","present":true,"smsReady":true,"profile":"eSIM.gg",
            "phone":"+86...","iccidTail":"000034","operator":"中国联通","eid":""},
  "sms":   {"stored":3,"unread":0,"capacity":50},
  "push":  {"enabled":1},
  "job":   {"active":false,"phase":"idle","progress":0,"message":""}
}
```

> `sim.phone` 来自 `AT+CNUM`,卡内未写入时为空;`sim.eid` 仅 eUICC 卡返回,普通 SIM 为空串。
> 管理页对静态 HTML 使用 ETag 协商,二次刷新返回 304 不重传页面。

## 三、eSIM / eUICC

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/esim/profiles` | — | Profile 列表(SGP.22 ES10c,通道两级降级:CCHO→CSIM 基础;非 eUICC 普通卡返回空列表) |
| POST | `/api/esim/refresh` | — | 强制刷新列表 |
| POST | `/api/esim/switch` | `id`(32位hex AID) | 切换 Profile(原子启用,自动断电重启模组→恢复配置→核对→自动选网) |
| GET | `/api/esim/eid` | — | 读 EID(带缓存) |
| POST | `/api/esim/delete` | `id` | 删除停用 Profile(启用中的拒绝,仅接受枚举 AID) |

profiles 响应:
```json
{"ok":true,"activeId":"A0000005591010FFFFFFFF8900XXXX","switching":false,
 "profiles":[{"id":"A000...1200","iccid":"8986030000...034","operator":"giffgaff",
              "name":"giffgaff","type":"eSIM","status":"可切换","active":false,"available":true}],
 "job":{...}}
```

## 四、运营商选网

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/operator` | — | 当前驻留与模式 |
| POST | `/api/operator/scan` | — | 扫描可见 PLMN(1–3 分钟,任务化) |
| POST | `/api/operator/select` | `numeric`(如 46001) `act` | 手动选网(带自动回退) |
| POST | `/api/operator/auto` | — | 恢复自动选网 |

## 五、配置

### GET `/api/config`

```json
{
  "webUser":"admin", "mustChangePassword":false,
  "adminPhone":"", "numberBlackList":"",
  "smtp":{"server":"smtp.example.com","port":465,"user":"...","recipient":"...","passwordSet":true},
  "push":[ {"enabled":true,"type":10,"name":"TG","urlSet":true,
            "key1Set":true,"key2Set":true,"customBody":""} , ...共10槽 ],
  "wifi":{"nets":[{"ssid":"Home","passSet":true}, ...共5槽], "apMode":false},
  "brand":{"title":"SMS FWD","sub":"愿你天黑有灯，下雨有伞"},
  "tasks":[{"type":1,"name":"每日重启","mode":1,"intervalSeconds":0,
            "hour":1,"minute":30,"weekday":1,"dayOfMonth":1,"param":"",
            "httpMethod":0,"headers":"","body":""}, ...共10槽],
  "pollSeconds":5, "apiTokenSet":true
}
```

> 所有密钥/密码字段只返回 `*Set` 布尔,不回显明文;API 令牌同样只返回 `apiTokenSet`。

### POST `/api/config`(表单编码)

| 参数 | 说明 |
|---|---|
| `webUser` `webPass` `adminPhone` `numberBlackList` | 账号/远程控制/黑名单(改密码≥8位,改后全体会话失效) |
| `smtpServer` `smtpPort` `smtpUser` `smtpPass` `smtpSendTo` | 邮件通知(空=不变) |
| `pushCount` + `push{i}en/type/name/url/k1/k2/body` | 推送通道,`pushCount` 之后的上限为已删除;type:1=POST JSON 2=Bark 3=GET 4=钉钉 5=PushPlus 6=Server酱 7=自定义模板 8=飞书 9=Gotify 10=Telegram |
| `brandTitle`(≤24) `brandSub`(≤40) | 界面品牌 |
| `taskCount` + `task{i}type/name/mode/param/intervalSeconds/hour/minute/weekday/mday` | 定时任务;type:1=重启 2=AT 3=HTTP;mode:0=间隔 1=每天 2=每周 3=每月;间隔模式用 `intervalSeconds`(≤31536000),日历模式用 时/分/星期(0=周日)/几号(1-28) |
| `pollSeconds`(1–60) | 页面状态刷新间隔 |
| `apiToken`(≤64,空=关闭) | API 访问令牌 |
| `task{i}hm`(0=GET 1=POST) `task{i}hdrs`(每行`Name: value`) `task{i}tbody` | HTTP 任务:方法/自定义头/请求体 |

### 其它设备操作

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/api/wifi` | `wifiSsid0..4` + `wifiPass0..4`(名称空=删该项;同名不填密码保留旧密码) | 保存 WiFi 并重启;`clearAll=1` 清空(重启进热点) |
| POST | `/api/reboot` | — | 整机重启(~40–60s) |
| POST | `/api/factory/reset` | — | 恢复出厂:清 NVS+短信,重启进配置热点 |
| POST | `/api/push/test` | `channel`(0–9 或 `email`) | **单通道**测试:通道编号或邮件 |

## 六、兼容接口(均需 Bearer)

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/` `/tools` `/sms` | — | SPA 页面(ETag 协商缓存) |
| GET/POST | `/query` | `type=ati\|signal\|siminfo\|network\|wifi` | 状态查询 |
| GET/POST | `/modem` | `signal\|operator\|imei`(GET 可);`restart\|hardreset` 仅 POST | 模组控制 |
| GET | `/log` | `rev`(可选) | 环形日志(120 行);带 `?rev=N` 且未变化时仅返回修订号 |
| POST | `/at` | `cmd` | AT 透传(5s 超时) |
| POST | `/wifi` | `action=restart` | 重连 WiFi |

> 旧 `/sendsms` 已移除,改用 `/api/sms/send`。

## 七、设备行为要点

- **WiFi**:1 主 + 4 备;主用断开超 2 分钟自动轮换;全部失败开启开放热点 `sms-forwarder`(192.168.4.1)
- 全部 IP 流量走 WiFi,模组仅通过 AT 收发短信;Ping 保号工具会临时激活 PDP
- **定时任务**:统一 60s 调度节拍;间隔模式按 `lastRun+intervalSeconds`,日历模式按本地 UTC+8 计划时刻;AT/重启任务避开模组忙时段,HTTP 任务仅需 WiFi
- **短信通知队列**:推送+邮件在独立任务按 FIFO 顺序发送(队列深度 8),连续到达的多条短信逐条通知,不阻塞 Web 服务
- **eSIM 通道降级**:CCHO 失败→CSIM 基础通道,日志可见降级过程
- **推送重试**:每通道 3 次退避;业务体校验(钉钉 errcode:0、飞书 code:0、PushPlus code:200、Server酱 code:0、Telegram ok:true)
