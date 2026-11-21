# CatOfficial

CatOfficial 把 QQ 官方机器人转接为标准 OneBot V11，让原本面向 OneBot 的应用不改代码即可接入官方机器人。

官方机器人已开放的能力会原样中继；官方后续开放新能力，CatOfficial 随之跟进。转接时处理好 OneBot 开发者关心的细节：

- 数字形式的 `user_id` / `group_id`，已做官民绑定的用户与群沿用原 QQ 号与群号
- 历史消息索引：引用回复、`get_msg`、撤回都能用；被动回复所需的 `msg_id` 自动补全
- 头像地址、昵称与群身份

## 快速开始

```
catofficial [配置文件]
```

第一次运行会在当前目录生成 `config.json` 并照常启动。打开控制台里显示的管理面板地址（默认 `http://127.0.0.1:18082/`，密码也显示在控制台），在「登录管理」里添加官方机器人（填 AppID 与 AppSecret，或直接扫码登录），在「连接管理」里添加 OneBot 连接即可。

## 管理面板

| 页面 | 内容 |
|---|---|
| 概览 | 各机器人是否在线、各连接的状态、收发数量 |
| 登录管理 | 添加多个官方机器人（默认扫码登录，也可手动输入密钥）；接入 OneBot 机器人（如 NapCat，反向或正向 WebSocket），供官民绑定使用 |
| 连接管理 | 添加 OneBot 连接，每个连接单独设置令牌、消息格式、是否上报自身消息、服务哪些官方机器人 |
| 消息记录 / 运行日志 | 收发的消息；程序最近 3000 条日志 |
| 官民绑定 | 已绑定的用户与群、等待确认的群、手动绑定与解绑 |
| 设置 | 主人、邮件（SMTP）与运行异常通知、绑定指令、Webhook 入口、面板端口与密码 |

- 机器人、连接与设置的修改写入配置文件并立即生效，不用重启。只有面板自身的地址、官方 Webhook 入口与数据目录改了要重启，面板顶部会提示，点一下即可；重启后登录状态保留。
- 面板与 OneBot 使用不同端口。默认只监听本机；改成 `0.0.0.0` 时其他机器也能访问，请使用足够强的密码。同一 IP 连续输错 5 次密码会被暂停 5 分钟。
- 前端文件放在程序旁的 `web` 目录（`webui.root` 可改）。从源码构建：`cd webui && npm install && npm run build`，再把 `webui/dist` 复制为 `web`。

## OneBot 连接

四类连接与 NapCat 的划分一致：

| 类型 | 说明 |
|---|---|
| `http_server` | HTTP API：`POST /{action}`，参数可放 JSON 请求体、表单或查询串 |
| `ws_server` | 正向 WebSocket：`/` 收事件并调用动作；`/event` 只收事件；`/api` 只调用动作 |
| `http_client` | HTTP POST 上报；设了令牌时带 `X-Signature: sha1=…`；响应体可做快速回复（`reply`、`auto_escape`、`at_sender`） |
| `ws_client` | 反向 WebSocket（Universal）：每个机器人一条连接，带 `X-Self-ID`；断线按 `reconnect_interval` 重连 |

鉴权用 `Authorization: Bearer <令牌>`，或查询参数 `access_token`。

反向 WebSocket、HTTP POST 上报与原 QQ 机器人的地址可以是 `wss://` / `https://`。`http_server` 与 `ws_server` 本身不带 TLS，需要让外网应用以 `wss://` 接入时，请在前面放 Nginx、Caddy 等反向代理。一个连接服务多个官方机器人时，事件带各自的 `self_id`；调用动作时用参数 `self_id`（或请求头 `X-Self-ID`）指定机器人，不指定则用第一个。开启「上报自身消息」后，机器人发出的消息以 `message_sent` 事件上报。

配置文件里的写法：

