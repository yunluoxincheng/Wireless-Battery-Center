# Windows 设备管理面板

## Wireless Battery Center P0

设备档案跨后台和面板重启保留。离线设备可继续查看详情、最后在线时间、最后有效电量和官方驱动入口，也可隐藏或确认移除。无电池、电量未知和不支持读取各自独立显示。电量历史曲线属于后续 P1。[项目定位](product-positioning.md) · [P0 开发任务](codex-device-center.md)

界面沿用 Gemini 原型 `wireless-battery.zip` 与现有 Liquid Glass QML。设备卡片、圆环、详情导航、自定义标题栏和浅深主题延续原有实现，详情内新增官方驱动和档案信息。

面板由 `WirelessBatteryCenterPanel.exe` 提供。后台 `WirelessBatteryCenter.exe` 负责硬件轮询、Windows 任务栏和托盘展示，以及保存设备设置。面板通过当前登录会话专用的 Windows 本地命名管道读取快照和提交更改。设备总览根据窗口宽度调整列数，选择设备可查看独立设置页。Lucide SVG 随面板内嵌，构建目录中的 `LUCIDE-LICENSE.txt` 保留许可证。

## 构建和运行

在项目根目录用 PowerShell 执行。需要 Qt 6.8 或以上的 MinGW 64 位 SDK，包含 Qt Quick、Quick Controls 和 Linguist Tools。使用与 SDK 匹配的 MinGW 工具链，并让 CMake、Ninja 和 Qt 工具可从 `PATH` 找到。本机验证使用 Qt 6.8.3、MinGW 13.1.0 和 CMake 3.31.10。

```powershell
cmd.exe /d /c build.bat
$qt = 'D:/DevCache/Qt/6.8.3/mingw_64'
cmake -S . -B build/panel -G Ninja "-DCMAKE_PREFIX_PATH=$qt" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build/panel --parallel 8
cmake --build build/panel --target WirelessBatteryCenterPanel_qmllint DriverLauncherTest
& "$qt/bin/windeployqt.exe" --release --qmldir panel build/panel/WirelessBatteryCenterPanel.exe
Copy-Item -LiteralPath WirelessBatteryCenter.exe -Destination build/panel/WirelessBatteryCenter.exe
```

启动 `build\panel\WirelessBatteryCenterPanel.exe` 即可使用。面板会自动启动同目录的后台，并在 IPC 可用后显示设备。托盘菜单或任务栏小组件中的 **打开设备管理面板** 也可打开面板。

- 同一 Windows 登录会话各运行一个后台和面板。重复打开面板会恢复原窗口。
- 关闭面板窗口后，后台继续在任务栏或托盘运行。面板全局设置中的 **退出 Wireless Battery Center**、原菜单的 **退出** 和后台的 `--quit` 都会关闭两个进程。
- 开机自启动仍运行轻量后台。演示时先结束当前版本，启动 `WirelessBatteryCenter.exe --demo`，再打开面板。
- 面板默认跟随 Windows 的应用模式。可在面板的 **全局设置 → 外观** 中固定为浅色或深色，所选模式会保留到下次启动。
- 安装版和便携版使用同一套中心配置及单实例标识。上一版 Wireless Battery 仍使用独立的名称、IPC 和配置。安装、完整部署与打包方式见 [Windows 发布文档](windows-release.md)。

如果使用 Git for Windows 和 Qt MinGW，可在 PowerShell 中调用 Makefile 构建和运行已有测试。

```powershell
& 'C:\Program Files\Git\bin\bash.exe' -c 'mingw32-make SHELL=sh -j8'
& 'C:\Program Files\Git\bin\bash.exe' -c 'mingw32-make SHELL=sh test'
$env:PATH = "$qt/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$qt/plugins"
$env:QT_FORCE_STDERR_LOGGING = '1'
& .\build\panel\DriverLauncherTest.exe
```

## 数据和设置

