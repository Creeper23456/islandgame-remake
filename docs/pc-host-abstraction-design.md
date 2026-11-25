# PC 版宿主抽象设计

本文是 [PC 版现代化需求](pc-modernization.md) 的子系统设计说明，只覆盖**原游戏逻辑与 Symbian 底层之间**的跨平台宿主抽象：渲染呈现、窗口/输入、音频、媒体、文件/存档、计时/线程、设备能力，以及 Symbian ABI 兼容层与现代宿主接口的边界。

本文不设计渲染管线内部算法（见 [PC 渲染设计说明](pc-rendering-design.md)），不设计输入上下文与背包语义（见 [PC 输入与背包设计](pc-input-inventory-design.md)），不重列验收矩阵（见 [PC 版现代化验收矩阵](pc-modernization-verification.md)）。本文不改源码、不实现功能、不运行构建或测试。

证据标记沿用同批文档：

- **【已确定】**：`docs/pc-modernization.md` 的需求，或原二进制/当前宿主中已定位、可在 `build/*.tsv`、`tools/*` 复核的事实。
- **【推荐】**：本文给出的接口或迁移方案，尚未实现。
- **【未验证】**：证据不足，落地前必须先补证据；不得按推断实现。

核心命题：**原游戏逻辑保留在 guest 代码里，Symbian 平台服务由兼容层重做；兼容层只通过本文的 `hb_*` 端口访问操作系统。宿主端口里不出现 Symbian 类型、不出现 guest 地址、不出现游戏规则。**

> 待整合：`docs/pc-modernization.md` 尚未链接本文；合并时在「总原则」或子系统设计处补链接。本文不修改主文档、README 或源码。

---

## 1. 证据基础

| 事实 | 证据 |
| --- | --- |
| 原 ARM/Thumb 代码在构建期翻译为桌面 C，再由 Clang 编译；不使用模拟器/JIT/解释器 | `docs/runtime.md` §1–2；`AGENTS.md`「运行边界」 |
| 当前 Symbian ABI 由一组内联宿主头实现，而不是独立进程或系统服务 | `tools/window_gc.h`、`tools/fbs_bitmap.h`、`tools/game_files.h`、`tools/guest_threads.h`、`tools/guest_sync.h`、`tools/guest_audio.h`，由 `tools/recomp_start.py` 内联进生成的翻译单元 |
| 位图像素放在 guest 堆，guest 代码与宿主看到同一地址 | `tools/fbs_bitmap.h` 头注释；`fbs_bitmap_create` 用 `heap_try_alloc` 分配像素 |
| guest 线程是协作式，不是 1:1 OS 线程；阻塞导入以 `THREAD_YIELD` 让出 | `tools/guest_threads.h`；`GUEST_THREAD_COUNT 16`；`threads_run_ready` |
| 音频输出按 16000 Hz、单声道、PCM16 打开；同一时刻只有一个未完成缓冲区；回调在 guest worker 上执行 | `tools/guest_audio.h`（`SDL_AudioSpec`、`audio_write`、`audio_callback` 注释） |
| 媒体服务器负责打开/播放/停止视频并维护 guest 读取线程；AVI 读取、mp4 解码、YUV420→RGB24 在 guest 模块内 | `build/SymbolWaveAudio.tsv` 0x3003dc/0x300434/0x30053c/0x300edc；`build/SymbolWaveVideo.tsv` 0x319e40/0x31a23c/0x31ade8 |
| 影片按微秒时间戳配速，落后时跳帧，不用宿主帧率驱动 | `build/SymbolWaveAudio.tsv` 0x3021c4/0x302210 |
| 软件渲染器有颜色/深度/alpha 三套缓冲，最终 `CompositeLightingBuffer` 按 alpha 与深度混合写入 RGB565 | `build/GameFull.tsv` 0x6f720/0x62f08/0x63194；`RendererSymbols.tsv` 0x63194 |
| 当前呈现是固定 176×208 RGB565 帧，整纹理拉到 3 倍窗口；呈现时记录 FNV-1a 像素哈希 | `tools/window_gc.h` `publish_window`；`window_width/height` 176/208；`guest frame: ... FNV-1a=` |
| 文件访问按 Symbian 盘符路径映射到安装盘/资源根；只有 `E:\system\apps\island\loadsave\` 可写 | `tools/game_files.h` `open_game_path` |
| 存档使用 `%d.dat`、`%d.id`、`%d.itm`、`%d.pub`、`tmp.idx`、`tmp` 等原文件；`tmp.id` 参与校验 | `build/SymbolWaveScene.tsv` 0x1f628/0x27130/0x1b4dc；`build/GameFull.tsv` 0x37690；`docs/runtime.md` |
| 游戏心跳使用 `CPeriodic` 500000 us；多处逻辑直接读 `TTime::HomeTime` 的微秒值（帧时间戳、会话时间、随机种子、视频配速） | `build/SymbolWaveGeometry.tsv` 0x59d00/0x63278/0x8c0d8；`build/SymbolWaveScene.tsv` 0x1fe44 |
| 电话/短信是显式空实现；相机/麦克风没有宿主实现 | `tools/recomp_start.py` 中 `RTelServer`「empty inventory」、`CTelephony`「no cellular modem」、`CMsvSession`「empty message store」；`tools/*.h` 无相机绑定；`build/SymbolWaveDevice.tsv` 的相机代码属于 guest 模块 |
| 输入由 SDL 事件转成 Symbian 扫描码与 `EEventKeyDown/Up`，每个调度轮只投递一个事件 | `tools/recomp_start.py` `pump_window_input` |

从这些事实只能得出：**当前宿主是一个「内联 shim + SDL2」的单体实现**。它证明 ABI 兼容层可以工作，但不证明其接口、线程模型和路径规则足以支撑第二、第三个平台。本文的接口是为此抽取的**推荐边界**，不是现有代码的既成 API。

---

## 2. 分层与职责

```text
┌──────────────────────────────────────────────────────────────────┐
│ L3 原游戏 guest 代码（ARM/Thumb 翻译后）                          │
│    IsLand_3rd.exe + SimMediaEngineDll + SimDeviceEngineDll        │
│    状态机、脚本、场景、镜头、碰撞、交互、存档字段、软件光栅器     │
├──────────────────────────────────────────────────────────────────┤
│ L2 Symbian ABI 兼容层                                             │
│    CBase/vtable、descriptor、CleanupStack、CActiveScheduler       │
│    RFs/RFile、RThread/RSemaphore/RMutex、RTimer/CPeriodic         │
│    CFbsBitmap/CFbsBitGc/CWindowGc、CCoeControl/AppUi              │
│    媒体/设备模块的宿主服务面（CMdaAudio…、CMediaServer…）         │
│    CTelephony/RTelServer/CMsvSession 的显式缺省实现               │
├──────────────────────────────────────────────────────────────────┤
│ L1 宿主端口 hb_*（本文定义的边界）                                │
│    storage / clock / wait / audio / window / input / render       │
│    present / device caps / shell                                  │
├──────────────────────────────────────────────────────────────────┤
│ L0 平台后端                                                       │
│    Linux：SDL2 + POSIX + ALSA/PipeWire + Vulkan                   │
│    Windows：SDL2/Win32 + WASAPI + Vulkan                          │
│    Steam Deck：SteamOS + Gamescope + Vulkan                       │
└──────────────────────────────────────────────────────────────────┘
```

| 层 | 拥有 | 不拥有 |
| --- | --- | --- |
| L3 guest | 游戏规则、脚本解释、场景状态、存档字段、软件渲染 | OS 句柄、窗口、音频设备、路径解析 |
| L2 ABI | Symbian 对象模型、描述符、活动调度、R 类句柄、位图内存、2D 绘制命令、错误/Leave 映射 | 平台 API、GPU、音频设备、文件系统细节 |
| L1 宿主端口 | 字节流、时钟、像素输出、输入事件、PCM、资产文件、能力查询 | 任何游戏判定、任何 Symbian 句柄 |
| L0 后端 | SDL/OS/GPU/音频/文件系统实现 | guest 头、L2 头、游戏数据结构 |

依赖方向只允许 L3→L2→L1→L0。L0 不得 `#include` L2/L3 头；L1 不得包含 `tools/*.h` 或生成代码中的 guest 类型。

**为什么现在抽取**：当前 shim 把 SDL、路径、音频、线程状态都写成翻译单元内的全局单例；平台代码与 ABI 语义混在同一文件，无法独立测试，也无法在不改 ABI 的情况下替换渲染器或音频后端。抽取不是重写游戏，而是把「Symbian 语义」与「OS 服务」拆开。

---

## 3. 硬性不变量

| ID | 不变量 | 违反示例 |
| --- | --- | --- |
| **I1** | guest 地址是 32 位值；宿主指针不得写入 guest 内存。guest 地址只允许在明确的调用/帧租约内被宿主临时访问，租约外不得解引用、不得跨帧保存 | 把 `CFbsBitmap*` 编成整数存进宿主贴图对象，下一帧继续用 |
| **I2** | 只有宿主端口 ID（`hb_ref`）跨边界；Symbian 句柄（RFile、RThread、TRequestStatus* 等）只在 L2 内解释 | 把 `RFile` 的 handle word 直接传给宿主当文件描述符 |
| **I3** | 同一时刻只有一个 OS 线程执行 guest 代码；宿主完成事件只能入队，不能直接回调 guest | 在 SDL 音频回调里调用 `audio_callback` 进入 guest |
| **I4** | 存档文件名、文件集合、字段字节、写入顺序语义不变；宿主不新增、不改写、不迁移存档 | 宿主写 `slot0.json` 侧车、自动压缩、自动云同步 |
| **I5** | 宿主失败必须映射为明确的 Symbian 错误；不得静默成功或伪造数据 | 相机缺失时返回一张黑帧；音频设备缺失时假装已打开 |
| **I6** | 设备能力在启动后不可变，并如实报告；当前 PC 目标没有相机/麦克风/电话/短信/网络 | 为「以后可能用」暴露假能力 |
| **I7** | L1/L0 不实现任何游戏规则：不判断物品、不选择分支、不推进场景状态 | 宿主根据 `engine+0x530` 决定合成是否成功 |
| **I8** | 资产只读；可写范围仅限安装盘的 `loadsave` 子树 | 把解包资源目录挂成可写，或回退到同名 `texture\sd2\` 文件 |

这些不变量是评审门槛，不是建议。任何一条被破坏，即使画面和流程看起来正常，也不接受。

---

## 4. 公共类型与调用模型

```c
/* hb_types.h —— 只含固定宽度整数和本文类型，不含任何 guest/Symbian 头。 */
typedef int32_t hb_status;                  /* 0 = KErrNone；负值 = Symbian KErr */

