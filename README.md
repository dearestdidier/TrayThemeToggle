# 主题切换器（TrayThemeToggle）

任务栏托盘一键切换 Windows 深色 / 浅色模式的小工具。单文件、免安装、不需要管理员权限。

**UI 跟随系统主题**：深色模式下菜单和窗口都是深色的，不再出现白底弹窗。

## 功能

| 操作 | 效果 |
|---|---|
| **双击托盘图标** | 翻转当前主题 |
| **右键托盘图标** | 弹出菜单：切换主题 / 开机自启 / 启用快捷键 / **设置快捷键…** / 关于 / 退出 |
| **全局快捷键**（默认 `Ctrl+Alt+S`，可自定义） | 翻转当前主题：浅色→深色，深色→浅色 |
| **`--toggle`** | 静默翻转一次后退出，适合做快捷方式、脚本、Stream Deck 按钮 |
| **`--settings`** | 直接弹出快捷键设置窗口 |
| **`--diagnose`** | 自检，把结果写到 `diagnose.log` |

托盘图标会跟着状态变：**浅色 = 太阳 ☀，深色 = 月亮 ☾**。
鼠标悬停能看到「当前：浅色　|　右键菜单　|　Ctrl+Alt+S → 深色」这样的提示，里面的键名会跟着你自定义的组合变。

## 界面

设置窗口跟随主题：

| 浅色 | 深色 |
|---|---|
| ![设置-浅色](res/settings_light.png) | ![设置-深色](res/settings_dark.png) |

右键菜单同样跟随主题：

| 浅色 | 深色 |
|---|---|
| ![菜单-浅色](res/menu_light.png) | ![菜单-深色](res/menu_dark.png) |

![关于](res/about_light.png)

Win11 风格的细节：

- **DWM 圆角** + **深色标题栏**（`DWMWA_WINDOW_CORNER_PREFERENCE` / `DWMWA_USE_IMMERSIVE_DARK_MODE`）
- 标题栏、边框颜色与内容区一致，不会出现突兀的色带
- 圆角卡片式输入框和按钮，主按钮用系统强调色
- 菜单项悬停高亮、勾选标记、右对齐快捷键提示
- 全部自绘 + 双缓冲，不闪；按钮有 hover / pressed 状态
- 字体用 `Segoe UI Variable`（老系统自动回落 `Segoe UI`）
- Per-Monitor V2 DPI，缩放 150% / 200% 下依然清晰

> 为什么要自己画：Win32 的 `TrackPopupMenu` 和 `MessageBox` 完全不响应深色主题，
> 系统只给白底黑字。所以菜单和对话框都改成了 GDI 自绘，配色走同一套调色板。

## 自定义快捷键

**右键托盘图标 →「设置快捷键…」**，点一下输入框，然后**直接按下你想要的组合键**就行。

- 支持任意组合：`Ctrl` / `Alt` / `Shift` / `Win` + 字母、数字、F1~F12、方向键等
- 只按修饰键（比如只按住 Ctrl）时会显示 `Ctrl+…`，等主键按下才记录
- 按 `Esc` 取消，按 `Enter` 确认
- 「恢复默认」回到 `Ctrl+Alt+S`
- 设置会存到注册表，重启后保留

**默认为什么是 `Ctrl+Alt+S` 而不是 `Ctrl+S`？**
`Ctrl+S` 是几乎所有程序的「保存」，做成全局热键会导致你在 Word、VS Code 里根本按不出保存。
`Ctrl+Alt+S` 冲突面小得多。当然你也可以改成别的，比如 `Ctrl+Alt+D`、`F9`。

如果某个组合被别的程序占用了，保存时会弹窗提示注册失败，换一个即可。
菜单里的「启用全局快捷键」可以整体关掉快捷键，只用托盘双击。

配置存放位置：

