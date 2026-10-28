# 内置登录（openopq，实验性）

新建账号时选「内置登录」，DN 自己下载并拉起 openopq 协议端，二维码直接显示在 WebUI 里，
用户不用另外部署、配置协议端。

协议端**不随配布包分发**：执行档由中心按 BDC 骰娘 API Key 发放，签名服务也认同一把 Key。
没有 Key 建不了这种账号，也下载不到执行档。

## DN 这一侧

| 部分 | 位置 |
|---|---|
| 清单、下载、校验、子进程守护、管理接口客户端 | `server/src/service/openopq_runtime.{h,cpp}` |
| 适配器（OneBot v11 正向 WS + 管理子进程） | `server/src/adapter/openopq_adapter.h` |
| 新建 / 修改 / 删除、状态与扫码接口 | `server/src/service/api_service.h` |

- 适配器类型 `openopq`（`AdapterType::kOpenOPQ = 6`）。消息、群管理全部沿用 OneBot v11。
- 新建时必填 `heartApiKey`（`bdc_` 开头）和 `qqNumber`。openopq 的本机端口由 DN 分配，存在
  `adapters.json` 的 `openopq.console_port`，不接受外部修改。
- DN 经这个端口上的 OneBot v11 **正向 WS**（`/onebot/v11/ws`）收发。openopq v1.3.0-rc.1 没有反向 WS，
  之后的版本也保留了这个入口，所以换客户端版本不用改 DN。openopq 只监听 `127.0.0.1`。
- 协议端由 DN 托管：连不上时每 3 秒重试、不暂停；扫码前取不到账号资料，每 5 秒再取一次，
  取到后刷新群列表。这两项只对内置登录生效，普通 OneBot 账号的重连策略不变。
- 执行档放在 `runtime/openopq/<版本>/`（不进 `data/`，免得备份变大）；每个账号的配置、设备
  身份、日志在 `data/openopq/<账号 id>/`，删除账号时一并删除。
- 子进程退出后按 5 秒起、翻倍、最长 5 分钟重启；连续跑满 10 分钟后退避归零。Windows 上子进程在
  作业对象里，DN 退出时一起结束；Linux/macOS 上启动前按本账号的数据目录 `pkill` 掉残留进程。
- 中心返回 401/403 时停在 `key_rejected`，不再重试，直到用户在 WebUI 点重连或改 Key。

### WebUI 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/adapters/{id}/openopq` | 运行阶段与二维码，见下 |
| POST | `/api/adapters/{id}/openopq/login` | 开始扫码登录 |
| POST | `/api/adapters/{id}/openopq/cancel` | 取消扫码 |
| POST | `/api/adapters/{id}/openopq/disconnect` | 断开 QQ（进程保留） |

状态返回：

```json
{
  "stage": "preparing | downloading | running | backoff | key_rejected | failed | stopped",
  "error": "",
  "version": "v1.3.0-rc.1",
  "restarts": 1,
  "account": "123456789",
  "console": {
    "phase": "ready | waiting | online | offline | …",
    "online": false,
    "nickname": "",
    "qr": "data:image/png;base64,…",
    "qrExpires": "2026-10-25T12:00:00Z",
    "error": "",
    "notice": ""
  }
}
```

`console` 只在 `stage == "running"` 且 openopq 管理接口有响应时才有，其余时候为 `null`。
openopq 目前不保存登录票据，**每次进程重启都要重新扫码**。

## 中心要提供的东西

### 清单 `GET https://cloud.dice.zone/api/openopq/manifest`

请求带 `Authorization: Bearer <BDC 骰娘 API Key>`；Key 无效返回 401/403。返回：

```json
{"payload": "<清单 JSON 原文>", "signature": "<对 payload 原文字节的 Ed25519 签名，base64>"}
```

`payload` 解出来是：

```json
{
  "schema": 1,
  "version": "v1.3.0-rc.1",
  "qq_version": "QQNT-v7.0.2-53644",
  "sign_url": "https://sign-v1.dice.zone/k/{key}/sign",
  "cdn_lists": [],
  "assets": [
    {"os": "windows", "arch": "amd64", "url": "https://cloud.dice.zone/api/openopq/download/v1.3.0-rc.1/windows-amd64",
     "sha256": "<64 位小写十六进制>", "size": 8507392}
  ]
}
```

- `os` 取 `windows` / `linux` / `macos`，`arch` 取 `amd64` / `arm64`。
- 执行档是**解压后的单个文件**，不是发布包：DN 不解压，校验大小和 SHA-256 后直接运行。
- 执行档下载同样要带 Key，否则返回 401/403。
- `qq_version` 必须和签名服务对应的 QQ 版本一致。
- 签名私钥只在中心发布清单时使用，不进任何仓库。DN 内置的公钥是
  `eQKl/N6UbaUw8g4OW9R+UfiH40rXSvcVFmHrC6xE3OI=`。换钥匙要发新版 DN。
- 路径不带扩展名、放在 `/api` 下，响应一律 `Cache-Control: private, no-store`：前面是 CDN，
  带 Key 才给的东西一旦进了缓存，不带 Key 也能拿到。
- Key 须属于骰子之间里仍有效的骰娘绑定。发布新版本时先传执行档、核对摘要，最后才替换清单。

### 签名 `sign_url`

`{key}` 由 DN 换成用户的 API Key，写进 openopq 的 `FakeSignApi`。openopq 的签名请求本身没有
鉴权字段，所以 Key 放在路径里，签名网关按路径鉴权，也按它分流。

同一个 QQ 的会话必须固定在同一个签名 worker（`X-OPQ-Session`），**不能轮询分流**。网关应维护
「Key + QQ → worker」的持久对应：新号分给负载最低的 worker，已在跑的号不迁移。纯哈希在扩容时
会搬走一部分号、打断会话。

## 本地联调

设环境变量 `DICE_OPENOPQ_MANIFEST_URL=http://127.0.0.1:<端口>/api/openopq/manifest` 可指向本机的
假中心（仅接受 `127.0.0.1` 的 http）。清单仍须用正式私钥签名，换得了地址、换不了内容。