```jsonc
{
  "bots": [
    { "id": "bot1", "name": "主号", "enabled": true, "type": "official", "app_id": "", "app_secret": "",
      "mode": "websocket", "subscribe_group_member_events": false, "auto_ack_interactions": true },
    { "id": "bot2", "name": "", "enabled": true, "type": "onebot", "connection": "reverse", "host": "127.0.0.1", "port": 6800, "token": "" }
  ],
  "connections": [
    { "id": "ws", "name": "正向 WebSocket", "enabled": true, "type": "ws_server", "host": "127.0.0.1", "port": 6700,
      "token": "", "message_format": "array", "report_self": false, "heartbeat_interval": 30000, "bots": [] }
  ],
  "masters": [ { "bot_id": "", "openid": "…" } ],
  "notify_masters": true,
  "webhook": { "path": "/webhook/qqbot", "standalone": false, "host": "0.0.0.0", "port": 8443 },
  "smtp": { "enabled": false, "host": "", "port": 465, "security": "ssl", "username": "", "password": "", "from": "", "notify_emails": [], "notify_errors": true },
  "bind": { "enabled": true, "command": ".bind", "window_seconds": 5, "group_confirm_users": 3, "email_enabled": true, "email_domain": "qq.com", "code_ttl_minutes": 10 },
  "webui": { "enabled": true, "host": "127.0.0.1", "port": 18082, "password": "", "root": "web" },
  "data_dir": "data", "log_level": "info", "message_retention_days": 7
}
```

机器人的 `name` 留空时显示机器人昵称。OneBot 机器人的 `connection` 为 `reverse`（默认，在 NapCat 里添加 WebSocket 客户端连 `ws://host:port/`）或 `forward`（填 `url`，CatOfficial 去连它）。连接的 `bots` 为空表示服务全部官方机器人。Webhook 入口默认挂在面板端口上，`standalone` 为 true 时单独监听 `port`。旧版的单机器人配置（`bot` + `onebot`）启动时会自动转成上面的格式。

## 号码与官民绑定

官方只提供 OpenID。CatOfficial 给每个 OpenID 分配一个固定号码：群为 `1` 开头、用户为 `2` 开头的 13 位数字，与 Dice!Next 相同。号码一经分配就不会再给别人，绑定或解绑后空出来的号码也不会回收。`self_id` 是机器人的真实 QQ 号。

用户或群从普通 QQ 机器人迁移过来时，绑定回原来的 QQ 号与群号，应用里按号码存的数据就能接上。绑定只是 CatOfficial 里的号码映射，不搬迁任何数据。

群友可以自助绑定（指令可在设置中修改）：

```
.bind <暗号>        // 在官方机器人与原 QQ 机器人都在的群里发送，两边几秒内同时收到即完成绑定
.bind email <QQ号>  // 把验证码发到该 QQ 的邮箱（需要配置 SMTP）
.bind code <验证码> // 用邮件里的验证码完成绑定
.bind status        // 查看当前绑定
.bind remove        // 解除自己的绑定
.bind group <群号>  // （主人）把本群直接关联到群号
```

暗号绑定需要在「登录管理」里接入一个 OneBot 机器人（如 NapCat），并和官方机器人在同一个群。用户绑定立即生效；群号在群主或管理员完成绑定时立即关联，否则需要凑够设定人数的普通成员。同一暗号被两个人同时使用时会作废，请用不容易撞的暗号。

主人也可以在面板的「官民绑定」页手动绑定，或：

- 在 `data_dir` 下放 `bindings.json`，启动时载入：
  ```json
  [ { "type": "user", "openid": "…", "qq": 12345678 },
    { "type": "group", "openid": "…", "qq": 87654321 } ]
  ```
- 调用扩展动作 `set_identity_binding`：`{"type": "user", "user_id": <当前号码>, "qq": <真实号码>}`。

## 主人与运行异常通知

主人 = 某个官方机器人（或全部）上的 OpenID，可以在群里用 `.bind group <群号>` 直接关联群号。OpenID 可以让对方发送 `.bind status` 查看。

程序出现错误时，CatOfficial 会把一段时间内的错误汇总（最多每 10 分钟一次），发到设置里的通知邮箱（可填多个），并私聊通知主人。邮件也用于 `.bind email` 的验证码。

## 支持的动作

