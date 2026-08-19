# 自用工具箱

Windows 自用工具箱，把四个独立工具放在同一个可构建、可追溯的仓库中：

- **ICC Switch**：切换显示器 ICC/ICM 色彩配置。
- **MUX**：切换显示拓扑和屏幕布局。
- **IYX Fast Launcher**：启动本地 IYX 驱动界面。
- **键盘检查**：只读监控磁轴键盘 HID 数据。

## 使用

运行 `dist\Toolbox.exe`，从统一入口启动四个工具。入口只负责发现并启动组件，组件仍在独立进程中运行；因此原有权限边界和清理逻辑保持不变。`打开工具目录`可直接查看收集后的可执行文件。

`Toolbox.exe --check` 是无界面完整性检查：四个组件全部存在时返回码为 `0`，否则返回 `1`。

## 构建

要求：Windows x64、MinGW-w64、.NET Framework 4.x C# 编译器，以及 Git LFS。首次克隆后先执行：

```powershell
git lfs pull
.\build.cmd
.\tests\run-tests.ps1
```

构建产物位于 `dist\`：

```text
dist\Toolbox.exe                 统一入口
dist\tools\icc-switch-gui.exe   ICC 图形界面
dist\tools\icc-switch-cli.exe   ICC 命令行
dist\tools\MUX.exe              MUX 图形界面
dist\tools\MUX-cli.exe          MUX 诊断命令行
dist\tools\IYX.exe              IYX 启动器
dist\tools\keyboard-check.exe  键盘检查
```

顶层 `build.cmd` 会调用 `components` 下各原项目的构建脚本，再编译统一入口。IYX 的 `Payload.zip` 由 Git LFS 管理，缺少 LFS 内容时构建会按原项目规则失败。

## 分支和来源

四个原仓库的完整提交历史分别保留在以下分支，分支根目录与原仓库一致：

| 分支 | 来源 |
| --- | --- |
| `icc-switch` | `wed957/icc-switch` |
| `mux-display-switcher` | `wed957/mux-display-switcher` |
| `keyboard-check` | `wed957/keyboard-check` |
| `iyx-fast-launcher` | `wed957/iyx-fast-launcher` |

`main` 在 `components/` 中保留四个仓库的源码/资源快照，并新增统一入口和测试。`keyboard-check` 原仓库历史本身只包含 `键盘检查.exe` 与文档，没有可恢复的源代码；该二进制及其 SHA-256 校验值均原样保留。

## 测试

`tests\run-tests.ps1` 检查：

- 四个组件的关键源码、资源和 LFS 指针存在；
- 键盘检查二进制 SHA-256 与原仓库 README 一致；
- 统一入口和收集后的组件均已生成；
- ICC CLI 帮助、MUX 只读自检、IYX 补丁校验成功；
- `Toolbox.exe --check` 返回成功。

ICC/MUX 的实际显示器操作仍需在目标 Windows 会话中手动验证；测试不会切换显示布局或修改 ICC 配置。