typedef struct { uint32_t index, generation; } hb_ref;  /* {0,0} = 无效 */

typedef struct {
    uint32_t width, height;                 /* 逻辑像素 */
    uint32_t stride_bytes;
    uint32_t format;                        /* HB_FORMAT_RGB565 / RGBA8 / BGRA8 */
    const void *pixels;                     /* 仅调用期间有效；宿主不得保存 */
} hb_surface;

typedef struct {
    uint32_t size;                          /* sizeof(hb_event)，用于版本兼容 */
    uint32_t kind, seq;                     /* seq 单调递增，同一事件只消费一次 */
    hb_ref   ref;                           /* 关联宿主对象；纯输入事件为 {0,0} */
    hb_status status;                       /* 完成事件的 KErr；输入事件为 0 */
    uint64_t timestamp_us;                  /* 单调时钟 */
    uint32_t a, b, c, d;                    /* 按 kind 解释；不得放指针 */
} hb_event;                                 /* 完成事件用 kind=HB_EV_COMPLETION，与输入同队列 */
```

调用模型：

1. **同步调用**：L2 在 guest 调度线程上调用 `hb_*`。文件读写、窗口呈现、PCM 提交等在同一线程完成；返回值即结果。
2. **异步完成**：音频/媒体/设备等宿主线程只把 `kind=HB_EV_COMPLETION` 的 `hb_event` 放入线程安全队列；L2 在调度轮中通过 `hb_wait` 取出并映射成 `TRequestStatus`。宿主永远不直接进入 guest。
3. **唤醒**：任意宿主线程可调用 `hb_wake()` 唤醒正在 `hb_wait` 的 guest 调度线程。
4. **生命周期**：`hb_ref` 由 L2 创建并拥有。关闭时 L2 先取消宿主操作、排空/丢弃在途完成，再递增 generation；携带旧 generation 的完成必须被丢弃。
5. **错误**：只使用 Symbian `KErr*` 语义（如 `-1 KErrNotFound`、`-5 KErrNotSupported`、`-8 KErrBadHandle`、`-10 KErrUnderflow`、`-18 KErrNotReady`）。不得用 `errno` 或平台错误码穿透到 L2。
6. **版本**：`hb_platform_abi_version` 与 `hb_platform_caps` 在初始化后提供版本/能力位；结构体首字段统一为 `size`。新增能力只追加字段或能力位；破坏性变更提升主版本，L2 在启动时拒绝不兼容后端。未知能力必须可降级，不得假设后端一定支持。

---

## 5. 子系统接口

以下签名为**接口形状**，用于固定边界；具体实现可调整参数顺序，但不得引入 guest/Symbian 类型、不得改变所有权与线程规则。

### 5.1 平台与启动

```c
hb_status hb_platform_init(const hb_config *cfg);
void      hb_platform_shutdown(void);
uint32_t  hb_platform_abi_version(void);
const hb_caps *hb_platform_caps(void);   /* init 后不可变 */

