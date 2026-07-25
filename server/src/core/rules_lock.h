#pragma once
// ─── Dice!Next — 规则包注册表读写锁 ───────────────────────────
// 同义词表(card_store aliasRegistry)、衍生公式表(command_router computedRegistry)、
// 规则包元数据表(command_router rulePacks) 在「启动加载」和「WebUI 热重载」时被写，
// 在消息处理线程(canonical/attrMax/rulePackByKey)被读。用一把共享读写锁守护：
// 读者 shared_lock、重载者 unique_lock。低层写入函数本身不加锁（由重载者持独占锁
// 调用，或启动期单线程调用），避免非递归 mutex 的自锁。
#include <shared_mutex>

namespace dice {
inline std::shared_mutex& rulesLock() {
    static std::shared_mutex m;
    return m;
}
}  // namespace dice
