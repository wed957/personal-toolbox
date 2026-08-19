# ICC Switch

Windows ICC 色彩配置切换工具。使用 C++17 和 Win32 原生界面编写，不依赖第三方运行时。

![ICC Switch 图标](assets/icc-switch.png)

## 功能

- 查看当前活动显示器和已安装的 ICC/ICM 配置
- 将选定配置应用到指定显示器（当前用户）
- 导入本地 `.icc` / `.icm` 文件
- 一键将全部活动显示器恢复为系统 sRGB 配置
- 提供 GUI 和命令行两种入口
- 内嵌多尺寸前卫图标，适配任务栏、窗口和资源管理器

## 下载

直接下载仓库中的 Windows 程序：

- [ICC-Switch.exe](release/ICC-Switch.exe)：图形界面
- [icc-switch-cli.exe](release/icc-switch-cli.exe)：命令行版本
- [GitHub Releases](../../releases)：版本化发布包

运行环境：Windows 10 1809 或更高版本。程序只处理 Windows 色彩管理中的 ICC/ICM 配置，不包含任何 ICC 文件。

## GUI 使用

1. 启动 `ICC-Switch.exe`。
2. 在“显示器”列表中选择目标显示器。
3. 在“ICC 配置”列表中选择已安装配置。
4. 点击“应用”。
5. 需要导入新配置时，点击“导入 ICC...”，选择文件后再点击“应用”。

“恢复默认”会将全部活动显示器切回系统 `sRGB Color Space Profile.icm`。

更完整的操作说明和故障排查见 [`docs/使用说明.md`](docs/使用说明.md)。

## 命令行用法

在程序所在目录打开 PowerShell：

```powershell
.\icc-switch-cli.exe list
.\icc-switch-cli.exe profiles
.\icc-switch-cli.exe set "sRGB Color Space Profile.icm"
.\icc-switch-cli.exe set "D:\ICC\display.icc" 2
.\icc-switch-cli.exe install "D:\ICC\display.icc"
```

`set` 默认切换第 1 台活动显示器；传入本地文件路径时会先安装配置，再设置为当前用户默认配置。使用 `--help` 查看完整参数。

## 从源码构建

### 依赖

- Windows 10 1809 或更高版本
- MinGW-w64 g++ 和 `windres`
- 可选：CMake 3.16+

默认构建脚本使用 Scoop 安装的 MinGW 路径：`%USERPROFILE%\scoop\apps\mingw\current\bin`。

```powershell
.\build.cmd
.\build\icc-switch-gui.exe
.\build\icc-switch.exe --help
```

构建脚本会先编译 Windows 资源文件，再链接 GUI 程序，因此图标会直接嵌入 EXE。CMake 工程同样包含 `src/app.rc`。

## 项目结构

```text
src/
  main.cpp          命令行入口和 ICC 操作
  gui.cpp           Win32 GUI 入口
  app.rc            Windows 图标资源
  resource.h        资源 ID
  icc-switch.ico    多尺寸应用图标
release/             已验证的 Windows 可执行文件
docs/                中文使用说明
build.cmd            MinGW 一键构建脚本
CMakeLists.txt       CMake 构建配置
```

## 设计说明

图标使用“断裂色盘”概念：青色与洋红色表示不同色彩配置，酸性黄色闪电表示快速切换，深色圆角底保证在浅色和深色桌面上都清晰可辨。ICO 内含 16、20、24、32、40、48、64、96、128、256 像素帧。

## 注意事项

- 这是 Windows 专用工具，不能在 macOS 或 Linux 上运行。
- 应用修改的是当前用户的显示器 ICC 关联；系统策略可能要求提升权限。
- 切换配置前请确认 ICC 文件来自可信来源。