typedef struct {
    uint32_t size;
    uint32_t features;                    /* HB_FEAT_DIR_LIST=1<<0，HB_FEAT_HOST_DECODE=1<<1 */
    uint32_t has_camera, has_microphone, has_telephony, has_messaging, has_network;
    uint32_t screen_width, screen_height, screen_format;  /* 逻辑屏幕，当前 176×208 / RGB565 */
    uint32_t pointer, gamepad, touch;
} hb_caps;

typedef struct {
    const char *install_root;      /* 只读安装盘根；其下有 system/apps/island/… */
    const char *asset_root;        /* 只读资源根；对应原 SIMLIFE\ISLAND（分发包可能已展平） */
    const char *save_root;         /* 可写存档根；即安装盘 system/apps/island/loadsave */
    const char *host_config_root;  /* 宿主配置；不参与游戏存档 */
    uint32_t window_mode;
    uint32_t render_backend;
    uint32_t audio_backend;
    uint32_t log_level;
} hb_config;
```

- 根路径由启动器传入，**不得依赖当前工作目录**；缺失或不可读时启动失败并报明确错误。
- `save_root` 必须与安装盘映射后的 `...\loadsave\` 指向同一目录；L2 负责把 Symbian 路径解析为 root + 组件，L1 不解析盘符。
- 平台后端在启动时固定；渲染 API 和音频设备允许在 `hb_caps` 声明的范围内运行时选择。

### 5.2 文件与存档

```c
typedef enum { HB_ROOT_ASSET, HB_ROOT_INSTALL, HB_ROOT_SAVE, HB_ROOT_HOST_CONFIG } hb_root;
typedef enum { HB_OPEN_READ, HB_OPEN_UPDATE } hb_open_mode;

hb_status hb_storage_open(hb_ref *out, hb_root root,
                          const char *const *parts, uint32_t count, hb_open_mode mode);
hb_status hb_storage_read(hb_ref f, void *dst, uint32_t bytes, uint32_t *read);
hb_status hb_storage_write(hb_ref f, const void *src, uint32_t bytes);
hb_status hb_storage_seek(hb_ref f, int64_t offset, uint32_t whence);
hb_status hb_storage_size(hb_ref f, uint64_t *size);
hb_status hb_storage_set_size(hb_ref f, uint64_t size);
hb_status hb_storage_flush(hb_ref f);
void      hb_storage_close(hb_ref f);

/* 可选能力：HB_FEAT_DIR_LIST。原游戏用 CDirScan 找注册文件；当前流程是否需要未验证。 */
hb_status hb_storage_open_dir(hb_ref *out, hb_root root,
                              const char *const *parts, uint32_t count);
hb_status hb_storage_next(hb_ref dir, char *name_utf8, uint32_t cap,
                          uint32_t *name_len, uint32_t *is_dir);
void      hb_storage_close_dir(hb_ref dir);
```

**路径规则【已确定 + 推荐】**：

| 规则 | 说明 |
| --- | --- |
| 组件化传参 | `parts[]` 是 UTF-8 单级名；禁止 `/`、`\`、`:`、空串、`.`、`..`；L2 已按 Symbian 路径拆好 |
| 大小写不敏感匹配 | 每个组件按序比较，匹配现有项；这对 Linux 的 ext4 必须由 provider 模拟 |
| 只允许已存在文件写入 | `HB_OPEN_UPDATE` 仅对 `HB_ROOT_SAVE` 开放；当前 shim 只打开已存在文件。若原流程需要创建新槽位文件，必须另行验证，并限制在保存根、单级文件名、受控后缀内 |
| 不跟随符号链接/重解析点 | Linux 用 `O_NOFOLLOW` + 目录 fd；Windows 检查 reparse point；拒绝穿越 |
| 不创建额外文件 | 宿主不得写索引、锁文件、缩略图、日志到存档目录 |
| 64 位偏移 | 文件大小/偏移用 `int64/uint64`，不得退回 32 位 |

**存档语义【已确定】**：

- 文件集合与命名保持原样：`%d.dat`、`%d.id`、`%d.itm`、`%d.pub`、`tmp.idx`、`tmp`/`tmp.id`；具体由原代码写入。
- `RFile::Open` 不截断；`SetSize(0)` 才截断；写入从当前位置继续。宿主不得在 open 时自动清空。
- 写入的字节必须与 guest 提交的字节完全一致；不做换行转换、不做编码转换、不加头部、不压缩、不加密。
- `tmp.id` 是校验文件，宿主不得更新、修补或纳入任何「修复」流程。
- 宿主在首次运行时**不**自动生成或初始化存档；安装期复制初始存档属于部署步骤，必须逐字节保留，不得改写或补文件。
- 槽位内容只由原存档状态触发。
- 不增加云存档、自动备份、版本迁移。若以后做外部备份，必须在游戏不可见的位置、显式 opt-in，且不得重命名/替换原文件。
- 【未验证】崩溃时部分槽位文件可能处于「写了一半」的状态；原文行为与可接受恢复策略未验证。第一阶段**不引入**原子重命名或日志，因为那会改变失败可见性；先记录现状，再单独决策。

**资产规则**：资源根只读；`texture\scene\` 缺失不得回退到 `texture\sd2\` 同名文件（`docs/runtime.md` 明确禁止）。

### 5.3 计时与调度

```c
typedef struct {
    int32_t year, month, day, hour, minute, second, microsecond;
    int32_t utc_offset_seconds;              /* 已应用 DST 的本地偏移 */
} hb_local_time;

