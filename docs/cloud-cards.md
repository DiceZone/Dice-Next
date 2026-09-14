# BDC 云人物卡适配

协议基线：`ShiaNyaa/Better-Dice-Control` 的 `b894848`（2026-09-14，已核对远端 HEAD）。
对应源码：`sites/account/backend/app/api/cloud_cards.py`、`oauth_device.py`、
`packages/shared-backend/better_dice_control/cards/document.py`。

## 玩家使用

骰主先在账号中心验证骰娘所有权，在**当前适配器**填写正式骰娘 API Key
（`heart_api_key`）。自动未验证心跳凭据不能用于云卡。不要把 Key 发给玩家。

所有云卡操作在玩家与骰娘的**私聊**中进行：

```text
.pc cloud auth             # 默认请求 cards.read
.pc cloud auth write       # 需要上传/同步时主动申请写入；网页仍须明确同意
.pc cloud confirm          # 网页输码同意后确认，服务端会限制确认频率
.pc cloud list             # 列出本人授权账号的云卡 ID、名称、版本
.pc cloud pull 云卡ID       # 创建同名本地卡；不会覆盖无关同名卡
.pc cloud pull 云卡ID 新名  # 使用指定本地名；已有映射时新名字作为独立对照副本
.pc cloud push 本地卡名     # 首次上传；已关联则按原 card_id 同步
.pc cloud sync 本地卡名     # 按上次同步版本提交，接受云端三方合并结果
.pc cloud status
.pc cloud logout
```

拉取后，在跑团群 `.pc tag 本地卡名` 绑定，原有 `.st/.ra/.sc`、Lua/JS 读卡继续使用
本地人物卡。普通掷骰/改卡不发网络请求；无网络、退出授权或云端异常不影响本地跑团。
当前为手动同步，不是后台自动同步。
显式云卡请求在有界独立后台队列执行，结果另行私发，不占用适配器消息处理线程。

授权码单独从原适配器私发，不经过普通回复日志、AI、插件回复加工、模拟聊天或 log
落库。玩家不要转发验证码。自部署服务器由骰主管理，不能承诺骰主无法读取已获授权的数据。

## 数据保护

- 完整转换数值属性、`__meta.locks`、`__meta.texts` 和未识别的插件扩展字段。
- `card_id` 是云卡身份，映射保存本地行 ID 和同步快照。本地改名继续写同一云卡；
  自选本地别名不会在第一次同步时意外覆盖云卡名。
- 同名历史多行、空名卡、未知 Schema、越界数值和格式异常明确拒绝，不能猜一条上传。
- 不自动切换规则系统、不自动绑定群卡、不删除云卡，也不隐式跨玩家授权。
- 写入带上真实 `base_rev`。409 不重试、不换成最新 rev 强行覆盖，保留双方数据；
  用户可拉取另名对照副本，手动整理或作为新卡上传。当前没有冲突逐字段选择界面。
- 拉取不能覆盖已修改的本地卡。网络期间本地变化使用条件写入检测；不能用旧响应
  覆盖新 `.st` 修改。现有读/写锁继续限制上传与同步。
- 映射/快照存于主数据库的独立 `bdc-cloud:<账号上下文摘要>` 用户设置作用域，
  不放入人物卡 attrs，避免污染规则包数据。访问令牌和待授权码仅保存在内存。

## 当前 BDC 协议限制

- 目前设备授权没有签发 Refresh Token，因此令牌过期或 Dice!Next 重启后需要重新授权。
  同步映射、本地卡不会丢失。不能用普通 OIDC Refresh Token 流程冒充续期。
- 当前发现文档没有 `device_authorization_endpoint`。确认官方 issuer 后兼容使用
  源码中的 `/api/oauth/device_authorization`；令牌地址仍读发现文档。只允许同一官方
  HTTPS origin，校验 TLS，不向任意重定向目标发送 Key/Token。
- `logout` 只清除本机临时凭据；完全撤销同意需到账号中心“已授权应用”操作。
- 首次 POST 遇到网络超时时，远端可能已经建卡。不会自动重试，请先 list 确认，避免重复创建。
- 只同步卡片 attrs 文档。另存于本地用户设置的表达式绑定、武器快捷指令等并非 BDC
  Schema v1 卡文档的一部分，不会被擅自执行或转换；未知字段原样保留。

## 验证

`test_cloud_cards.cpp` 使用隔离 SQLite 和模拟 BDC 传输，覆盖数据往返、授权隔离、
限速、改名、离线可用、锁定、旧数据歧义、409 和并发本地修改。
设置 `DICENEXT_CLOUD_LIVE=1` 可单独运行 `LiveMetadataSmokeWhenExplicitlyEnabled`，
只探测公开 Discovery，不读取真实玩家卡片、不创建授权。

2026-09-14 验证结果：Release 编译成功；核心 381 个用例 / 1863 项断言、Lua 9 个
用例 / 71 项断言全部通过。云卡定向测试 118 项断言通过，公开 HTTPS 探测 2 项断言
通过。文档站构建通过。真实玩家授权、读写私有云卡仍需由本人完成网页同意后联调。