| 动作 | 说明 |
|---|---|
| `send_private_msg` / `send_group_msg` / `send_msg` | 文字、@、引用、图片、语音、视频、文件；图片可用 URL、`base64://`、本地路径 |
| `delete_msg` | 撤回机器人发出的消息 |
| `get_msg` | 取回收发过的消息；引用 CatOfficial 没见过的旧消息时，按官方带来的内容补建索引 |
| `get_login_info` / `get_stranger_info` / `get_friend_list` | 好友列表为私聊过的用户 |
| `get_group_info` / `get_group_list` / `get_group_member_info` / `get_group_member_list` | 来自收到过的消息与事件 |
| `set_group_kick` / `set_group_ban` / `set_group_add_request` | 需要机器人有对应的官方群管权限 |
| `get_status` / `get_version_info` / `can_send_image` / `can_send_record` | |

官方没有对应能力的动作返回 `retcode` 1404。扩展动作：

| 动作 | 说明 |
|---|---|
| `set_identity_binding` | 官民绑定，见上文 |
| `get_official_id` | 查号码对应的 OpenID |
| `call_official_api` | 直接调用开放平台接口：`{"method": "GET", "path": "/v2/…", "body": {}}` |

扩展消息段：`markdown`（`{"content": "…"}` 或模板参数）、`keyboard`（按钮）。收到的事件里，用户与发送者带 `avatar` 字段；也可以请求 `/avatar/user/{user_id}?s=640` 跳转到头像。

## 事件

| 官方事件 | OneBot 事件 |
|---|---|
| 单聊、群聊（@ 或全量）、频道 @ 消息 | `message`；群里 @ 机器人时消息开头带 `[CQ:at,qq=<self_id>]` |
| 机器人入群 / 被移出 | `notice.group_increase`（invite）/ `notice.group_decrease`（kick_me） |
| 成员入群 / 退群 | `notice.group_increase`（approve）/ `notice.group_decrease`（leave） |
| 添加好友 | `notice.friend_add` |
| 加群申请 | `request.group`（add） |
| 其他（主动消息开关、按钮互动、订阅状态、频道事件等） | `notice.qq_official`，`sub_type` 为官方事件名的小写，原始数据在 `data` |

## 构建

需要 CMake 3.21+、C++20，以及 [Drogon](https://github.com/drogonframework/drogon)、[IXWebSocket](https://github.com/machinezone/IXWebSocket)、[nlohmann/json](https://github.com/nlohmann/json)、OpenSSL、SQLite3、libcurl（`vcpkg.json` 已列出）。

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

产物为 `catofficial` 与单元测试 `catofficial-tests`。

## 作为库使用

协议层全部是头文件（`catofficial::core`），[Dice!Next](https://github.com/DiceZone/Dice-Next) 的 QQ 官方机器人适配器即基于它构建。作为子项目引用时默认只提供协议层，不编译程序与测试。

```cmake
add_subdirectory(path/to/CatOfficial)
target_link_libraries(your-target PRIVATE catofficial::core)
```

| 头文件 | 内容 |
|---|---|
| `catofficial/bot.h` | 一个机器人的完整生命周期，发送时自动补全被动回复 |
| `catofficial/qrlogin.h` | 扫码登录官方机器人，取回 AppID 与 AppSecret |
| `catofficial/session.h` | AccessToken、WebSocket 连接、鉴权、心跳与重连 |
| `catofficial/event.h` | 与框架无关的事件解析 |
| `catofficial/api.h` | 开放平台接口调用、机器人资料与分享链接 |
| `catofficial/media.h` | 富媒体 URL 直传与本地文件分片上传 |
| `catofficial/webhook.h` | Webhook 验签、回调地址验证与按 AppID 分派 |
| `catofficial/reply_state.h` | 被动回复序号与事件去重 |
| `catofficial/protocol.h` | 字段解析、WebSocket 订阅的 intents |
| `catofficial/message.h` | 官方消息格式（表情、图片地址等） |

## 许可

[AGPL-3.0](LICENSE)。通过 OneBot 协议连接 CatOfficial 的应用不受此许可约束；修改 CatOfficial 本身或将其代码并入其他程序时，须以相同许可公开源代码，以网络服务形式提供修改版时亦然。

「CatOfficial」名称与标志未经许可不得用于衍生版本。