uint64_t hb_clock_monotonic_us(void);       /* 只增不减；用于帧时间戳、配速、超时 */
uint64_t hb_clock_wall_utc_us(void);        /* 墙钟；用于日期显示、存档时间字段 */
hb_status hb_clock_local(hb_local_time *out); /* 本地日期时间；供 TTime::DateTime */
hb_status hb_wait(uint64_t timeout_us, hb_event *out); /* 0=超时，1=事件，<0=错误 */
void      hb_wake(void);                     /* 任意宿主线程可调用 */
```

- **【已确定】** 心跳是 `CPeriodic` 500000 us；帧时间戳、会话时间、视频配速、随机种子都读 `TTime::HomeTime` 微秒值。
- **【已确定】** 引擎节奏属于现代化范围：宿主可以以高于原逻辑 tick 的频率运行渲染和输入采样。逻辑 tick 的频率与渲染插值方案由引擎节奏调查文档（`pc-engine-timing.md`，调查后产出）定案；定案前宿主不改变原逻辑每帧推进的次数，也不用显示刷新率直接驱动游戏状态。
- 定时器截止时间由 L2 计算，L1 只提供时钟和带超时的等待；不得把 `RTimer` 映射成宿主定时器对象。
- `hb_wait` 是 guest 调度线程唯一允许的**主动等待原语**；文件 I/O 仍是同步调用，后端不得让它变成无限期阻塞（网络/设备路径必须显式失败，不得挂起调度线程）。`User::After` 必须让出调度，不得在 guest 线程上长睡眠。
- 墙钟变化（NTP、手动改时、时区/DST）立即反映到 `hb_clock_wall_utc/local`；不得缓存启动时的时区。
- **【推荐】** Linux 使用包含挂起的时钟（如 `CLOCK_BOOTTIME`）；Windows 使用不会因休眠倒退的计数源。恢复时每个定时器最多补一次到期，随后按原间隔重排，不重放整段欠账。
- 【未验证】原 Symbian 在挂起/恢复后的定时器语义；`RandomizeFromTime`/`RandomHundred` 的时间种子是否影响可观察玩法（目前只见于渲染/随机效果路径）。

#### 线程与同步

| 项 | 规则 |
| --- | --- |
| `RThread` | 留在 L2 的协作式线程表（当前上限 16）；不得 1:1 映射为 `pthread`/Win32 线程 |
| `RSemaphore`/`RMutex` | 留在 L2；`RMutex` 按二值信号量处理；阻塞等待以 `THREAD_YIELD` 让出 |
| guest 执行线程 | 全进程只有一个 OS 线程执行 guest 代码；调度轮之间由 `hb_wait` 等待事件或超时 |
| 宿主服务线程 | 仅音频回调、可选解码等 L0 内部线程；不得调用 guest，只能向完成队列投递 |
| 优先级 | 不建模；宿主不得依赖或修改线程优先级来改变游戏时序 |
| 关闭顺序 | 后端线程必须在 `hb_platform_shutdown` 前停止并 join；不得留下回调进入已销毁的 ABI 对象 |

### 5.4 音频

```c
typedef struct {
    uint32_t sample_rate;      /* guest 请求 16000 */
    uint32_t channels;         /* 1 */
    uint32_t format;           /* HB_AUDIO_S16LE */
    uint32_t period_frames;    /* 建议，后端可调 */
} hb_audio_format;

hb_status hb_audio_open(hb_ref *out, const hb_audio_format *fmt);
hb_status hb_audio_write(hb_ref a, const void *pcm, uint32_t bytes, uint32_t *accepted);
hb_status hb_audio_buffered(hb_ref a, uint32_t *bytes);
hb_status hb_audio_volume(hb_ref a, int32_t volume_0_128);
hb_status hb_audio_pause(hb_ref a, uint32_t paused);
void      hb_audio_close(hb_ref a);
```

- **【已确定】** guest 以 16 kHz 单声道 PCM16 打开；写入块最大约 800 个采样（`guest_audio.h`）；同一时刻只有一个未完成缓冲区；完成/欠载通过 guest 回调槽投递（`KErrUnderflow -10`）。
- **【推荐】** L1 保持「提交即复制」或「提交后立刻拥有」的明确所有权；返回后 guest 可复用缓冲区。后端的重采样、声道转换、设备缓冲区全部在 L1 内部完成，不改变 guest 看到的采样率和时长。
- 完成事件：队列排空到不超过一个块时投递 drained；队列为空且仍在播放时投递 underflow；设备丢失投递 `KErrNotReady`/`KErrGeneral` 并允许重新打开。不得在音频回调线程调用 guest。
- 音量域保持 0..128（与当前 `SDL_MIX_MAXVOLUME` 语义一致），由宿主缩放样本，不修改 guest 混音结果的上限。
- 暂停背包时音频是否停止由原状态机决定（见输入设计 §4）；宿主不得自行 stop/start。

### 5.5 窗口与输入

```c
hb_status hb_window_create(hb_ref *out, const hb_window_desc *d);
void      hb_window_present(hb_ref w, const hb_surface *s);
hb_status hb_window_resize(hb_ref w, uint32_t pixel_w, uint32_t pixel_h);
hb_status hb_window_set_mode(hb_ref w, uint32_t mode);
void      hb_window_set_title(hb_ref w, const char *title_utf8);
void      hb_window_close(hb_ref w);