- **名称与排序**共用后台配置。别名留空会恢复设备报告的名称。面板排序会反映到任务栏设备顺序。
- **可见性**可分别控制面板和任务栏／托盘。启用「显示隐藏设备」即可在面板里找回这些设备。
- **低电量阈值**默认为 15%，可设为 1–100%。它影响电量配色，并沿用托盘模式的低电量通知。
- **低电量系统通知**默认开启，可在全局设置关闭。此选项由后台保存，控制托盘模式的 Windows 通知。电量配色仍使用低电量阈值。
- **最后读数**保存有效电量、来源和时间。离线、断开后台或只有缓存读数时，电量环呈灰色并标记「上次读数」。Windows 蓝牙属性没有测量时间，面板保留最近观测到的属性值及其变化时间。仅检测到接收器时，状态为「无线状态待确认」。
- **档案存储**位于 `HKCU\Software\WirelessBatteryCenter\Backend\DeviceProfiles`。每台设备使用独立、带版本的注册表值，在有效数据变化时原子写入。仅时间变化时按最多五分钟一次合并保存，离线转换和正常退出时补写。损坏或未知版本的记录会提示读取问题，未来版本的值保留原样。
- **设备身份**优先使用验证过的设备单元 ID、蓝牙设备容器或 Windows HID 物理容器与产品端点。多个 HID 集合共用同一个已确认端点。旧别名、排序和可见性沿用原注册表项，旧键只在单一实例有在线证据时建立绑定。端口关联的容器可能随换口变化，缺少共同物理身份的连接模式会各自保留。只能识别槽位或产品的上游输出保留会话状态，等待可靠的实例证据后才归档。
- **无电量设备**包括 Windows 已报告连线且分类明确的蓝牙外设，以及已核实的 USB 型号。初始 USB 清单涵盖 HECATE G5 无线版接收器、Dell KB216 和部分 Logitech G102/G203。发现型记录只显示在面板中，原任务栏和托盘继续显示原电量来源。[USB 型号记录](https://usb-ids.gowdy.us/read/UD/046d)
- **演示验证**可在后台启动时传入 `--demo`。面板顶部标记演示模式，示例档案及设置保存至 `HKCU\Software\WirelessBatteryCenter\Demo`。

### 从 Wireless Battery 迁移

首次启动时，中心补齐新版配置中缺少的旧版档案、别名与身份绑定、显示和排序偏好、阈值及任务栏设置。面板单独补齐外观设置。旧版数据以只读方式访问，已有新版值原样保留。迁移全部成功后才写入完成标记，中断后下次启动从缺少的值继续。手动指定的驱动 EXE 路径和开机启动项需在中心重新选择。原有自定义语言文件仍可读取，新建文件保存在 `%APPDATA%\WirelessBatteryCenter\lang`。

确认移除离线档案时，中心同时清理该设备及其已绑定旧键的别名、隐藏、排序和身份绑定。若清理中断，下次启动会先完成待处理操作，再登记设备。卸载保留中心的档案和设置，重装继续读取同一份数据。

## 官方驱动

详情页优先打开已安装的厂商客户端。自动发现读取 Windows 已安装应用注册信息，并核对应用名、发布者和本地 EXE 名称。也可选择自己的官方客户端 EXE，路径按驱动保存，随时可恢复自动检测。客户端不可用或进程启动失败时，入口依次选择官方网页驱动和支持页。网页入口由系统浏览器打开，WebHID 可能需要 Chrome 或 Edge 和用户授权。

| 设备 | 本地客户端 | 网页驱动或支持 |
| --- | --- | --- |
| ATK F1 PRO MAX 支持的 1K / 8K 接收器、ATK V75X | ATK HUB | [网页驱动](https://desktop.atkgear.com/) · [官方说明](https://www.atk.store/blogs/news/atk-v-hub-instructions) |
| HECATE G5 无线版 | HECATE | [漫步者 HECATE 下载](https://cnweb.edifier.com/driver.html) |
| VGN V98Pro V2 | VGN V-HUB | [VGN 驱动下载](https://download.vgnlab.com.cn/) |
| Logitech G102/G203（已登记的 USB 型号） | Logitech G HUB | [Logitech G HUB](https://www.logitechg.com/innovation/g-hub.html) |

## 验证记录

### 外设管理 P0（2026-11-20）

- Windows 11、Qt 6.8.3、MinGW 13.1.0。Win32 Makefile、`build.bat`、Qt Quick CMake 构建、`qmllint` 与 `windeployqt` 通过。Makefile 的 20 项旧测试与新增档案测试通过，Qt 驱动启动测试通过。便携版位于 `dist/WirelessBatteryCenter-P0-20261120`，移除 Qt SDK 的 PATH 配置后仍成功打开原生窗口。
- 档案测试覆盖不同容器的同型号设备、同容器的多 HID 集合、旧版本和损坏数据、有效读数与缓存、移除及重新登记。隔离 Windows 后台通过本地 IPC 实测旧别名绑定、排序和可见性持久化、离线过重启、线上拒绝移除、同型号独立档案、移除后再发现、面板关闭后后台继续，以及断线重连后详情选择稳定。
- 真实设备验证使用独立注册表配置。ATK F1 PRO MAX（`373B:1015`）两次分别返回 95% 与 100%，均报告充电。iQOO TWS 1i 由 Windows 蓝牙报告在线，30% 显示为上次读数。VGN V98Pro V2（`320F:5088`）在此次读取中电量未知。HECATE G5 无线版接收器（`35BB:A008`）识别为不支持电量读取且无线状态待确认。实机通过手动刷新、档案关联重启、关闭面板以及任务栏与托盘切换。测试结束恢复原来运行的后台。
- 隔离驱动测试覆盖注册表发现、手动路径优先及保存、无效路径、已删除路径、进程创建失败和官方网页／支持回退。原生面板另确认本机 ATK HUB 自动发现。测试用浏览器处理器接收 URL，厂商软件启动和 WebHID 授权留待实物操作。
- 原生截图位于本地 `build/device-center-qa`。检查了浅色实机总览、浅色单设备和驱动详情、深色离线详情及断线重连，以及 520×460 最小、860×648 常规和最大化窗口。验证了原生标题栏拖动、按钮命中、边角缩放区、最大化／还原、详情返回和通过确认弹窗移除离线档案。实物拔插、实体同型号双设备、纯有线无电池设备和低电量系统弹窗仍待实机验证。

### Liquid Glass 迁移

- 参考原型位于 `design-reference/wireless-battery/`。面板采用 Windows 11 DWM Acrylic 作为整窗背板，以 Qt Quick 绘制共享柔光、渐变边缘和九宫格阴影。旧版 Windows 或 DWM 拒绝材质时，使用 Qt 原生背板。
- 电量环使用驱动读数与后台阈值。阈值内使用琥珀色，其下半区使用红色标明严重低电量，通知仍使用完整的原有阈值。缓存环为灰色并带虚线，未知读数仅显示轨道。跨行拖拽由后台按两个稳定设备键一次写入既有排序配置。
- 用上述命令将输出目录设为 `build/panel-liquid`，在 Qt 6.8.3、MinGW 13.1.0 下完成面板构建、`WirelessBatteryPanel_qmllint` 和 `windeployqt`。Win32 Makefile 构建及 20 项既有测试通过。部署目录同时提供新版面板和后台，拖拽保存需配套新版后台。
- 真实设备 ATK F1 PRO MAX 通过 2.4 GHz 接收器显示 100%、充电和 `373B:1015`。验证面板自动启动后台、手动刷新、关闭面板后后台继续运行。隔离演示验证跨行拖拽、排序重启持久化、阈值、别名、面板隐藏、任务栏可见性、任务栏／托盘切换和断开重连。演示设置写入隔离配置。
- Windows 11 实测标题栏移动、边缘缩放、最大化到工作区、最小化后单实例唤回、Win+左键贴靠和最大化按钮的 Snap Layout 浮层。150% Qt 缩放验证了图标、圆环和窗口按钮命中。
- 资源采样在 Windows 11、RTX 4060 Laptop、860×648 窗口下完成。单台真实设备静置、16 台演示设备静置、16 台演示设备持续拖拽的面板进程 CPU 占整机分别为 0.02%、0.00%、1.95%，工作集分别约为 166、187、193 MiB，GPU 3D 峰值分别为 0%、0%、7%。采样仅统计面板进程。

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
