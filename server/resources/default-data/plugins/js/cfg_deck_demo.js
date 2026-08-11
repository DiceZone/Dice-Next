// ==UserScript==
// @name         示例：配置项与牌堆
// @author       DiceNext
// @version      1.0.0
// @description  测试 register/getStringConfig 与 seal.deck.draw：.jstest
// ==/UserScript==

if (!seal.ext.find('jstest')) {
  const ext = seal.ext.new('jstest', 'DiceNext', '1.0.0');
  seal.ext.register(ext);
  seal.ext.registerStringConfig(ext, '问候语', '你好');

  const cmd = seal.ext.newCmdItemInfo();
  cmd.name = 'jstest';
  cmd.help = '.jstest — 测试配置项与牌堆';
  cmd.solve = (ctx, msg, cmdArgs) => {
    const greet = seal.ext.getStringConfig(ext, '问候语');
    const card = seal.deck.draw(ctx, '数字', false);
    seal.replyToSender(ctx, msg, `${greet}，${ctx.player.name}！抽到数字：${card}`);
    return seal.ext.newCmdExecuteResult(true);
  };
  ext.cmdMap['jstest'] = cmd;
  seal.ext.register(ext);
}
