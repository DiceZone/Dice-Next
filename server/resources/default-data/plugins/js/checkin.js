// ==UserScript==
// @name         示例：打卡
// @author       DiceNext
// @version      1.0.0
// @description  测试持久化变量：.打卡 累计次数（seal.vars）
// ==/UserScript==

if (!seal.ext.find('checkin')) {
  const ext = seal.ext.new('checkin', 'DiceNext', '1.0.0');

  const cmd = seal.ext.newCmdItemInfo();
  cmd.name = '打卡';
  cmd.help = '.打卡 — 累计你的打卡次数';
  cmd.solve = (ctx, msg, cmdArgs) => {
    let [n] = seal.vars.intGet(ctx, '$m打卡次数');
    n = (n || 0) + 1;
    seal.vars.intSet(ctx, '$m打卡次数', n);
    seal.replyToSender(ctx, msg, `${ctx.player.name} 打卡成功，这是第 ${n} 次！`);
    return seal.ext.newCmdExecuteResult(true);
  };
  ext.cmdMap['打卡'] = cmd;
  seal.ext.register(ext);
}
