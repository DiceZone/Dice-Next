# Milky Adapter

Dice!Next connects to a Milky implementation through its HTTP API and accepts
incoming events through a manually configured WebHook. The API token and the
WebHook token are separate credentials.

Create an adapter with `type: "milky"`. Its required fields are `endpoint`
and `accessToken`; the connection mode is always `http`.

```json
{
  "name": "Milky QQ",
  "type": "milky",
  "connection_mode": "http",
  "endpoint": "http://127.0.0.1:8080",
  "accessToken": "milky-api-token",
  "webhook_base_url": "https://dice.example.com",
  "webhook_token": "milky-webhook-token",
  "enabled": true
}
```

After creating the adapter, configure the Milky implementation manually with:

```text
POST https://dice.example.com/milky/webhook/<adapter-id>
Authorization: Bearer milky-webhook-token
Content-Type: application/json
```

`<adapter-id>` is the Dice!Next adapter ID returned by the adapter API or shown
in the Web UI. Dice!Next does not register this WebHook remotely.

Outbound CQ and SealDice media aliases are translated to Milky segments:

- `[CQ:image,...]`, `[img,...]`, `[图片:...]`, `[图:...]` -> `image`
- `[CQ:record,...]`, `[CQ:voice,...]`, `[voice,...]` -> `record`
- `[CQ:video,...]`, `[video,...]` -> `video`
- `[CQ:at,...]`, `[CQ:face,...]`, `[CQ:reply,...]` -> native mention, face and reply segments
- `[CQ:file,...]`, `[file,...]`, `[文件:...]` -> group-file upload (group messages only)

Unsupported CQ segments are sent as literal text so message content is not
silently lost.