```
HKCU\Software\TrayThemeToggle
    HotkeyModifiers  = 3       ← 位标志：Alt=1, Ctrl=2, Shift=4, Win=8
    HotkeyVk         = 83      ← 虚拟键码（83 = 'S'）
    HotkeyEnabled    = 1
```

（`Ctrl+Alt+S` → Modifiers=3, Vk=83；`Ctrl+Alt+T` → Modifiers=3, Vk=84）


## 直接使用（重要）

**不要双击 `build\TrayThemeToggle.exe`。**

那个文件位于 DSH 沙箱工作区内，沙箱会给工作区文件打上 `Low Mandatory Level` 完整性标签。
带这个标签的 exe 被双击时，Windows 会把进程降到 Low 完整性级别，于是：

- `Shell_NotifyIconW` 被 UIPI 拒绝 → **托盘没有图标**
- 写 HKCU 主题设置被拒绝 → **切换无效**

表现就是「双击了但什么也没发生」。

正确做法是跑一次部署脚本，把 exe 复制到工作区外：

```bat
deploy.bat
```

它会复制到 `%LOCALAPPDATA%\TrayThemeToggle\TrayThemeToggle.exe` 并询问是否立即启动。
复制到工作区外之后 Low 标签不再存在，进程以正常的 Medium 级别运行，一切正常。

之后就用 `%LOCALAPPDATA%\TrayThemeToggle\TrayThemeToggle.exe`，可以右键 → 发送到 → 桌面快捷方式。


想开机自启：右键托盘图标 → 勾选「开机自动启动」。
（它只是往 `HKCU\...\CurrentVersion\Run` 写一条，随时可以取消。）

## 自己编译

需要 MinGW-w64（`g++` + `windres` 在 PATH 里）。

```bat
build.bat
```

产物：`build\TrayThemeToggle.exe`，约 350 KB，静态链接，不依赖任何 DLL。

图标是从 `tools\gen_icons.py` 用 Pillow 生成的，想换风格改这个脚本重跑即可：

```bat
python tools\gen_icons.py
```

## 它改了什么

只动当前用户（HKCU）下的三个值，不需要管理员权限：

```
HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
    SystemUsesLightTheme = 0/1     ← 任务栏、开始菜单、系统 UI
    AppsUseLightTheme    = 0/1     ← 应用窗口
    ColorPrevalence      = 0       ← 切浅色时顺手关掉「标题栏显示主题色」
```

改完会广播三条系统消息（`WM_SETTINGCHANGE` + `ImmersiveColorSet`、`WM_THEMECHANGED`），
让资源管理器立刻重绘，**不用注销也不用重启 explorer**。

不联网、不写自启动（除非你点菜单）、不注入进程、不创建子进程。

## 关于快捷键冲突

早期版本固定用 `Ctrl+S`，会抢占所有程序的「保存」——在 Word、VS Code、记事本里按 Ctrl+S 都不会保存，而是切主题。

v1.1 起改成**可自定义**，默认 `Ctrl+Alt+S`：

- 想换键位：右键托盘图标 →「设置快捷键…」→ 点输入框 → 按下新组合 → 确定
- 想彻底关掉：右键托盘图标 → 取消勾选「启用全局快捷键」
- 设置会持久化，重启后保留

## 文件结构

```
tray-theme-toggle/
├── build.bat                  编译脚本
├── deploy.bat                 部署到 %LOCALAPPDATA%（绕开沙箱完整性标签）
├── src/
│   ├── main.cpp               全部逻辑
│   ├── app.rc                 资源脚本（图标 + 版本信息）
│   ├── resource.h             资源 ID
│   └── no-default-manifest.specs  MinGW 专用，避免 manifest 冲突
├── res/
│   ├── sun.ico / moon.ico / app.ico   多尺寸图标（16~256）
│   └── app.manifest           asInvoker + PerMonitorV2 DPI
├── tools/
│   └── gen_icons.py           图标生成脚本
└── build/
    └── TrayThemeToggle.exe    编译产物
```

## 实现上几个值得说的点

