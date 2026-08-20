# IYX Fast Launcher

`IYX Fast Launcher` 是面向 Windows x64 的 IYX 单文件启动器。它把运行资源、原生驱动链和本地网页界面封装到一个可执行文件中，减少每次启动时的完整解压开销。

## 主要特性

- 单文件分发，目标机器无需单独安装项目依赖。
- 启动时仅解压原生启动必需文件，界面静态资源直接从内嵌 ZIP 提供。
- 使用系统 Microsoft Edge 作为渲染器，并保持 IYX 原生 WebSocket 驱动通信路径。
- 禁用首次启动时自动弹出的按键校准页面，手动校准入口仍保留。
- 关闭校准界面或程序时主动退出校准并停止全键监听。
- 改键配置保存在本机用户目录，重启程序后仍会正确回显。
- 当前桌面图标采用 Lucide 风格的圆角等宽线条，并包含 16 至 256 像素资源。

## 仓库结构

| 文件 | 用途 |
| --- | --- |
| `FastLauncher.cs` | 当前单文件启动器源码 |
| `Payload.zip` | IYX 运行资源，使用 Git LFS 管理 |
| `sdk.js` | 本地 IYX SDK 桥接脚本 |
| `IYX.ico` | 当前 Windows 多尺寸图标 |
| `GenerateIconV6.cs` | 当前图标的可复现生成器 |
| `build.cmd` | Windows 构建脚本 |

## 构建

要求：

- Windows x64
- Git LFS，用于完整拉取 `Payload.zip`
- .NET Framework 4.x 自带的 x64 C# 编译器

克隆后执行：

```powershell
git lfs pull
.\build.cmd
```

构建产物为仓库根目录下的 `IYX.exe`。

## 运行说明

- 目标机器需要可用的 Microsoft Edge。
- 用户配置保存在 `%LOCALAPPDATA%\IYXFastLauncher`，不会打包进可执行文件。
- 启动速度受磁盘、系统负载、驱动服务启动和硬件枚举速度影响。
- Release 页面提供已经构建好的单文件版本。

## 版本

当前启动器版本：`3.1.0`
