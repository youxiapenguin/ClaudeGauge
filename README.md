# ClaudeGauge

在 **Windows 桌面 / 任务栏**实时显示 **Kimi for Coding**(K2/K3 编程订阅)的用量:5 小时滑动窗口、本周配额、总池,以及并发会话数。

> 第三方小工具,**非月之暗面官方产品**。作者:初见
> v2.0 起数据源由 Claude Code 切换为 Kimi for Coding(账号原因),并彻底移除 WSL/Python 依赖,改为纯 Windows 原生。

![ClaudeGauge 演示](docs/demo-hero.gif)

## 功能

- **两种形态**(右键菜单切换,记忆选择):
  - 悬浮窗:无边框圆角卡片,可拖动、置顶、调透明度
  - 任务栏:嵌进任务栏的精简一行(仅 Windows 10)
- **7 套配色主题**:红 / 橘黄 / 蓝 / 绿 / 墨绿 / 紫 / 橙黄(标题深、正文同色系浅)
- **倒计时**:5 小时窗口显示"剩 Xh Ym",本周显示"剩 N天",每分钟本地递减
- **系统托盘图标**:悬停看三项用量 + 重置时间;右键弹出全部设置
- 透明度可调、总是置顶、锁定位置、**开机自启**
- **多账户/备用密钥**:在 `config.ini` 里加 `api_key_2`、`api_key_3`… 即可同时显示多个 Kimi 账户。每个 key 可配 `name` / `name_2` 作为显示名（如"大号"/"备用"）。只配一个 key 时界面和原来一样;多 key 时悬浮窗自动纵向拉长、任务栏条自动横向扩展。
- 套餐等级自动识别(如 Kimi 高级版),附并发会话数(如 并发 2/30)

## 运行前提

1. **能联网**(直连 `api.kimi.com` 即可,无需代理)
2. 在 exe 同目录的 `config.ini` 里填上 Kimi for Coding 的 API Key:

```ini
[kimi]
api_key  = sk-大号key
name     = 大号
api_key_2 = sk-备用key
name_2   = 备用
```

需要更多账户继续加 `api_key_3`、`name_3`… 即可(目前支持到 `api_key_10`)。

> Key 即 Claude Code 接入 Kimi 时用的 `ANTHROPIC_API_KEY`(Kimi for Coding 订阅颁发)。
> `config.ini` 已在 `.gitignore` 里,key 不会进仓库。

## 使用

1. 下载本仓库(Code → Download ZIP),解压。
2. 双击 `ClaudeGauge.exe`。
3. 在窗口或托盘"K"图标上**右键**进行设置。

## 系统支持

| | 悬浮窗模式 | 任务栏嵌入模式 |
|---|---|---|
| Windows 10 | ✅ | ✅ |
| Windows 11 | ✅ | ❌(Win11 任务栏架构不同) |

Win11 用户请使用悬浮窗模式。

## 刷新频率与额度消耗

- **每 10 分钟自动刷新一次**;取数失败时每 60 秒重试,恢复后回到 10 分钟节奏。倒计时在两次刷新之间本地每分钟递减,不走网络。
- **刷新不消耗你的 Kimi 额度**:`GET /coding/v1/usages` 是纯查询接口(实测连续两次调用用量数字纹丝不动),额度只在你真正和模型对话时才消耗。

## 原理

纯 Windows 原生:WinINet 发 HTTPS 请求 `GET https://api.kimi.com/coding/v1/usages`(带 `Authorization: Bearer <key>`),nlohmann/json 单头文件解析返回。接口数值全是字符串形式的百分比:`usage`=本周配额、`limits[0]`=5 小时滑动窗口、`parallel`=并发会话、`totalQuota`=总池。

## 从源码编译

WSL 里:`sudo apt install -y mingw-w64`,然后 `bash build.sh`(用 mingw-w64 交叉编译为 Windows x64)。

## 鸣谢

[nlohmann/json](https://github.com/nlohmann/json) · [mingw-w64](https://www.mingw-w64.org/) · WinINet · GDI+

## License

[MIT](LICENSE)
