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

第一次运行会在当前目录生成 `config.json` 模板，填好 `bot.app_id` 与 `bot.app_secret` 再启动即可。

```jsonc
{
  "bot": {
    "app_id": "",
    "app_secret": "",
    "mode": "gateway",                  // gateway：主动连官方 Gateway；webhook：由官方回调推送
    "subscribe_group_member_events": false,
    "auto_ack_interactions": true       // 自动回应按钮互动，避免客户端转圈
  },
  "webhook": { "host": "0.0.0.0", "port": 8443, "path": "/webhook/qqbot" },   // 仅 webhook 模式
  "onebot": {
    "access_token": "",
    "message_format": "array",          // array 或 string（CQ 码）
    "heartbeat_interval": 30000,
    "servers": [                        // CatOfficial 当服务端
      { "type": "http", "host": "127.0.0.1", "port": 5700 },
      { "type": "ws", "host": "127.0.0.1", "port": 6700 }
    ],
    "http_post": [ { "url": "http://127.0.0.1:8080/", "secret": "" } ],
    "ws_reverse": [ { "url": "ws://127.0.0.1:8080/onebot/v11/ws", "reconnect_interval": 3000 } ]
  },
  "data_dir": "data",
  "log_level": "info",
  "message_retention_days": 7
}
```

四种连接方式与 NapCat、go-cqhttp 的约定一致：

| 方式 | 说明 |
|---|---|
| HTTP API | `POST /{action}`，参数可放 JSON 请求体、表单或查询串 |
| 正向 WebSocket | `/` 收事件并调用动作；`/event` 只收事件；`/api` 只调用动作 |
| HTTP POST | 事件 POST 到 `url`；设了 `secret` 时带 `X-Signature: sha1=…`；响应体可做快速回复（`reply`、`auto_escape`、`at_sender`） |
| 反向 WebSocket | Universal 连接，带 `X-Self-ID`、`X-Client-Role`；断线按 `reconnect_interval` 重连 |

鉴权用 `Authorization: Bearer <access_token>`，或查询参数 `access_token`。

## 号码与官民绑定

官方只提供 OpenID。CatOfficial 给每个 OpenID 分配一个固定号码：群为 `1` 开头、用户为 `2` 开头的 13 位数字，与 Dice!Next 相同。`self_id` 是机器人的真实 QQ 号。

用户或群从普通 QQ 机器人迁移过来时，可以绑定回原来的 QQ 号与群号，应用里按号码存的数据就能接上。两种方式：

- 在 `data_dir` 下放 `bindings.json`，启动时载入：
  ```json
  [ { "type": "user", "openid": "…", "qq": 12345678 },
    { "type": "group", "openid": "…", "qq": 87654321 } ]
  ```
- 调用扩展动作 `set_identity_binding`：`{"type": "user", "user_id": <当前号码>, "qq": <真实号码>}`。

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

需要 CMake 3.21+、C++20，以及 [Drogon](https://github.com/drogonframework/drogon)、[nlohmann/json](https://github.com/nlohmann/json)、OpenSSL、SQLite3（`vcpkg.json` 已列出）。

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
| `catofficial/session.h` | AccessToken、Gateway 连接、鉴权、心跳与重连 |
| `catofficial/event.h` | 与框架无关的事件解析 |
| `catofficial/api.h` | 开放平台接口调用、机器人资料与分享链接 |
| `catofficial/media.h` | 富媒体 URL 直传与本地文件分片上传 |
| `catofficial/webhook.h` | Webhook 验签、回调地址验证与按 AppID 分派 |
| `catofficial/reply_state.h` | 被动回复序号与事件去重 |
| `catofficial/protocol.h` | 字段解析、Gateway intents |
| `catofficial/message.h` | 官方消息格式（表情、图片地址等） |

## 许可

[AGPL-3.0](LICENSE)。通过 OneBot 协议连接 CatOfficial 的应用不受此许可约束；修改 CatOfficial 本身或将其代码并入其他程序时，须以相同许可公开源代码，以网络服务形式提供修改版时亦然。

「CatOfficial」名称与标志未经许可不得用于衍生版本。
