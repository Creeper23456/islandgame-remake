# 移植架构与诊断

1. `tools/extract_e32.py` 从本地原版 ZIP 内的 SIS 提取 `IsLand_3rd.exe`、`SimMediaEngineDll.dll` 和 `SimDeviceEngineDll.dll`，并按开关把应用资源和初始存档写入本地安装盘。
2. `tools/recomp_start.py` 读取 E32 重定位与导入，从原 `_E32Startup` 等入口遍历 ARM/Thumb 代码，生成包含原游戏模块和宿主 ABI 的 C；Clang 把它编译为桌面机器码。原游戏状态机驱动启动、开局影片、脚本和场景。EKA2L1 只供查阅接口与设计，不参与构建或运行。
3. `tools/*.h` 提供宿主文件、位图、窗口、线程、同步和音频等接口。SDL2 接收键盘事件并输出原游戏绘制的 RGB565 帧和解码出的 PCM。原菜单「访问官网」把原游戏提供的 URL 交给 `SDL_OpenURL`；手测游戏无需选择该项。

## 诊断

- 空窗口不是首帧。`guest frame` 记录实际绘制与呈现位置及像素哈希。`--first-frame` 仅用于首帧探针。
- 错误优先看首次出现的未支持指令地址、DLL/ordinal 或 `unhandled Symbian leave`，不要把后续症状当作根因。外部 `timeout` 的退出码 `124` 表示测试限时结束，不是宿主报错。
- 首场景可绘制，但当前仍有 `texture\scene\` 贴图缺失警告；`texture\sd2\` 的同名文件不一定是同一素材，不做自动回退。
- 中文乱码或方框时，确认 Fontconfig 可用且默认 `sans-serif` 匹配支持简体中文的字体。无声时先确认桌面音频设备可用，再检查 SDL 音频日志。
