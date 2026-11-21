# Windows 构建与发布

## 本地构建安装包和便携版

在项目根目录使用 PowerShell 7。需要 Git for Windows、CMake、Ninja、Qt 6.8.3 MinGW x64 SDK、对应的 MinGW-w64 13.1.0 工具链和 Inno Setup。GitHub Actions 使用 MSYS2 的 GNU Make 和 Inno Setup 6.7.4，两个 EXE 仍使用同一套 Qt MinGW 编译器。脚本从 `res/version.h` 读取版本，并核对两个 EXE 的版本资源。

```powershell
$params = @{
    QtDir        = 'D:/DevCache/Qt/6.8.3/mingw_64'
    MingwDir     = 'D:/DevCache/Qt/Tools/mingw1310_64'
    InnoCompiler = 'C:/Program Files (x86)/Inno Setup 6/ISCC.exe'
    Version      = '0.1.0'
    RunTests     = $true
}
& ./installer/build-installer.ps1 @params
```

修改示例路径以匹配本机工具目录。默认输出 `dist/release-v0.1.0`。重复构建时通过 `-OutputDir` 指定空闲路径，脚本会保留旧产物。项目根目录的 `build.bat` 和 Makefile 仍可单独生成 `WirelessBatteryCenter.exe`。

打包脚本依次编译后台和面板，用 `windeployqt` 部署 MinGW 运行时、Qt DLL、Windows 平台插件和 QML 模块，再把同一暂存目录交给安装器和 ZIP。HeadsetControl 4.3.0 二进制及其对应源码均使用固定 SHA-256 校验。软件包内附项目、Qt、MinGW、Lucide 和 HeadsetControl 的许可和来源说明。产物及命名见 [v0.1.0 发布说明](releases/v0.1.0.md)。

## 验证安装器与便携版

使用无既有中心安装的 Windows 测试账户或 CI runner。安装回归会主动安装、启动并卸载中心，在该账户内留下正常生成的中心设备档案与日志。

```powershell
python tests/windows_package_test.py dist/release-v0.1.0 build/release-qa-v0.1.0
```

回归测试核对 SHA-256、安装后与 ZIP 的逐文件一致性、独立的旧版互斥锁、开始菜单安装后的首次启动、后台 IPC 路径、刷新、关闭面板后后台存活、卸载与开机启动项清理、重装档案保留，以及脱离 Qt SDK 的便携版启动。迁移和档案移除的回归也包含在 Makefile 的测试中。

### v0.1.0 本地结果

2026-11-21 在 Windows 11 x64 使用 Qt 6.8.3、MinGW-w64 13.1.0 和 Inno Setup 6.7.4 构建。Makefile、独立的 `build.bat`、Qt CMake、QML lint 和安装包生成均通过。22 项 Makefile 测试及 Qt 驱动启动测试通过，包括旧数据导入、移除中断恢复和设备重新登记。

实际安装包拒绝覆盖含有旧版程序的目录。按用户安装后，开始菜单快捷方式指向中心面板，安装目录与便携 ZIP 的 1398 个分发文件逐一通过 SHA-256 比对。首次启动、命名管道刷新、关闭面板后后台保留、卸载时清理中心自启动项、重装后档案保留，以及无 Qt SDK 的便携启动均通过。旧版档案与开机启动项保持原样。测试副本已卸载，本地日志与截图保留在 `build/release-qa-v0.1.0-verified`。GitHub 托管 runner 上的执行待仓库启用工作流后验证。

## GitHub 发布步骤

1. 在目标仓库提交经审查的源代码和文档。核对 `git status --short` 和暂存区，排除本机 `build/`、`dist/`、`third_party/`、设计参考和 `wireless-battery.zip`。
2. 在目标仓库启用 Actions，运行 **Windows build and release**。普通分支只生成构建产物。下载并检查 Windows x64 工作流产物。
3. 为已通过构建的 `main` 提交创建并推送 `v0.1.0` 标签。标签构建通过后会生成标记为 Pre-release 的 Release 草稿。确认安装包、便携版、`SHA256SUMS.txt` 与 HeadsetControl 对应源码齐全，再由维护者批准并发布草稿。

后续版本同步更新 `res/version.h`、`res/app.manifest` 的组装版本以及 `docs/releases/vX.Y.Z.md`。标签版本必须匹配 EXE 的产品版本。
