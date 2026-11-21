# CatOfficial

CatOfficial 把 QQ 官方机器人转接为标准 OneBot V11，让原本面向 OneBot 的应用不改代码即可接入官方机器人。

官方机器人已开放的能力会原样中继；官方后续开放新能力，CatOfficial 随之跟进。转接时处理好 OneBot 开发者关心的细节：

- 数字形式的 `user_id` / `group_id`，已做官民绑定的用户与群沿用原 QQ 号与群号
- 历史消息索引：引用回复、`get_msg`、被动回复所需的 `msg_id` 自动补全
- 头像地址、昵称与群名片

## 当前状态

开发中。目前仓库提供官方机器人协议层（`catofficial::core`），[Dice!Next](https://github.com/DiceZone/Dice-Next) 的 QQ 官方机器人适配器即基于它构建。OneBot V11 服务端尚未完成。

| 头文件 | 内容 |
|---|---|
| `catofficial/protocol.h` | 事件与接口字段解析、Gateway intents |
| `catofficial/webhook.h` | Webhook Ed25519 签名与回调校验 |
| `catofficial/gateway_socket.h` | Gateway WebSocket 客户端 |
| `catofficial/message.h` | 官方消息格式（图片地址、富媒体等） |

## 构建

需要 CMake 3.20+、C++20，以及 [Drogon](https://github.com/drogonframework/drogon)、[nlohmann/json](https://github.com/nlohmann/json)、OpenSSL。

```cmake
add_subdirectory(path/to/CatOfficial)
target_link_libraries(your-target PRIVATE catofficial::core)
```

## 许可

[AGPL-3.0](LICENSE)。通过 OneBot 协议连接 CatOfficial 的应用不受此许可约束；修改 CatOfficial 本身或将其代码并入其他程序时，须以相同许可公开源代码，以网络服务形式提供修改版时亦然。

「CatOfficial」名称与标志未经许可不得用于衍生版本。
