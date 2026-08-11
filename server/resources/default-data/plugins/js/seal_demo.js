// ==UserScript==
// @name         示例：召唤海豹
// @author       希亚
// @version      1.0.0
// @description  海豹 JS 插件兼容性测试：.seal [名字]
// ==/UserScript==

if (!seal.ext.find('test')) {
  const ext = seal.ext.new('test', '希亚', '1.0.0');

  const cmdSeal = seal.ext.newCmdItemInfo();
  cmdSeal.name = 'seal';
  cmdSeal.help = '召唤一只海豹，可用 .seal <名字> 命名';
  cmdSeal.solve = (ctx, msg, cmdArgs) => {
    let name = cmdArgs.getArgN(1);
    if (name === 'help') {
      const ret = seal.ext.newCmdExecuteResult(true);
      ret.showHelp = true;
      return ret;
    }
    if (!name) name = '氪豹';
    seal.replyToSender(ctx, msg, `${ctx.player.name} 抓到一只海豹，取名为「${name}」！`);
    return seal.ext.newCmdExecuteResult(true);
  };

  ext.cmdMap['seal'] = cmdSeal;
  seal.ext.register(ext);
}