typedef struct {
    uint32_t size;
    uint32_t logical_width, logical_height;   /* 玩法基准：176×208 */
    uint32_t initial_width, initial_height;   /* 0 = 逻辑尺寸的整数倍 */
    uint32_t mode;                            /* windowed / borderless / fullscreen */
    uint32_t scale_policy;                    /* nearest / integer / sharp_bilinear */
    uint32_t vsync;
    const char *title_utf8;                   /* 调用期有效，宿主复制 */
} hb_window_desc;
```

| 事件 `kind` | 字段 | 语义 |
| --- | --- | --- |
| `HB_EV_KEY` | a=物理扫描码，b=按下/抬起，c=修饰键，d=重复 | 布局无关的物理键；映射成 Symbian 键码是 L2/输入层职责 |
| `HB_EV_POINTER` | a,b=**逻辑画面**坐标，c=按键位，d=滚轮 | 由后端按呈现视口做逆变换；不在窗口像素坐标里传 |
| `HB_EV_FOCUS` | a=获得/失去 | 失焦时 L2 合成所有按住键的抬起，防止卡键 |
| `HB_EV_RESIZE` | a,b=可绘制像素尺寸 | 只改变输出尺寸，不改变逻辑画面 |
| `HB_EV_QUIT` | — | 请求退出；由 L2/启动器决定顺序 |
| `HB_EV_SUSPEND/RESUME` | — | 平台休眠；触发时钟/音频/文件句柄检查 |

- 窗口只暴露 `hb_ref`，**不得把 Win32 `HWND`、X11 `Window`、Wayland surface 或 SDL 指针交给 L2**。
- `present` 的 `hb_surface` 是瞬时视图；窗口后端可复制或上传纹理，但调用返回后不得继续引用 guest 像素。`present` 不得无限阻塞 guest 调度；vsync 由后端处理，若后端可能等待刷新，必须提供非阻塞或限时路径，且游戏时间仍只来自 `hb_clock`。
- 逻辑画面与输出像素分离：宽屏、DPI、全屏、整数缩放都由窗口/呈现层处理；逻辑 176×208 仍是画面、HUD 和点击坐标的基准，碰撞/交互继续使用原世界坐标，不因窗口尺寸变化。画幅策略由渲染/验收文档定义，本文只要求接口能表达「逻辑尺寸 ≠ 输出尺寸」。
- **【推荐】** 输入映射表属于 L2 的输入适配模块（仍是非 guest 代码），只此一处把物理键映射为原 Symbian 键码与 Q/E/F/Tab 动作；L0 只报告物理输入，L1 不出现游戏动作名。原 `key` 配置文件属于游戏，宿主不得代写（见输入设计 §8–9）。
- 手柄/Steam Deck 控制器产生与键盘相同的逻辑动作；模拟量不做新玩法。指针语义（点击=确认、拖拽=合成提交）见输入设计 §3.3，宿主只保证坐标一致与事件有序。

### 5.6 渲染与呈现

3D 管线、接入点 A/B/C、材质/深度/雾等由 [PC 渲染设计说明](pc-rendering-design.md) 定义；本文只固定它与窗口/ABI 的所有权接缝。

```c
hb_status hb_render_create(hb_ref *out, const hb_render_desc *d);
void      hb_render_close(hb_ref r);
```

`hb_render_desc` 与 3D 提交调用由渲染设计定义；本文只要求 `hb_render_*` 不保留 guest 指针、关闭后不得再提交，且最终呈现统一走 5.5 的 `hb_window_present`。

- **【已确定】** 当前最终画面是 guest 侧 RGB565 帧：软件渲染器写颜色/深度/alpha，`CompositeLightingBuffer` 混合，GC/文字/影片再画进同一表面，最后由 `CWindowGc` 槽 0x114 触发整帧呈现。
- **兼容路径**：L2 把最终帧作为 `hb_surface` 交给 `hb_window_present`；宿主只做缩放、色彩空间和呈现，不做遮挡/透明/雾的二次判断。这是第一阶段必须通过的路径。
- **现代路径**：渲染后端按渲染设计的接入点读取 guest 已提交的只读帧数据（相机、对象、0x58 三角形批次、材质），并在同一帧内写回 guest 可见帧缓冲；随后 guest 继续执行 HUD/对白/调色，最终仍以同一 `hb_surface` 呈现。host 不重排、不增删提交，不重复实现 2D 层。
- **帧租约**：上述 guest 地址只在 begin/end 帧内有效。租约外不得解引用，不得跨帧保存；需要跨帧的资源（纹理、缓冲）必须复制成宿主资源并由 L2 显式管理生命周期。
- 格式：`HB_FORMAT_RGB565` 为基线；中间层可用 `RGBA8/BGRA8`。原点在左上，行距显式，禁止隐式翻转。RGB565→输出格式的转换必须可重复，不得依赖后端默认。
- 文本/字体：当前 GC 文本由宿主 FreeType/Fontconfig 栅格化（`window_gc.h`），字体解析与栅格化属于 L1 的宿主文本服务；字形选择、排版与布局仍由 L2 决定。渲染设计可选择在输出分辨率重栅格，但必须保持原布局与度量。字体缺失/中文缺字是已知风险（`docs/runtime.md`）。
- **不做**：宿主不得读回最终帧做「画面识别」，不得根据像素改变游戏状态，不得为了宽屏伪造视野或移动镜头。

### 5.7 媒体

媒体资产通过 5.2 的文件接口读取；视频解码结果由 guest 写进最终帧。宿主不单独接收视频层；若后续启用宿主 YUV 转换，也只能写回同一帧缓冲，不得另起覆盖层。

```c
/* 可选、默认关闭：宿主解码后端。仅用于后续性能优化，必须逐帧与 guest 解码对比。 */
hb_status hb_decode_open(hb_ref *out, hb_ref asset, uint32_t codec_hint);
hb_status hb_decode_next(hb_ref d, hb_surface *frame, int64_t *pts_us);
void      hb_decode_close(hb_ref d);
```

- **【已确定】** 媒体服务器 `open/play/stop` 生命周期、AVI chunk/index 解析、mp4 解码、YUV420→RGB24、按微秒预算跳帧都在 guest 模块；配速由 `TTime` + `User::After` 决定，不由宿主播放器决定。
- 影片按素材比例显示，不拉伸、不裁切，必要时留黑边（`pc-modernization.md`）；宿主不得用平台播放器窗口或系统覆盖层替换游戏内呈现。
- 音频轨走 5.4 的同一 PCM 通道；音画同步以原时间戳为准。宿主解码后端必须接受 guest 的帧请求/时间戳，不得自行起播放时钟。
- **【推荐】** 第一阶段不启用 `hb_decode_*`；先证明 guest 解码在宽屏/高分辨率呈现下可用，再评估宿主解码。若启用，`hb_decode_next` 返回的帧内存归后端所有，仅在下次调用或 `close` 前有效；L2 必须复制后才能跨帧使用。

### 5.8 设备能力与外壳

能力字段与屏幕/输入能力见 5.1 的 `hb_caps`；设备能力在 `hb_platform_init` 后不可变。外壳只保留：

```c
hb_status hb_shell_open_url(const char *utf8_url);
```

- **【已确定】** 当前 PC 目标是：电话簿为空、无蜂窝模块、消息库为空；相机/麦克风没有宿主绑定。`CTelephony` 的 IMEI 获取因此会失败，`CGetIMEI` 走错误路径。
- **【推荐】** 能力位如实为 0；原流程若触达相机/麦克风/电话/短信接口，返回 `KErrNotSupported`/`KErrNotFound` 并记录首次地址或 DLL/ordinal。**不得**生成黑帧、静音假流、伪造 IMEI 或短信。相机/麦克风玩法替代不在本阶段（`pc-modernization.md`），也不得以「设备抽象」为名偷偷加入。
- 屏幕能力报告逻辑尺寸与显示格式；实际窗口像素尺寸不属于设备能力。
- `hb_shell_open_url` 只接受原游戏提供的 `http/https` URL，记录并交给系统浏览器；禁止 `file:`、`shell:`、命令行拼接，禁止宿主自行构造 URL。
- 【未验证】正常游玩流程是否可达相机/麦克风/电话/SMS；`SimDeviceEngineDll` 的导入面是否已全部绑定。未确认前，只做确定性失败，不补功能。

---

## 6. Symbian ABI 兼容层与宿主接口的边界

| guest/Symbian 实体 | 归属（服务/句柄由谁拥有） | 宿主看到的形式 |
| --- | --- | --- |
| `CBase`、vtable、`NewL/ConstructL`、Leave/Trap、CleanupStack | L2 | 不可见 |
| `TDesC8/TDesC16/TBuf/HBufC` 及所有权 | L2 | 调用期的字节/UTF-8 + 长度；不暴露描述符头 |
| `RFs`/`RFile`/`RFileReadStream`/`RFileWriteStream` | L2 | `hb_ref` + root + 组件数组 |
| `RThread`/`RSemaphore`/`RMutex` | L2 | 不可见；宿主只看到单个 guest 调度线程 |
| `RTimer`/`CPeriodic`/`TRequestStatus`/`CActive` | L2 | 单调时钟 + `hb_event`（完成事件同队列） |
| `CFbsBitmap` 及像素内存 | L2 + guest 堆 | `hb_surface` 瞬时视图（只读或可写，按调用约定） |
| 可见帧缓冲、0x58 三角形批次、相机/对象数据 | guest 写入，L2 授权租约 | 仅帧租约内的宿主只读/回写视图；见渲染设计，租约外失效 |
| `CFbsBitGc`/`CWindowGc`/`CCoeControl`/`AppUi` | L2 | `hb_window`（最终帧；2D 命令留在 L2） |
| `CMdaAudioOutputStream`（类在媒体模块） | L3 类 + L2 服务面 | `hb_audio` |
| `CMediaServerSim2006`（类在媒体模块） | L3 类 + L2 服务面 | `hb_storage` + 最终帧（可选 `hb_decode_*`） |
| `CTelephony`/`CCamera`/`CAudioInputEngine` | L3 类 + L2 缺省实现 | `hb_caps`（能力位为 0）；无宿主对象 |
| 原 `key` 键位文件 | guest | 普通文件；宿主不解析、不代写 |

注：表中「归属」指服务/句柄由哪层拥有；媒体与设备 DLL 的类实现仍作为 guest 代码运行，L2 只提供它们导入的 Symbian 服务。

兼容层不是 EKA2 模拟器：不实现内核对象、进程抢占、服务器会话 IPC 或 Symbian 全量 API。它只提供 guest 实际导入且被触达的入口；未绑定的导入保持显式失败并报告地址/DLL/ordinal（与 `docs/runtime.md` 的诊断口径一致）。R 类句柄是进程内表项，活动调度是协作式，二者都不对应真实内核对象。

**边界规则（可检查）**：

1. L1 的 `hb_*.h` 只允许包含 `<stdint.h>`、`<stddef.h>`、`<stdbool.h>` 和 `hb_types.h`；出现 `CFbsBitmap`、`TDesC`、`RFile`、`RThread`、`TRequestStatus` 即失败。
2. `hb_*` 结构体不得有字段被注释为「guest 地址」；不得把 `uint32_t` guest 地址当 ID。`hb_ref.index` 只在 L2 的私有表中解释。
3. 宿主对象表必须带 generation；`close` 后在途完成一律丢弃。现有文件 shim 用 `streams[]` 存 `FILE*`、guest 只看到索引，方向正确；但该索引是 L2 的私有映射，抽取后不得把它直接当成宿主 ID，也不得把宿主 ID 写回 guest 句柄。
4. `hb_surface.pixels` 只在调用期有效。任何需要跨帧保存的后端必须复制到自己的内存，不能保留 guest 指针。
5. 宿主 → guest 的唯一通道是完成队列；`hb_wake` 只唤醒等待，不执行 guest。
6. 不共享结构体布局：所有跨边界结构体首字段为 `size`，字段显式固定宽度，不传 C 结构体数组，不依赖 padding/endianness。
7. 错误只映射为 `KErr*`；不得把 `SDL_*`、`errno`、`HRESULT` 直接返回给 L2。
8. L1/L0 不得引用游戏状态/规则字段（如 `engine+0x530`、物品 id、脚本名、场景状态）；渲染设计明确列出的只读渲染数据（相机块、0x58 三角形记录、材质）是唯一例外，且只在帧租约内按渲染设计读取，不得用于改变游戏状态。

**典型反模式**：

- 把 `CFbsBitmap*` 直接当贴图句柄传给 GPU 后端；
- 把 `TDesC16*` 存进宿主日志/异步任务，稍后解引用；
- 在 SDL 音频回调里调用 guest 的 `audio_callback`；
- 用 `pthread_create` 实现 `RThread`，再用互斥锁模拟 Symbian 调度；
- 用宿主绝对路径替换 Symbian 路径，导致存档位置随启动目录变化；
- 在宿主层解析 `.dat/.id/.itm` 做「兼容性修补」。

---

## 7. 平台适配原则

| 关注点 | 统一规则 | Linux | Windows | Steam Deck |
| --- | --- | --- | --- | --- |
| 语言/ABI | C11，固定宽度整数；跨边界不用 `long`/`size_t`；文件偏移 64 位 | `long` 64 位，仍需显式宽度 | `long` 32 位，禁止依赖其宽度 | 同 Linux |
| 路径 | L2 拆组件，L1 只收 root + 组件；不依赖 cwd；不接受盘符字符串 | ext4 大小写敏感，provider 模拟大小写不敏感 | 宽字符 API + `\\?\` 长路径；大小写不敏感 | microSD 常见 exFAT；不得依赖 POSIX 权限/硬链接 |
| 目录遍历 | 逐组件 `openat`/等价 API；拒绝 `..`、符号链接、重解析点 | `O_NOFOLLOW`、目录 fd | 检查 reparse point；句柄相对打开 | 同 Linux |
| 存档 | 字节与文件名不变；只写 `loadsave`；不建侧车 | 不用 `st_nlink==1` 作为唯一安全判据（FS 差异） | 共享模式与只读属性需显式处理 | 掉电/休眠后重新校验文件句柄 |
| 窗口 | 逻辑画面与输出分离；原生窗口句柄只在 L0 内部 | X11/Wayland 由 SDL 选择 | Win32/SDL；DPI 感知 | Gamescope；不依赖独占全屏 |
| 输入 | 物理扫描码 + 映射表；布局无关；失焦清零 | SDL 事件 | SDL/Raw Input 均可 | Steam Input → 同一逻辑动作 |
| 音频 | 16k 单声道 PCM16 → 后端重采样；设备热插拔 | PipeWire/ALSA | WASAPI | 同 Linux；休眠恢复后重开设备 |
| 渲染 | 兼容路径像素可重复；现代路径统一走 Vulkan | Vulkan | Vulkan | Vulkan |
| 时间 | 单调 + 墙钟分离；休眠不倒退 | `CLOCK_BOOTTIME` 优先 | 不休眠倒退的计数源 | 同 Linux；恢复不重放欠账 |
| 区域/编码 | 内部 UTF-8；Symbian 边界转 UTF-16；二进制模式 | locale 不影响解析 | 宽字符路径 | 同 Linux |
| 安全 | 仅访问传入根；不开网络；不执行 shell；URL 白名单 | — | — | — |

补充原则：

- **平台代码只在 L0**。L2 与 guest 不出现 `#ifdef _WIN32`；L1 头不出现平台类型。渲染 API/音频设备的选择通过后端注册与能力位，不在游戏代码里分支。
- **单一可执行文件 + 本地资产**。安装根与资源根由启动器传入；不写注册表、不写用户主目录（宿主配置除外）。
- **Linux 不是「默认平台」**。当前只在 Linux 验证过，接口仍需按上表在 Windows/Steam Deck 复核；Linux 特有的 `st_nlink`、`/proc`、`O_NOFOLLOW` 不得成为契约。
- **Steam Deck 是目标平台，不是特殊分支**：1280×800、手柄、休眠、exFAT 都应落在通用接口内解决。
- **不把宽屏/高分辨率做成新玩法**：碰撞、交互距离、镜头参数继续由原世界坐标和原镜头语义决定；逻辑画面 176×208 只决定 HUD/点击坐标系，输出尺寸变化只影响呈现。

