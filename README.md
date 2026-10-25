# ZZZ-Gacha 绝区零抽卡工具
Gacha tracker and visualizer for Zenless Zone Zero. Built with C++20 & Win32 API.
《绝区零》调频(抽卡)数据保存，分析与可视化。使用C++20与Win32 API高效处理数据。
## How to use 如何使用
1. Run `main.exe` (the exporter) and input your gacha link.
   运行用于保存抽卡数据的 `main.exe` 主程序，并输入你的抽卡链接。
2. You can find the `uigf_zzz.json` gacha data saved by `main.exe` in the current running directory.
   你可以在运行目录中找到 `main.exe` 程序保存的 `uigf_zzz.json` 文件。
3. Run `gui.exe` (the analyzer) and drag `uigf_zzz.json` onto the window.
   运行用于分析与可视化抽卡数据的 `gui.exe` 图形界面程序，并将 `uigf_zzz.json` 拖拽到程序窗口中。

> [!IMPORTANT]
> The official Signal Search record API only returns a limited recent period (usually about 6 months
> for HoYoverse games). Older records are dropped and can never be fetched again. `main.exe` merges each
> run into the existing `uigf_zzz.json` incrementally, so run it regularly and keep that file — it is the
> only long-term archive of your pulls. If anything goes wrong during a run, the whole update is cancelled
> and the file is left untouched — just run it again.
>
> 官方调频记录接口只能查询最近一段时间的记录（米哈游系游戏通常为 6 个月），更早的记录会被丢弃且无法再取回。
> `main.exe` 是增量合并到已有的 `uigf_zzz.json` 的，所以请定期运行并保留该文件 —— 它是你抽卡历史的唯一长期存档。
> 运行过程中任何一步出错，整次更新都会取消，文件保持原样，重新运行即可。

> [!NOTE]
> Since version 2.5 the game has two more channels: Exclusive Rescreening (agent reruns) and W-Engine
> Reverberation (W-Engine reruns). They are exported with `gacha_type` `"102"` and `"103"`, the values the
> official API uses for them. The UIGF v4.2 standard only lists `1` / `2` / `3` / `5` for Zenless Zone Zero
> so far, so other UIGF tools may not recognize these two values yet.
>
> The `time` of each record is the server's local time exactly as the API returns it; `timezone` is set
> from your server region (China, Asia and TW/HK/MO: UTC+8, Europe: UTC+1, America: UTC-5).
>
> 2.5 版本起新增了「独家重映」（代理人复刻）与「音擎回响」（音擎复刻）两个频段，导出时 `gacha_type` 写作官方接口
> 使用的 `"102"` 与 `"103"`。UIGF v4.2 标准目前只为绝区零列出了 `1` / `2` / `3` / `5`，其它 UIGF 工具可能暂时不认识
> 这两个值。
>
> 每条记录的 `time` 是接口原样返回的区服服务器当地时间；`timezone` 按区服写入（国服、亚服、港澳台服为 UTC+8，
> 欧服为 UTC+1，美服为 UTC-5）。

> [!TIP]
> In `gui.exe`, an S-Rank pull that is not in the non-featured lists counts as the featured (UP) one.
> Since version 3.1, Custom Signal Search lets you choose the off-banner S-Ranks, and Caesar, Yanagi and
> Zhu Yuan (plus their signature W-Engines) joined the choices, so list entries can carry a time window in
> server time: `name` (never featured), `name@2026-07-28 18:00` (not featured from then on),
> `name@from~to` (not featured within that period; either end may be omitted). The same name may appear
> more than once. The Rescreening and Reverberation channels use the same two lists; their first S-Rank of
> each period is guaranteed to be featured, so their featured side is shown as plain statistics without
> a 50/50 win rate or a K-S test.
>
> `gui.exe` 里，S 级不在「非UP S级代理人 / 非UP S级音擎」名单里即视为当期 UP。3.1 版本起「定制调频」可以自选歪出的
> S 级，可选范围多了 凯撒 / 柳 / 朱鸢 及其专属音擎，因此名单条目可以带时间（服务器时间）：`名字`（任何时候都不是 UP）、
> `名字@2026-07-28 18:00`（该时刻起不是 UP）、`名字@起~止`（只在该时段内不是 UP，起或止可省略）；同一名字可以写多条。
> 重映 / 回响频段共用这两份名单；它们每期首个 S 级必为 UP（福利保底），所以 UP 一侧只给描述统计，不算不歪率、不做 K-S 检验。

## How to compile 如何编译
1. Download and install [Build Tools for Visual Studio 2026](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026).
   下载并安装 [Visual Studio 2026 生成工具](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026)。
2. Open the **"x64 Native Tools Command Prompt for VS"** application.
   打开 **"x64 Native Tools Command Prompt for VS"** 应用。
3. Go to the folder that contains `main.cpp`, `gui.cpp` and `JsonScan.h` (all three are required).
   进入存放 `main.cpp`、`gui.cpp` 与 `JsonScan.h` 的文件夹（三个文件缺一不可）。
4. Copy the command from `Compile.txt` and paste it into the command prompt, then press Enter to run.
   打开 `Compile.txt`，把命令复制粘贴到命令行应用中，按下回车运行。
## Compatibility 兼容性
### Windows
- **System 系统**: Windows 10 or higher (视窗 10 或更高版本)
- **Minimum System 最低系统**: Windows 7 SP1 with installed [Microsoft Visual C++ Redistributable](https://visualstudio.microsoft.com/downloads/#microsoft-visual-c-v14-redistributable).
- **CPU 处理器**: x86, x86_64, and arm64 (32-bit and 64-bit / 32位 与 64位)
> ### Apple (macOS & iOS)
> Please check the Apple SwiftUI version here 请查看该SwiftUI版本: [ZZZ-Gacha-Apple](https://github.com/shangyuan0818/ZZZ-Gacha-Apple)
## Demonstration 效果展示
<img width="947" height="878" alt="image" src="https://github.com/user-attachments/assets/a210ea65-72b4-461f-9558-21f824cfbca2" />
