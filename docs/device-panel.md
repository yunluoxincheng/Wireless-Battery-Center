# Windows 设备管理面板

面板由 `WirelessBatteryPanel.exe` 提供。后台 `WirelessBattery.exe` 负责硬件轮询、Windows 任务栏和托盘展示，以及保存设备设置。面板通过当前登录会话专用的 Windows 本地命名管道读取快照和提交更改。设备总览根据窗口宽度调整列数，选择设备可查看独立设置页。Lucide SVG 随面板内嵌，构建目录中的 `LUCIDE-LICENSE.txt` 保留许可证。

## 构建和运行

在项目根目录用 PowerShell 执行。需要 Qt 6.8 或以上的 MinGW 64 位 SDK，包含 Qt Quick、Quick Controls 和 Linguist Tools。使用与 SDK 匹配的 MinGW 工具链，并让 CMake、Ninja 和 Qt 工具可从 `PATH` 找到。本机验证使用 Qt 6.8.3、MinGW 13.1.0 和 CMake 3.31.10。

```powershell
cmd.exe /d /c build.bat
$qt = 'D:/DevCache/Qt/6.8.3/mingw_64'
cmake -S . -B build/panel -G Ninja "-DCMAKE_PREFIX_PATH=$qt" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build/panel --parallel 8
& "$qt/bin/windeployqt.exe" --release --qmldir panel build/panel/WirelessBatteryPanel.exe
Copy-Item -LiteralPath WirelessBattery.exe -Destination build/panel/WirelessBattery.exe
```

启动 `build\panel\WirelessBatteryPanel.exe` 即可使用。面板会自动启动同目录的后台，并在 IPC 可用后显示设备。托盘菜单或任务栏小组件中的 **打开设备管理面板** 也可打开面板。

- 同一 Windows 登录会话各运行一个后台和面板。重复打开面板会恢复原窗口。
- 关闭面板窗口后，后台继续在任务栏或托盘运行。面板全局设置中的 **退出 Wireless Battery**、原菜单的 **退出** 和后台的 `--quit` 都会关闭两个进程。
- 开机自启动仍运行轻量后台。演示时先结束当前版本，启动 `WirelessBattery.exe --demo`，再打开面板。
- 面板跟随 Windows 的应用模式。在 **设置 → 个性化 → 颜色** 中切换浅色或深色。选择「自定义」时，面板跟随「选择默认应用模式」。
- 试用新版前先退出已安装版本，并保留其安装目录，以便切回。上述命令把新版运行文件集中在 `build\panel`。

如果使用 Git for Windows 和 Qt MinGW，可在 PowerShell 中调用 Makefile 构建和运行已有测试。

```powershell
& 'C:\Program Files\Git\bin\bash.exe' -c 'mingw32-make SHELL=sh -j8'
& 'C:\Program Files\Git\bin\bash.exe' -c 'mingw32-make SHELL=sh test'
```

## 数据和设置

- **名称与排序**共用后台配置。别名留空会恢复设备报告的名称。面板排序会反映到任务栏设备顺序。
- **可见性**可分别控制面板和任务栏／托盘。启用「显示隐藏设备」即可在面板里找回这些设备。
- **低电量阈值**默认为 15%，可设为 1–100%。它影响电量配色，并沿用托盘模式的低电量通知。
- **最后读数**标明缓存、离线前的读数或 IPC 断开前的读数。电量更新时间在收到新读数时更新。部分驱动仅在确认充电或休眠时提供状态，这类设备的其余状态显示为「未知」。设备拔除后，其离线记录显示到本次后台退出。
- **演示验证**可在后台启动时传入 `--demo`。面板顶部会标记演示模式，设备电量是示例值。演示模式使用同一套设置存储。

## 验证记录

### 应用图标和标题栏

- 修复了 Windows 客户区大于 Qt 绘制区域导致的右侧和底部黑带。复查时，浅色和深色的普通、最大化及缩放窗口中，两者尺寸一致。Windows 原生截图确认绘制区域覆盖窗口。
- Qt Quick 构建和 QML 检查通过。Win32 可从面板 EXE 提取上游图标，Qt 窗口也提供原生图标。浅色和深色顶栏均使用 nik-txt 的原始 SVG。
- 实际 Qt 窗口截图覆盖浅色、深色、最大化和缩放。脚本验证原生拖动区及八个缩放边角的命中、最大化到工作区、双击标题栏还原、最小化和还原，以及关闭面板后当前后台继续运行。
- 通过短暂占用本地 IPC 的测试连接复现断线。提示出现，缓存电量标为上次读数。连接恢复后提示自动消失。测试没有重启后台或修改用户设置。
- 已确认最大化按钮返回 Windows 所需的 `HTMAXBUTTON`。Win+方向键贴靠、Snap Layout 浮层和跨 DPI 显示器拖动尚未在桌面实测。

### 界面改版与统一入口

- 本轮通过 MinGW Makefile 后台构建、Qt Quick CMake 构建、QML 检查和 Makefile 的 20 项已有测试。
- 实际运行 Qt 面板并截图检查了浅色下的 ATK F1 PRO MAX、深色下的 16 台示例设备，以及 990×690 和 600×460 窗口。修正了深色图标着色、详情页信息密度和调整窗口大小时的页面重叠。最终用 Qt 截图脚本复查总览、详情、返回和刷新后的页面。
- 直接启动新版面板会拉起同目录后台。重复启动保留原有两个进程并恢复最小化的面板。关闭面板后，后台仍能响应 IPC。面板全局退出与后台 `--quit` 均已验证可结束两个进程。裸启后台未弹出面板。
- 测试后已恢复原安装版本，用户设置与测试前一致。本轮未重新登录 Windows 验证开机自启动，也未重新写入用户设置进行持久化测试。

### 第一版

2026-11-19 在 Windows 11 上验证。

| 范围 | 结果 |
| --- | --- |
| 构建 | Win32 `build.bat`、Makefile 和独立 Qt Quick CMake 构建通过。 |
| 已有测试 | Makefile 的 20 项测试全部通过。 |
| 实机 | ATK F1 PRO MAX 通过 2.4 GHz 接收器识别，显示 100%、驱动报告的充电状态、`373B:1015` 和更新时间。面板发起的手动刷新由原后台完成。 |
| 设置 | 在面板编辑中文别名、面板隐藏、任务栏／托盘可见性、显示模式及阈值。后台重启后读回设置，面板自动重连。任务栏小组件在切回任务栏模式后重新可用。 |
| 进程 | 后台退出后面板标记上次读数。关闭面板进程后，后台仍能完成刷新。实机验证结束后恢复了已安装后台及原设置。 |
| 演示 | 16 台示例设备验证了排序、排序持久化、休眠、离线和未知读数。 |

实机仅覆盖上述鼠标。实体设备的拔插、充电／休眠状态切换和低电量弹窗还需实物验证。桌面检查使用了简体中文深色界面。原生菜单入口按原有菜单处理路径接入，尚未通过直接点击任务栏和托盘菜单逐一确认。