---

## 8. 替换顺序

原则：先固定接口与基线，再按「影响面小、可独立回滚」的顺序替换；渲染最后，因为它依赖前面所有子系统的可重复性。

| 阶段 | 替换对象 | 保持的语义 | 通过门槛 | 回滚 |
| --- | --- | --- | --- | --- |
| **H0 接口壳** | 用 `hb_*` 包装现有 SDL/全局 shim，不换实现 | 行为零变化 | 同一路线 `guest frame` 哈希一致；无新增文件；计时日志不变 | 编译开关回退到内联 shim |
| **H1 文件/存档** | `game_files.h` → `hb_storage` | 路径映射、大小写、截断、文件集合、字节 | 槽位 0 存档语料逐文件哈希一致；重启可读；`tmp.id` 不变；只读资产拒绝写入 | `HB_STORAGE_LEGACY` |
| **H2 计时/调度** | 时间函数与等待 → `hb_clock`/`hb_wait` | 500 ms 心跳、微秒时间戳、日期显示 | 10 s 内心跳数在容差内；改系统时间不崩溃；休眠恢复不爆发欠账 | 旧时间函数 |
| **H3 音频** | `guest_audio.h` → `hb_audio` | 16k 单声道、单块在途、音量域、欠载回调 | 序章无持续欠载；音量/暂停/设备丢失行为正确 | `HB_AUDIO_LEGACY` |
| **H4 窗口/输入** | `publish_window` + `pump_window_input` → `hb_window`/`hb_input` | 176×208 逻辑画面、已验收键位、焦点丢失清键 | 序章手验键位不回归；Alt+Tab 后无卡键；指针坐标与视口一致 | 旧 SDL 路径 |
| **H5 媒体** | 媒体资产读取与影片呈现 → `hb_storage` + 最终帧 | 原解码、原配速、原跳帧、素材比例 | 开场影片完整播放；音画同步；留边不拉伸 | 旧 BitBlt 呈现 |
| **H6 窗口呈现/缩放** | 整纹理拉满 → 逻辑视口 + 缩放策略 | 兼容像素、guest 2D/影片绘制顺序 | 176×208 下哈希逐帧一致；宽屏安全区正确 | 旧拉伸 |
| **H7 3D 管线** | 软件光栅 → 现代后端（渲染子系统定义） | 场景/遮挡/透明/深度/雾/HUD | 验收矩阵 R1–R7；脚本与碰撞结果不变 | 兼容渲染路径 |
| **H8 设备能力** | 各设备导入 → 能力位/显式失败 | 空电话/空消息/无相机麦克风 | 触达时确定性失败、日志含首次地址/ordinal；无假数据 | 无需回滚（只读查询） |