**单实例**：用 `Local\TrayThemeToggle_SingleInstance` 互斥体。重复启动会静默退出，不会出现两个托盘图标。

**资源管理器重启**：注册 `TaskbarCreated` 消息。explorer 崩溃重启后，托盘会自动把图标加回去，不用手动重开程序。

**DPI 变化**：监听 `WM_DPICHANGED` 和 `WM_DISPLAYCHANGE`，按新的 `SM_CXSMICON` 重新加载图标，多显示器不同缩放时也不发虚。

**广播要带超时**：`SendMessageTimeoutW(HWND_BROADCAST, ...)` 必须给超时值。这里踩过一个坑——
一开始用了 `SMTO_NOTIMEOUTIFNOTHUNG`，那个标志的含义是「只要对方没被判定为挂起就不超时」，
等于把超时关掉了，于是只要有一个程序不处理消息，整个进程就永久卡死。现在固定 400ms / 250ms。

**读不到注册表时**：`IsSystemLight()` 先读 `SystemUsesLightTheme`，读不到退回 `AppsUseLightTheme`，
再读不到按 Windows 出厂默认的「浅色」处理，不会因为权限问题崩溃。

## 环境限制说明

这份代码在沙箱里编译并**实测通过**（MinGW-w64 g++ 16.2.0，Windows 10.0 build 26300）：

| 项目 | 结果 |
|---|---|
| 托盘图标出现 | ✅ 实测通过（UIAutomation 读到 tooltip） |
| `--toggle` 切换主题 | ✅ 连续 6 次全部正确翻转 |
| GUI 运行时 `--toggle` | ✅ 正确翻转（转成窗口消息交给常驻实例处理，图标同步刷新） |
| **默认 Ctrl+Alt+S** | ✅ 模拟按键 3 次，主题翻转 3 次 |
| **旧 Ctrl+S 不再触发** | ✅ 按两次无反应（冲突已解除） |
| **自定义为 Ctrl+Alt+T** | ✅ 设置窗捕获按键 → 保存 → 立即生效，按 3 次翻转 3 次 |
| **自定义配置持久化** | ✅ 重启后仍是 Ctrl+Alt+T，且生效 |
| 单实例 | ✅ 重复启动进程数保持 1 |
| 图标资源加载 | ✅ 16/20/24/32/40/48/64/128/256 全部可加载 |
| 进程内存 | 10~18 MB（常驻托盘，正常） |
| **设置窗·浅色** | ✅ 截图验证：451 色、亮度 235、按钮强调色到位 |
| **设置窗·深色** | ✅ 截图验证：455 色、亮度 43、标题区 (32,32,32) |
| **菜单·浅色** | ✅ 截图验证：427 色、亮度 237、背景 (249,249,249) |
| **菜单·深色** | ✅ 截图验证：403 色、亮度 50、背景 (43,43,43) |
| **菜单点击生效** | ✅ 点第一项后主题真的翻转，菜单自动关闭 |
| **文字对比度** | ✅ 实测 14.16 ~ 18.23 : 1（WCAG AA 要求 ≥ 4.5） |
| **关于窗口高度自适应** | ✅ 由 200px 修正为 324px，分带检测确认底部无截断 |

> 截图检查是程序化的：统计颜色数、区域平均色、边缘像素密度、WCAG 对比度、底部分带，
> 避免"看着像渲染了其实是空白"的误判。

唯一的坑是前面说的完整性标签：**工作区内的副本不能用**，必须 `deploy.bat` 复制出去。

如果部署后还有问题，跑自检：

```bat
"%LOCALAPPDATA%\TrayThemeToggle\TrayThemeToggle.exe" --diagnose
```

看 `%LOCALAPPDATA%\TrayThemeToggle\diagnose.log`，重点确认：

- `[进程权限]` 是「中 (Medium，正常)」
- `[托盘]` 是「已成功加入托盘并移除」
- `[注册表写入]` 是「OK」

三项都对就一定能用。

