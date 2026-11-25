# 移植架构与诊断

1. `tools/extract_e32.py` 从本地原版 ZIP 内的 SIS 提取 `IsLand_3rd.exe`、`SimMediaEngineDll.dll` 和 `SimDeviceEngineDll.dll`，并按开关把应用资源和初始存档写入本地安装盘。
2. `tools/recomp_start.py` 读取 E32 重定位与导入，从原 `_E32Startup` 等入口遍历 ARM/Thumb 代码，生成包含原游戏模块和宿主 ABI 的 C；Clang 把它编译为桌面机器码。原游戏状态机驱动启动、开局影片、脚本和场景。EKA2L1 只供查阅接口与设计，不参与构建或运行。
3. `tools/*.h` 提供宿主文件、位图、窗口、线程、同步和音频等接口。SDL2 接收键盘事件并输出原游戏绘制的 RGB565 帧和解码出的 PCM。原菜单「访问官网」把原游戏提供的 URL 交给 `SDL_OpenURL`；手测游戏无需选择该项。
4. 画面：客户机屏幕尺寸是运行时值（`tools/window_gc.h` 的 `screen_configure`，默认 16:9×208 行），原游戏按视口尺寸分配缓冲并居中其 176×208 菜单画面。`SetViewportWithOffset`（`0x6eb74`）的投影缩放被固定为原 176×208 的比例，使画面中心不放大、两侧多出视野。`FUN_00038e80`（场景 HUD）由 `tools/hud_layout.h` 用原位图按新布局重绘，其余绘制仍由原代码完成。`guest frame` 的 FNV 哈希只在 `ISLAND_SCREEN=native` 时等于 `039c1b3e`。
5. 色彩：宿主把 RGB565 经 65536 项查表转换成 RGBA8 再用暂存缓冲上传；`ISLAND_COLORSPACE` 选择 sRGB（硬件编码，线性过滤在线性光下进行）或 Display-P3。
6. 音频：宿主为 SDL 队列保持约 150 ms 的预读，减小慢帧造成的欠载；欠载会在日志中输出 `audio: underrun`。

## 诊断

- 私有 Xvfb 回放已从序章石阶进入第一章。取镐后沿楼梯中间上楼，关闭箱子和跳跃教程；用原推拉操作把木箱移到断梁前，先跳上箱子，再跳上石梁，进入走廊。原游戏在「走廊远处好像有什么东西……」之后打开 `VIDEO/176X208/0.SVO`、`li2.bsp.fix` 和 `SCRIPT/2` 的脚本；第一章实际画面显示石道、角色、HP 栏及「屁股好痛！刚才发生了什么事情？」。序章的 `li1.bsp.fix` 和 `SCRIPT/1` 不是第一章通过的证据。
- 空窗口不是首帧。`guest frame` 记录实际绘制与呈现位置及像素哈希。`--first-frame` 仅用于首帧探针。
- 错误优先看首次出现的未支持指令地址、DLL/ordinal 或 `unhandled Symbian leave`，不要把后续症状当作根因。外部 `timeout` 的退出码 `124` 表示测试限时结束，不是宿主报错。
- 首场景可绘制，但当前仍有 `texture\scene\` 贴图缺失警告；`texture\sd2\` 的同名文件不一定是同一素材，不做自动回退。
- 序章的「存储档案」菜单通过 `TTime::DateTime()` 显示本地日期。选择槽位后，原游戏通过 `RFileWriteStream` 写入序列化数据。安装盘副本上已验证槽位 `0` 及 `.idx`、`.itm`、`.dat`、`.pub`、`.id` 的写入；重启后从主菜单读取同槽位，回到序章场景。`tmp.id` 校验不变。该测试不覆盖第一章或其他槽位。
- 中文乱码或方框时，确认 Fontconfig 可用且默认 `sans-serif` 匹配支持简体中文的字体。无声时先确认桌面音频设备可用，再检查 SDL 音频日志。
