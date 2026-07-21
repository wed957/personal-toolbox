# 键盘检查

`键盘检查.exe` 是一个 Windows x64 磁轴键盘参数监控工具。

## 使用说明

1. 在 IYX 中打开键盘校准页。
2. 运行 `键盘检查.exe`。
3. 在终端查看按键当前位置、峰值和行列坐标；按 `Ctrl+C` 退出。

程序只读目标键盘的 `MI_02` HID 接口，不修改设备状态。

## 文件校验

- 文件大小：7,413,369 字节（7.07 MiB）
- SHA-256：`2EF56DF4F0A3D5A53FB790794A55F199165B120A0D22BD2E6ADFA9AD516B3517`
- SHA-1：`64930FF3325605A1C207E72930894CAE41E58250`
- MD5：`DFB29116E41B0DA159BAC14A56CA9784`

## 静态分析摘要

- PE32+（x86-64）Windows 控制台程序。
- 使用 Python 3.13 / PyInstaller 6.x 单文件打包。
- 应用入口模块：`magnet_reborn`；核心模块：`magnet_passive_core`。
- 通过 `hid.enumerate()` 查找指定 vendor ID 与 usage page 的 HID 接口。
- 以只读权限调用 `CreateFileW`，随后通过 `ReadFile` 读取 64 字节 HID 报告。
- 应用模块中未发现联网、下载/上传、注册表、自启动、外部命令执行或文件写入逻辑。
- 文件没有 Authenticode 数字签名和版本/厂商元数据。

以上结论来自静态分析，不代表绝对安全。请仅在信任文件来源时运行。
