# 《孤岛悲歌》PC 移植

将原 Symbian 游戏的 ARM/Thumb 代码在构建时翻译成 C，再编译为 Linux 桌面程序。原 EXE 和两份游戏 DLL 负责游戏逻辑、媒体解码和绘制；宿主用 SDL2 等库提供窗口、输入、音频和所需的 Symbian 接口。不依赖模拟器、JIT 或指令解释器。

当前自动验证覆盖原标题、主菜单、开局影片、首关绘制及教程后的方向键移动。首关的正常游玩以[手动验收](docs/first-level.md)为准；未验证通关或后续关卡。首场景仍有贴图缺失警告。

## 在 Linux 上构建和运行

使用 Python 3、Clang 19、`pkg-config`、`unzip`、SDL2、FreeType、Fontconfig、zlib 和中文字体。Debian 13 可安装：

```sh
sudo apt install python3 clang-19 pkg-config unzip libsdl2-dev libfreetype-dev libfontconfig1-dev zlib1g-dev fonts-noto-cjk
```

把自己持有的原版 `Lament-Island.zip` 放在仓库根目录。从根目录运行以下命令；`mktemp` 为本轮构建和游玩创建独立目录，不覆盖旧测试盘：

```sh
mkdir -p build
run=$(mktemp -d build/play.XXXXXX)
python3 tools/extract_e32.py Lament-Island.zip "$run/e32" --resource-root "$run/drive"
unzip -q Lament-Island.zip 'Lament-Island/SIMLIFE/*' -d "$run/original"
python3 tools/recomp_start.py --game-dlls "$run/e32" "$run/e32/IsLand_3rd.exe" "$run/island.c"
clang-19 -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
  "$run/island.c" $(pkg-config --cflags --libs sdl2 freetype2 fontconfig zlib) -lm -o "$run/island"
printf '测试目录：%s\n' "$run"
"$run/island" "$run/drive" "$run/original/Lament-Island/SIMLIFE" 2>"$run/runtime.log"
```

编译生成的 C 可能需要数分钟。运行时第一个参数是可写的安装盘副本，第二个参数是原 `SIMLIFE` 目录。复测可使用同一个构建产物，但若要保留旧存档，请另建安装盘副本。

## 文档与许可

- [首关手动验收与键位](docs/first-level.md)
- [移植架构与诊断](docs/runtime.md)
- 项目源码采用 [GPL-3.0](LICENSE)。原游戏包、素材、提取与翻译产物、存档和日志不在仓库许可或分发范围内。
