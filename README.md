# Personal Toolbox

中文名：自用工具箱。

Windows 自用工具箱，把四个原项目放在同一个可构建、可追溯的仓库中，并收敛为三个运行入口：

- **ICC Switch**：切换显示器 ICC/ICM 色彩配置。
- **MUX**：切换显示拓扑和屏幕布局。
- **IYX Fast Launcher**：启动本地 IYX 驱动界面及内置磁轴键盘检测。

## 使用

运行 `dist\Toolbox.exe`，从统一入口启动三个模块。入口只负责发现并启动组件，组件仍在独立进程中运行；因此原有权限边界和清理逻辑保持不变。`打开工具目录`可直接查看收集后的可执行文件。

统一入口使用 GDI+ 自绘响应式画布和 Lucide 矢量图标，支持鼠标悬停、窗口缩放以及键盘焦点操作。渲染为纯静态、事件驱动：仅在交互状态变化或时钟走秒时重绘（缓存底图 + 局部补丁合成），空闲 CPU 占用接近零，界面全程不使用表情符号。ICC、MUX 和 IYX 子界面共享同一套网格、字体、色彩、状态反馈和按钮交互规范；IYX 的内置网页界面额外注入同一套主题，磁轴检查入口使用 Lucide Keyboard 图标。独立的键盘检查程序已内置于 IYX，不再向 `dist\tools` 重复复制；原仓库快照和校验仍完整保留。

四个入口图标共享同一设计语言：近黑圆角底 + 2×2 色块网格。统一入口填满三个模块色块，各组件图标只点亮自己在启动器中对应位置的色块（ICC 珊瑚红左上、MUX 蓝 右上、IYX 青柠绿左下），由 `src\toolbox_icon.py` 一键生成。

`Toolbox.exe --check` 是无界面完整性检查：三个运行模块全部存在时返回码为 `0`，否则返回 `1`。

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
- 键盘检查二进制 SHA-256 与原仓库 README 及 IYX 内嵌副本一致；
- 发布目录不再包含重复的独立键盘检查程序；
- 统一入口和收集后的组件均已生成；
- ICC CLI 帮助、MUX 只读自检、IYX 补丁校验成功；
- `Toolbox.exe --check` 返回成功。

ICC/MUX 的实际显示器操作仍需在目标 Windows 会话中手动验证；测试不会切换显示布局或修改 ICC 配置。