顺序理由：

1. **H0 必须先做**：没有统一端口，后续替换会继续复制全局状态；但 H0 只做包装，不借机改行为。
2. **文件与计时先于一切验证**：存档字节和可重复的时间是后续像素/音频/影片对比的前提。
3. **音频、窗口/输入、媒体可独立验证**：它们的失败不会改写存档；但仍应逐项落地，避免同时变更多个线程/事件源。
4. **窗口呈现先于 3D**：先把「逻辑画面 → 输出画面」的缩放、像素基线做对，3D 后端才有稳定对照；guest 的 2D/影片绘制顺序不变。
5. **设备能力最后**：当前流程大多不触达；先保证不崩溃、不伪造，再讨论是否需要实现。

---

## 9. 风险登记

| 风险 | 触发 | 影响 | 检测 | 缓解 |
| --- | --- | --- | --- | --- |
| **ABI 句柄泄漏/stale ref** | 把 R 类 handle 或 guest 指针传入 L1；关闭后仍有完成 | 崩溃、错写文件、串流 | generation 断言；关闭即取消测试；头文件 grep | 私有 `hb_ref`；close 排空；禁用跨边界指针 |
| **32/64 位混淆** | guest 地址与宿主指针互转；用 `size_t` 存偏移 | 截断、越界 | 编译告警；边界类型检查 | 固定宽度；只在 L2 转换；64 位文件偏移 |
| **回调重入/死锁** | 宿主线程直接调用 guest；持锁等待 | 卡死、竞态 | 线程断言；音频/影片完成路径审计 | 完成队列；单 guest 线程；不在持锁时等待 |
| **存档字节漂移** | open 截断、换行转换、额外头部、大小写重写 | 读档失败、`tmp.id` 校验失败 | 逐文件 SHA-256；退档重读 | `r+b` 无截断；`SetSize(0)` 才截断；二进制模式；不建侧车 |
| **部分存档集** | 崩溃/断电落在多文件写入中间 | 槽位混合状态 | 人为中断测试 | 第一阶段保持原失败可见性；备份方案单独评审，不改文件名 |
| **时间跳变/休眠** | NTP、手动改时、挂起恢复 | 心跳停摆、补帧风暴、随机序列变化 | 改时钟/挂起测试；心跳计数 | 单调与墙钟分离；恢复只补一次；不缓存时区 |
| **音频欠载/延迟/设备丢失** | 后端缓冲过小、重采样错误、拔设备 | 爆音、静音、卡顿 | 欠载计数；设备热插拔 | 单块在途；后端内部重采样；丢失走显式错误并重开 |
| **影片配速/编解码漂移** | 宿主解码时钟与 guest 不一致；宽高比处理错误 | 音画不同步、拉伸 | 与 guest 解码逐帧对比；开场影片 | 默认 guest 解码；宿主解码需 parity 门槛；按素材比例留边 |
| **输入回归/卡键** | 映射表改动、失焦未清键、指针视口不一致 | 无法操作、误用物品 | 序章手验；Alt+Tab；点击坐标断言 | 映射集中一处；失焦合成抬起；视口变换共享 |
| **渲染像素漂移** | RGB565 精度、alpha/深度顺序、雾/AA | 基线不通过、遮挡错误 | FNV 哈希；截图对照 | 兼容路径先逐帧一致；增强项独立开关 |
| **文本缺字/度量变化** | 无 CJK 字体、重栅格改变布局 | 方框、文本溢出 | 字体覆盖检查；对白/菜单截图 | 内置/声明字体依赖；默认不重排布局 |
| **路径/大小写/链接** | ext4 大小写敏感、Windows 重解析点、exFAT | 资源找不到、存档写到错误位置 | 大小写变体、符号链接、`..` 测试 | provider 模拟大小写；组件校验；拒绝链接 |
| **范围蔓延** | 以「宿主抽象」为名加入相机/云存档/新玩法 | 违反需求边界 | 文档/评审对照 `pc-modernization.md` | L1/L0 禁止游戏规则；能力位为 0 |

