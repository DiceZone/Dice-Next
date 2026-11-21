#pragma once

// QQ 官方机器人协议字段解析位于 CatOfficial（https://github.com/DiceZone/CatOfficial）。
// 保留原路径与 dice::qqprotocol 名称，Dice!Next 其余代码无需改动。
#include "../common/utils.h"
#include "../message/qq_official_message.h"
#include <catofficial/protocol.h>

namespace dice {
namespace qqprotocol = catofficial::protocol;
}  // namespace dice
