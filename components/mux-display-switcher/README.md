# MUX

MUX 是一个高性能 Windows 10/11 显示拓扑切换器。它按 EDID 名称精确识别 `P25W2GC`，支持一键仅保留目标屏，也支持手动勾选任意显示器组合。

## 使用

运行 `dist\MUX.exe`：

- “仅保留 P25W2GC”在一次 CCD Apply 中完成拓扑切换。
- 勾选列表后点击“应用所选”，可手动控制启用哪些屏幕。
- “恢复上次布局”恢复本次切换前保存的活动拓扑。

MUX 不再生成独立的一键 EXE，所有操作集中在单一主程序中。

## 性能

- 原生 Win32 C++，无框架、无后台轮询、无需管理员权限。
- 首帧先显示，显示器枚举延后到窗口出现之后。
- 正常切换只调用一次 `SetDisplayConfig`；仅在驱动报错时验证和回滚。
- 请求准备、CCD 切换和列表刷新全部在受控工作线程完成，界面消息循环不被阻塞。
- 工作线程由窗口持有并在退出前回收，不使用 detached 线程或跨线程裸指针。
- 列表顺序按稳定设备身份排列；被动刷新不会清除尚未应用的手动选择。
- 已启用屏幕显示当前模式，未启用屏幕明确显示首选模式。
- 使用 legacy CCD 索引，规避部分显卡驱动的 virtual-aware 索引报告错误。
- 静态链接运行时，单文件部署。

显示器黑屏时间主要由 Windows DWM 重建桌面、显卡驱动训练链路和显示器重新握手决定，无法由用户态程序完全消除。

## 构建

本机运行：

```bat
build.cmd
```

输出：

- `dist\MUX.exe`：主程序。
- `dist\MUX-cli.exe`：诊断与自动化工具。

重新生成品牌图标：

```powershell
python tools\build_icon.py
```

图标生成脚本需要 Pillow。提交的 `assets\mux.ico` 已可直接用于正常构建。

## CLI

```text
MUX-cli.exe list
MUX-cli.exe self-test
MUX-cli.exe check-p25w2gc
MUX-cli.exe only-p25w2gc
MUX-cli.exe extend
```

`self-test` 会只读验证当前所有非空显示器组合，不执行切换。