---

## 10. 验收项

### 10.1 接口契约（静态，可自动化）

1. `hb_*.h` 独立编译；不包含 guest/ABI 头；不出现 Symbian 类型名（白名单仅注释）。
2. `hb_*` 结构体首字段为 `size`；无 `void*` 持久字段；无「guest 地址」字段；无平台句柄类型。
3. `hb_ref` 关闭后 generation 递增；在途完成事件被丢弃的单元测试通过。
4. 宿主线程不得进入 guest：以桩函数替换 guest 入口，确认音频/影片/设备完成路径只入队。
5. 静态检查 L1 不引用游戏字段偏移、物品 id、脚本名、场景状态。

### 10.2 行为验收

| 项 | 判据 | 证据等级 |
| --- | --- | --- |
| 存档字节 | 参考运行与替换后逐文件字节一致；槽位 0 重启可读；`tmp.id` 不变；无新增文件 | A（需资产） |
| 资产只读 | 对资源根写入失败；`texture\scene\` 缺失不回退 | A |
| 路径安全 | `..`、绝对路径、符号链接、大小写变体按规则处理 | A |
| 心跳 | 500 ms `CPeriodic` 计数在容差内；改墙钟不影响单调计时；休眠恢复无爆发 | A/P |
| 音频 | 16k 单声道；音量 0..128；暂停/恢复；设备丢失显式错误；无持续欠载 | A/P/U |
| 输入 | 序章已验收键位不回归；失焦清键；指针逻辑坐标正确（启用时） | P/U |
| 影片 | 开场影片完整；音画同步；按素材比例留边；跳帧不改变剧情 | P/U |
| 兼容呈现 | 176×208 下逐帧哈希与基线一致；宽屏视口/安全区正确 | A/P |
| 现代 3D | 按 [验收矩阵](pc-modernization-verification.md) R1–R7 逐条结案 | 见矩阵 |
| 设备能力 | 相机/麦克风/电话/短信/网络能力为 0；触达时确定性失败并记录首次地址/ordinal | A/P |

### 10.3 平台矩阵与回滚

- Linux x64 为当前主验证平台；Windows x64、Steam Deck（SteamOS）为后续平台。每个平台至少完成：启动、序章场景、存档写读、音频、影片、窗口缩放/失焦。
- Steam Deck 追加：1280×800、手柄映射、休眠/恢复、microSD 存档读写。
- 每个阶段保留编译/启动开关回退到上一实现；回退后同一存档仍可读写，证明替换未改变格式。
- **S 级静态事实不能单独结案**；A/P/U 的定义与记录模板见验收矩阵。

---

## 11. 未验证项与待决策

| 项 | 缺什么 | 建议下一步 |
| --- | --- | --- |
| 相机/麦克风是否可达 | 正常流程未见调用；设备 DLL 导入面未全部核对 | 先补静态调用图/运行日志；不可达则不实现 |
| 目录枚举 | 当前 shim 无 `CDirScan` 绑定；注册文件搜索是否需要未验证 | 确认是否触达；若需要，先定义大小写/排序语义 |
| 部分存档集 | 原文崩溃/断电后的可见状态未测 | 人为中断测试，记录后再决定是否引入备份 |
| 挂起/恢复 | 原 Symbian 定时器语义、当前 SDL 后端行为未测 | 在目标平台做休眠前后心跳/音频/文件测试 |
| 宿主解码 parity | `hb_decode_*` 未实现，逐帧对比方法未定 | 先保留 guest 解码；优化前建立逐帧差异指标 |
| 高分辨率文本 | 重栅格是否改变布局、字体依赖如何分发未验证 | 默认沿用逻辑分辨率栅格；重栅格单独立项 |
| 逻辑尺寸/画幅策略 | 宽屏附加区 vs 留边的最终策略由渲染/验收文档决定 | 本文只固定「逻辑≠输出」接口 |
| SDL2/SDL3 与 GPU 后端 | 迁移时机、Vulkan/D3D 后端选择未定 | 后端可替换；先以 SDL2 完成接口抽取 |
| 500 ms 心跳与渲染关系 | `CPeriodic` 之外是否还有独立帧驱动未完全确认 | 补调用图；宿主不得自行加帧率 |
| 墙钟对存档/校验的影响 | 日期显示之外的写入字段未逐一确认 | 改时钟测试 + 存档字节对比 |
| 240×320 / 352×416 路径 | 原版存在资源/缩放因子，PC 流程是否触达未验证 | 确认可达性后再纳入窗口/呈现测试 |

---

## 12. 关联文档

- [PC 版现代化需求](pc-modernization.md)：需求优先级最高；本文与其冲突时以主文档为准。
- [移植架构与诊断](runtime.md)：当前构建、运行、日志与已知缺陷。
- [序章手动验收](first-level.md)：已验收的键位与序章路线。
- [PC 版现代化验收矩阵](pc-modernization-verification.md)：R/I/C 条目与证据等级。
- [PC 输入与背包设计](pc-input-inventory-design.md)：输入上下文、指针语义、暂停、键位配置。
- [PC 渲染设计说明](pc-rendering-design.md)：3D 管线、接入点 A/B/C、材质、雾、宽屏画幅与像素基线；本文只定义它与宿主/窗口的接缝。
