# CS2 POV：游戏原生逻辑与修复记录

此文档是持续维护的逆向记录。每次调查按 **DLL 版本 → 功能** 增补，分别记录游戏原生行为、POV 实现、开关、证据及验证边界。地址只对对应哈希有效；更新游戏后必须重新定位，不能把历史地址当作新版本结论。

## 记录模板

新增条目应包含：

1. 日期、模块、完整 SHA-256、分析 image base，以及能取得的游戏版本信息。
2. 原生触发条件、阈值、事件先后、状态清理、输出效果。
3. 原生函数与调用点的 RVA／分析 VA；真正 detour 的入口、签名／vtable slot；仅调用的 helper 要单独标明。
4. POV 与原生逻辑的差异、最小修复及源码位置。
5. 普通 Release 可用的独立 debug 开关、默认值、生效时机、关闭范围及残留状态处理。
6. 静态证据、实际运行版本、正反向测试与未验证项。推断和近似必须显式注明。

新增功能统一注册到 `mirv_pov_debug_feature <effect> <0|1>`，不再另建 debug 命令入口。不得仅依赖 `hud`、`feedback` 之类的模块总开关，也不得把用户开关编译进仅诊断构建。旧版本记录保留，新版本另开条目。

## 2026-09-21：当前分析版本

| 模块 | SHA-256 | 分析 image base |
| --- | --- | --- |
| client.dll | `a0c195f0b6ec00915ef08c548200a010ebbe7982d3a4bc468cad939b67c8c4e3` | `0x180000000` |
| server.dll | `1cac9113b10037c0ba8fb739e5538c21d7ddaee5529325ca4f7e9a241fad43cc` | `0x180000000` |

来源：本机 CS2 `game/csgo/bin/win64/`。没有将未核实的营销版本号当作构建标识。表内 VA 是静态分析地址，ASLR 下运行地址应为实际模块基址 + RVA。

### 失聪与耳鸣

#### 游戏原逻辑

**闪光弹**：服务器先发 `flashbang_detonate`，再执行 RadiusFlash。后者把爆炸源 Z 加 1，计算玩家眼睛到该点的距离，结合可见性／有效作用范围判断是否产生 `player_blind`。失明时长不是失聪分档依据。对符合闪光作用条件的玩家，原生声音选择如下：

| 眼睛到修正后爆炸点距离 d（游戏单位） | DSP 控制 | 同时播放 |
| --- | --- | --- |
| `0 <= d < 100` | `control.deafenLong` | `Flashbang.Ring.Long` |
| `100 <= d < 500` | `control.deafenMedium` | `Flashbang.Ring.Medium` |
| `500 <= d < 1000` | `control.deafenShort` | `Flashbang.Ring.Short` |
| `d >= 1000` | 此路径不触发失聪 | 此路径不播放耳鸣 |

**HE**：原生伤害处理器要求 blast 伤害路径（`DMG_BLAST`, `0x40`），伤害结果的 damage-dealt 转整数达到 `30`，且服务器 `m_bTargetBombed` 为 false。满足条件时同时触发 `control.deafenHE` 和 `Flashbang.Ring.Long`；不是任何非零 HE 伤害都触发，也不按剩余生命值分档。

#### 原生点位与 POV 接入

| 模块 / 用途 | RVA | 分析 VA | 接入方式 |
| --- | --- | --- | --- |
| server：闪光声音分档 | `0x226040` | `0x180226040` | 只分析，不 hook |
| server：RadiusFlash / Z+1 / player_blind | `0x39DBB0` | `0x18039DBB0` | 只分析，不 hook |
| server：闪光爆炸事件先于 RadiusFlash | `0x39B4C0` | `0x18039B4C0` | 只分析，不 hook |
| server：HE 声音门槛 | `0x1E0880` | `0x1801E0880` | 只分析，不 hook |
| server：blast 伤害调用入口 | `0xA5E750` | `0x180A5E750` | 只分析，不 hook |
| client：AudioParameter 处理器 | `0xB59A40` | `0x180B59A40` | 定位 SoundSystem；不 detour 此函数 |
| client：SoundSystem 全局槽 | `0x25F1248` | `0x1825F1248` | 经 RIP 相对指令解析，不硬编码运行地址 |
| client：CGameEventManager::FireEventClientSide | RTTI 定位 | vtable slot `8` | 现有实际 hook，原生分发前交给 POV 事件处理器 |

AudioParameter 签名为 `8B 41 ?? 48 8B D1 39 05`。实现验证 handler `+0x52` 的 `48 8B 0D` 及控制调用尾部，解析 SoundSystem，再调用其 vtable 字节偏移 `0x1B8`（slot 55）的 trigger-control。控制 hash：HE `0xB60B5483`、Short `0x523F9894`、Medium `0x67BDB4CB`、Long `0xF339FBBB`。

本地耳鸣通过 SendAudio 原生 helper 播放：入口签名 `40 53 48 83 EC 60 48 8B 59 48 48 83 E3 FC 48 83 7B 18 0F 76`，取 `+0x35` call 的本地玩家 filter 构造器及 `+0x64` call 的本地声音播放函数，校验 call opcode 和函数前缀。不 detour SendAudio 来屏蔽其他声音。

POV 路径：

- `flashbang_detonate` 缓存实体 ID、当前 POV pawn handle、framecount 和重建距离；对应 `player_blind` 的三项身份／时间检查通过后消费缓存，按距离选择原生 DSP 与耳鸣。
- HE 从当前 POV 受害者的 `player_hurt` 读取 `weapon` 和 `dmg_health >= 30`。服务器 `m_bTargetBombed` 不在客户端 schema 中，不能直接读取；客户端改用同步的 `m_eRoundWinReason == 1`（TargetBombed）作为排除条件。当前运行验证得到此字段偏移 `2480`，由 schema 动态解析。
- round_start、POV 选择重置及失聪开关操作会清除闪光关联缓存。
- 不重设全局音量，不压掉原生声音消息。此为远端 POV 的补偿路径。

源码：`AfxHookSource2/GameEvents.cpp`、`MirvPovFeedback.cpp`（Initialize / HandleFlashDetonate / HandlePlayerBlind / HandleHeGrenadeHurt）、`SchemaSystem.cpp`。

#### 独立 debug 开关

```text
mirv_pov_debug_feature            // 查看所有 feature 的 active / configured 状态
mirv_pov_debug_feature deafen 0   // 关闭 POV 补加的闪光／HE 失聪及耳鸣
mirv_pov_debug_feature deafen 1   // 恢复，默认值为 1
```

普通 Release 中可用；也接受 on/off、true/false。立即影响后续事件，不需要重开 `mirv_pov`。关闭不会关闭命中反馈、伤害方向、闪光画面、HUD 或语音；原生游戏自身播放的声音仍保持原样。已经触发的 DSP／耳鸣自然结束，不调用可能影响原生音频的全局重置。切换时清空闪光缓存，防止重新开启后补播旧事件。设定保留到本进程结束，`mirv_pov 0/1` 和换 demo 不重置它；不写用户配置文件。

#### 证据与边界

统一 feature 开关验证：普通非诊断 Release 的 9 项检查通过，包含默认开启、立即关闭／恢复、非法输入不改变状态、POV 总开关不重置 deafen、旧 radio feature 保留 pending／下次启用时应用的语义。随后带日志的 Release 执行 16 步事件回归：关闭时真实 HE 54 点伤害及闪光致盲事件仍到达，但不产生 POV 失聪／耳鸣调用；保持 POV 开启，仅将 deafen 置 1 后，同样片段恢复原生 DSP 与耳鸣调用。关闭期间并未停止整个 feedback 分发。

最终非诊断 Release 冷启动 9 项检查再次通过（`feature-final-report.json`）；实际加载模块及 SHA-256 记录在 `feature-final-loaded.json`，交付目录为 `build/staging-pov-feature-deafen/`。记录：`feature-release-report.json`、`feature-effects-retry-report.json` 及同名 NetCon 日志，位于上述诊断目录。第一次事件测试在连接 NetCon 前超时，未运行断言；重启 HLAE 后重试通过，不把启动超时写成功能失败。普通 Release 也保留完整 feature 注册表；日志详细程度仍由 `AFX_MIRV_POV_DIAGNOSTICS` 决定。除标为 immediate 的 deafen 外，既有 feature 继续在下次启用 POV 时应用。

本地证据目录：`diagnostics/pov-native-20260921/`，包括对应地址的反编译、audio.json、client-rules.json、验证报告和 NetCon 日志。2026-09-21 原修复的 18 步实机回归确认：HE 54 点触发、16 点不触发；闪光 969.089 单位走 Short，324.061／126.550 单位走 Medium；原生 DSP 与耳鸣 helper 均成功执行。Release 冷启动 5 步 smoke 通过，加载模块哈希已核对。

这不等于音频波形或听感完全相同：未做 live/POV 定量音频比较；Long 距离档、准确阈值边缘、爆炸结束时序和同帧多闪光未做运行验证。客户端眼睛插值可能影响边缘分档。`dmg_health` 与所有伤害修饰下的原生 damage-dealt 完全等价尚未证明；同步回合结果是服务器 bombed 条件的客户端近似。原生音频与补偿路径是否重复播放尚未定量检测。

### 多次击杀 HUD 特效

原生 client `HealthAmmoCenter` 更新函数 RVA `0xE252F0`（VA `0x180E252F0`）查询观察者模式。调用点 RVA `0xE25879`、`0xE25883`（返回点 `0xE2587E`、`0xE25888`）调用 observer helper RVA `0xB2CC70`。模式为 2／3 时选择 `ui_hud_kill_streaks_spectator_*`，否则选择普通玩家粒子组。

签名：`E8 ?? ?? ?? ?? 83 F8 02 74 0A E8 ?? ?? ?? ?? 83 F8 03 75 09 48 85 DB 74 04 B2 01`；当前版本唯一命中，两个 call 目标一致。POV detour observer helper，仅对上述两个返回地址返回普通玩家模式 0；其余查询维持原值。保留原生击杀队列、动画及播放次数。

源码：`MirvPovHud.cpp` 的 `MirvPovHud_InstallObserverModeHook` / `New_HudObserverMode`。证据：`kills-sheet.png` 及 `capture/two-kills/`，19.dem 中 Koraa 的同回合两次击杀（9852、10049 tick）未再出现白色扇形观察者特效。现有门控为 POV 总开关及hud 模块门控；尚未将此历史功能改造成独立 Release 效果开关。

### 顶部玩家名称与冻结期

原生 TeamCounter 自己的 panel 拥有 `ROUNDDOWNTIME`，不是外层 HUD root。CSS 通过祖先选择器展示 `.AvatarL__name`。

| 原生 client 函数 | RVA | 逻辑 |
| --- | --- | --- |
| round_freeze_end 处理 | `0xE43660` | 移除 FREEZETIME 与 ROUNDDOWNTIME |
| round_end 处理 | `0xE4DE40` | 加入 ROUNDDOWNTIME |
| round_start 处理 | `0xE4E020` | 根据 GameRules 加入 FREEZETIME |

这里的函数是原逻辑证据，POV 没有新增这些函数的 detour。`MirvPovHud.cpp` 的 panel 遍历继承祖先的 `ROUNDDOWNTIME || FREEZETIME`，冻结／间歇期显示名称，之后隐藏；停用 POV 恢复可见性。必须包含 FREEZETIME：第一次冷启动没有之前 round_end 留下的 ROUNDDOWNTIME，只看后者会漏掉首回合。

首回合冷启动与后续回合截图／显隐断言已验证。证据见 verified-report.json、capture/freeze、capture/live。此历史功能目前随 hud 模块及 POV 总开关控制；今后新增效果必须提供独立 Release 开关。

## 2026-09-22：顶部名称过渡与短距离 demo 跳转

本条取代上方名称修复中直接修改 `visibility` 的实现，保留旧条作为历史记录。client.dll 仍为 `a0c195f0b6ec00915ef08c548200a010ebbe7982d3a4bc468cad939b67c8c4e3`；本次分析的 **panorama.dll** 为 `a607e0e4a7fdd1cf3c7f828c7a0f56da6d84eee93010972a56a94ade92461c41`，二者 image base 均为 `0x180000000`。不要混用 panoramauiclient.dll 的点位。

### 游戏原逻辑

- `hudteamcounter.css` 定义 `AvatarNameHeight: 14px`；`.AvatarL__name` 默认 `height: 0px; transition-property: height; transition-duration: 0.5s`。competitive / scrimcomp2v2 模式下，祖先 `ROUNDDOWNTIME`、`HUD--localplayer--dead` 或 `HUD--localplayer--spectator` 使高度为 14px。原生动画是高度过渡，不是可见性开关。
- client 的 round_end（RVA `0xE4DE40`）添加 ROUNDDOWNTIME；round_start（`0xE4E020`）读取 GameRules +64 后添加 FREEZETIME；round_freeze_end（`0xE43660`）移除两者。类添加／移除使用 panel vtable slots 144 / 147。这些事件处理器未新增 detour。CSS 本身只使用 ROUNDDOWNTIME；首回合冻结期的 FREEZETIME 是 POV 补齐条件，不能混称为原生 CSS 选择器。
- Panorama height 属性布局 24 字节：vtable +0，property ID +8，禁止过渡标志 +9，float +16，单位 +20（1 为 px）。构造器 RVA `0x177470`，克隆 `0x1774C0`，高度 vtable `0x46C5D0`；所有分析 VA 为 image base + RVA。
- style setter RVA `0x195810`：等值比较（height vtable slot 15，RVA `0x180030`）通过时不重启过渡；否则克隆属性并进入 `0x195B00`，结合 CSS 样式更新。merge（slot 1，`0x17FC50`）会用旧值填充未设置单位，因此不能通过提交一个 unset height 来清除覆盖。
- 原生单属性清除 RVA `0x1A3F50`，CPanelStyle vtable（RVA `0x474710`）slot **151**：销毁指定 inline property，重算其 CSS 值（`0x197DD0`），令 panel 样式失效；不清除其他属性。它只是被调用的 native helper，不是 hook。

### POV 实现与 HUD 控制

`MirvPovHud.cpp` 遍历当前 HUD 的 AvatarL__name，仅在 competitive / scrimcomp2v2 祖先下覆盖 height。每帧读取 schema 动态解析的 `C_CSGameRules.m_bFreezePeriod` 与 `m_eRoundWinReason`：冻结期或非零回合结果取 14px，其余取 0px。不再从事件驱动的 HUD 类推断目标 tick，也不依赖“跳了多少 tick”的阈值。GameRules 查找只缓存 entity index，并在每次读取时重新核验实体类；缺字段／缺实体时恢复原生 CSS。该同步条件是 POV 对冻结／回合间歇的重建，不声称原生 CSS 直接读取这两个字段。

复用既有 Panorama setter/resolver；height/CPanelStyle vtable 通过 RTTI `.?AVCStylePropertyHeight@panorama@@` / `.?AVCPanelStyle@panorama@@` 定位。setter 的既有 call-site 签名为 `E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 45 ?? EB`，当前首个命中 VA `0x1801027C7` 调用 `0x180195810`。调用清除 helper 前核对实际 style vtable。没有新增名称事件 hook 或手写插值动画。

按用户同日明确要求，名称修复归入现有 `hud` 模块，不保留 `playernames` 独立 feature。通过 `mirv_pov_debug_feature hud 0/1` 统一控制，默认 1，沿用 HUD 的下次启用生效语义：POV 已开启时，修改后执行 `mirv_pov 0`、`mirv_pov 1`。停用 POV 会清除带 `afx-pov-playernames` 内部所有权标记的 height 覆盖并恢复原生 CSS；重新启用时仅在 hud 开启的情况下接管名称。此标记只是清理依据，不是用户开关。恢复不强制写死 14px，也不重设其他 inline 样式。

证据：`diagnostics/pov-names-20260922/panorama-a607-native-height.json`、同目录保存的 panorama.dll.i64，以及 `diagnostics/pov-native-20260921/resources/panorama/styles/hud/hudteamcounter.vcss_c.txt` 和原 client round 处理器反编译。

### 运行验证与边界

以下 17／28 步报告和 DLL 哈希记录的是移除独立开关前的版本，保留为名称动画与跳转逻辑的运行证据；其中独立 playernames 开关测试不代表当前命令接口。

归入 HUD 后，已核对注册表和命令帮助不再含 playernames feature，名称门控改为 hud；Release x64 重新构建及 `git diff --check` 通过（`build-hud-control.log`）。本次仅调整控制归属，未重跑游戏验证或替换运行中的测试 DLL。

- Release x64 的诊断／非诊断两种构建均通过，`git diff --check` 通过。诊断版 17 步回归通过（`verified-report.json`）：19.dem 首回合 `1430 ↔ 1460`、第二回合 `5440 ↔ 5460` 跨冻结期短前跳／后跳，暂停状态正确回到 14 / 0；feature 关闭、恢复及 POV 关闭、恢复均通过。实际加载路径／哈希以 `loaded-diagnostic.json` 为准，reuse-only 报告中的默认 hook 参数不是模块测量值。
- 30 fps 录制 `animation/take0000/`，`animation-sheet.png` 的 20 / 25 / 30 / 35 帧显示名称栏逐渐收起，确认原生高度过渡实际执行，而不只是目标值日志改变。
- 最终普通 Release 冷启动 28 步通过（`release-report.json`），包含默认开启、feature off/on、关闭状态跨 POV 总开关保留、主开关恢复，以及冻结期／live／后跳截图。`release-states.png` 的 8 组图像确认：关闭 feature 或 POV 恢复原生观战名称，重开修复在 live 收起名称，后跳冻结期恢复名称；hud/deafen feature 状态未随名称开关改变。
- 最终实际加载 `build/staging-pov-names-release/x64/AfxHookSource2.dll`，SHA-256 `fa89dee6e03f96ac1446ac87695b7d3b383bc103fffeb257a03d79ba40f35ab5`，见 `loaded-release.json`。此次未替换 HLAE 安装目录 DLL。
- 前两次脚本断言失败是跳转后继续播放，启用时已越过冻结期；不能算冻结期功能失败。最终使用 `demo_timescale 0` 固定跳转目标，录制正常过渡时恢复 1，并检查日志实际目标 tick。

静态分析已证明旧 visibility 路径绕过原生动画；本次实测证明上述短跳转和开关场景下的新实现正确。没有取得用户最初出错的具体 demo/tick，也未对旧 DLL 做同片段 A/B，所以“短 seek 遗留 CSS 类”仍是工作解释，不能把它写成已复现的唯一根因。未覆盖所有游戏模式、死亡视角、任意 demo 或游戏更新后的 ABI；未知 schema 恢复原生 CSS，不猜偏移。

## 2026-09-23：游戏更新后的 POV 兼容性静态核对

### 构建身份与范围

用户更新本机 Steam 游戏后重新复制、计算哈希并分析，未把更新前安装的 DLL 当作新版。下列模块分析 image base 均为 `0x180000000`；本节地址统一写 RVA，分析 VA = image base + RVA，实际运行地址须使用 ASLR 模块基址。

| 模块 | SHA-256 |
| --- | --- |
| client.dll | `40bce8206f51b92ee05d6121c6e42c717bf3fa0cc0edeeb6698744b1c4799feb` |
| panorama.dll | `b4dd42375d7224363d42a69438228f55ad82dcbfd5bc55b10f44cf12a0d66f57` |
| engine2.dll | `b0bad7a87e232b5807c8254961c2d9fae281fa279ca765906fdf42fc54387d8c` |
| schemasystem.dll | `c784a02d503f8a4db2506468ff237189b64cc94d012df0c225ed3f47efddcf15` |
| soundsystem.dll | `d28fd4a86a8d9a46c143c4af0cdd48978e0cc17524351484812aa44935266f8a` |

旧 client `a0c195f0...` 的库保留；新库位于 `diagnostics/pov-update-20260923/client-analysis/40bce8206f51b92e/`，新 image size `0x2993000`。这次只修改 POV 与其直接共享依赖。原报告的 SceneSystem 1746/1749/1752、MirvColors 301、main 1108、ClientEntitySystem 471、addresses 120 分别属于场景渲染、颜色、相机、附件查询和 RenderService 路径，不为消除弹窗而修改它们。共享实体类型判断、Panorama 布局加载虽不在 MirvPov 命名文件中，但属于 POV 必需依赖，纳入修复。

### 游戏原生逻辑与本次实现

| 功能 / 源码 | 新版原生证据与接入 |
| --- | --- |
| 实体识别，ClientEntitySystem.cpp | pawn 主 vtable `0x1C82E78`、controller 主 vtable `0x1C05330`。IsPlayerPawn / IsPlayerController 为 slots **158/159**，eye origin / angles 为 **173/174**；旧 155/156 已是实体 `+0x5F8` 读写函数。GetClientClass 保持 slot 48。按新槽位调用，并按 schema 的 uint8 类型读取 m_iTeamNum，避免连读相邻三字节影响 POV 队伍判断。 |
| 伤害反馈，MirvPovFeedback.cpp | detour Damage 消息 `0xE80390`、HUD 构造 `0xE71350`；被调用而不 detour 的方向 helper 更新为 `0xE75B70`。原消息 amount `+0x50`、victim `+0x54`、source 指针 `+0x48`，source xyz `+0x18/+0x1C/+0x20` 保持。 |
| 失聪与耳鸣，MirvPovFeedback.cpp | AudioParameter `0xB9EB70 +0x52` 仍为 SoundSystem RIP load，之后 slot 55 / `+0x1B8` 控制调用形状不变。SendAudio `0xB9EC10 +0x35/+0x64` 分别调用 `0xA7A390/0xC19FF0`。它们是原生播放 helper，不新增 detour；本次没有重新验证 server 的伤害/距离阈值，历史服务器结论不能当作本次新版实测。 |
| 计分板，MirvPovScoreboard.cpp | UserCommands handler detour `0xB79AA0`；条目 player slot 从 `+0x34` 移到 **+0x38**。外层 count `+0x48`、vector `+0x50`、entries `+8`、条目 tagged string `+0x18` 保持；增加空 string 对象检查。HLTV parser `0x63B7D0` 的字段 6 仍写 bool `+0x40`。 |
| 拾取提示，MirvPovPickupPrompt.cpp | hint builder `0xEAFAE0 +0x0F` 调用 `0xC7A420`；active builder `0xEAD8C0`。原短 caller 签名同时命中另一函数，现限定调用点 `0xEA90DE` 的栈/对象上下文。pickup target updater `0xC9D870` 保持签名。 |
| 雷达 C4，MirvPovRadar.cpp | package update detour `0xEC0930`，原生面板 holder **+0x2A0**、flags **+0x17808**、carrier handle **+0x17810**。其 `0xEC0AEF` 调用 HUD lookup `0xE7B7A0`，`0xEC0B24` 调用 slot resolver `0xEADDA0`。不使用相邻的玩家雷达更新 `0xECCCC0` 代替 package update。色号 helper `0x888C50`、颜色输出 helper `0x888BA0`；后者无有效色号时输出灰色，合法色号进入竞争色表，仍是被调用的 helper。 |
| 雷达阵营与内联补丁，MirvPovRadar.cpp | relationship call `0xEB83A9` → `0x8D8250`，返回 `0xEB83AE`，栈存储从 rbp+0x90 改为 +0xA0。队伍显示 patch `0xEB85EA` 仍使用 r15 pawn；竞争色路径 `0xECCD7E`，CT/T calls `0xECCF40/0xECCEAC`，enemy style `0xEBF1E5` 仍读写 ebx style。既有最小覆盖长度和寄存器契约保持。 |
| 声音圈，MirvPovSoundCircle.cpp | producer `0xEABC55` 与 queue `0xEB8B35` 现在调用观察目标感知 getter **0xC7A420**；position updater `0xECE549` 仍调用真实本地 pawn getter **0xC7AAD0**。不能再要求三者目标相同。分别 detour 两个 getter，仍仅在三处返回地址替换为 POV pawn；其他调用保留原生返回。 |
| 声音消息与补播，MirvPovSoundCircle.cpp | DoStartSoundEvent detour `0x3CFE80` 的 `+0xEC` call → `0x3AE3C0`；后者 `+0xB8` 解析声音接口槽 `0x27925F0`。消息布局至 `0x68`、字段 `+0x40…+0x60` 保持。直接补播 helper 为 **0x3EB040**，独立入口签名替代不唯一的中间锚点；它仅被调用，不 detour。 |
| 语音状态，MirvPovVoice.cpp | ServerVoiceData detour **0xB45610**；原生首先检查消息 has-bits `+0x40`：`0x100` 时使用 `+0x6C` 的实体 index，再转为零基 slot；否则 `0x80` 使用 `+0x68` slot。POV 同步这一路径。UpdateSpeakerStatus helper **0xC1CA50**、VoiceStatus getter `0xC0ACB0`；IsPlayingDemo 展示调用返回点 `0xB458A6`。 |
| 死亡面板，MirvPovDeathPanel.cpp / DeathMsg.cpp | 新死亡监听器 `0xE834C0` 的 `0xE836BF` 直接读取 ConVar 槽 **0x249C560**，判断对象 **+0x58**；旧 resolve/fallback helper 链已不适用。临时开放 replay-others 门控现在按这个布局取值，仍保存并恢复原字节。主面板 visible **+0x1A1** 经 `0xE88C70` 确认；secondary setter `0xE88D90`，show `0xE840E0`、hide `0xE80DC0`。 |
| HUD 闪光，MirvPovHud.cpp | compact call `0x11C1E8B` 与 per-view call `0x11D9FC5` 均到 `0xCE0C70`；per-view 签名随寄存器/结构字段更新。两处返回地址门控保留。 |
| HUD 购买区，MirvPovHud.cpp | HudMoney 更新 `0xEC5210` 中 `0xEC549D` 连续序列解析 GameRules 槽 **0x255AA88**、buy-enabled **0x75C1F0**、time-elapsed **0x76BCE0**、pawn-zone **0x8D7DC0**。最后一个原生 predicate 同时处理 mp_buy_anywhere 与 pawn zone。移除已过期的三个硬编码 RVA，按同一调用链定位；这些 predicate 只调用，不 hook。 |
| 击杀奖励文字，MirvPovKillReward.cpp | 旧 TextMsg prologue 在新版误命中无关的 `0x9DB94A`。改为真正 handler **0x11A3160**，重复字符串 count / at 调用从 `+0x94/+0xAD` 改到 **+0xB5/+0xCC**，对应 helpers `0x122EF90/0x122D5A0`。TextMsg params `+0x48`、destination `+0x60` 保持。SayText `0x11A30C0 +0x47` → `0x119F8F0`，其 `+0xC3/+0xEA` → `0xE7B7A0/0xEB9A50`，仍调用原生本地化/notice helper。 |
| 无线电，MirvPovRadio.cpp | 按 RTTI 解析 delegate slot 5：RadioText table `0x1D1A2F0` → `0x11A0E20`；SendAudio table `0x1C61260` → `0xB90EB0`；RawAudio table `0x1D1A3D0` → `0x11A0FF0`。删除旧地址 fallback，避免“槽位可执行”却是错误消息类型。 |
| 无线电 emitter / formatter，MirvPovRadio.cpp | emitter 通用签名有多个克隆，增加 `+0x65` RIP LEA 与 `CUserMessageSendAudio_t` RTTI table **0x1C60B78** 相等的检查，唯一入口 **0xB8BA50**。typed parser **0xB9EC10**、RadioText formatter **0x11A2040**、RawAudio formatter **0x11A2830** 各自原 ABI 保持；demo controller getter `0xD341B0`、suppress byte `+0x72` 保持。保留消息来源和 typed/wrapper 参数区别。 |
| Panorama 布局依赖，DeathMsg.cpp | `CLayoutFile::LoadFromFile` 为 panorama **0x157A30**；旧签名锁定了原生源码行号 0x3F4，新版为 0x3FD，导致 POV layout 通知链不初始化。将行号通配并补足后续指令上下文，参数仍为 (layout, filename, byte)。 |

### 购买菜单：整组版本迁移

`MirvPovBuyMenu.cpp` 保留完整 SHA-256 门控，仅允许本节的 client。它复用原生 open → refresh/hover → preview/equip → close 顺序；POV 用记录玩家的控制器、服务器 loadout 和模型替换 viewer 数据，阻止真实购买/出售及写回记录的 buy-menu bit，不改变已有 feature 开关。

| 用途 | 新 RVA | 接入 |
| --- | --- | --- |
| open / close | `0xDB4AA0 / 0xD9DC50` | detour |
| refresh / full refresh | `0xDB8580 / 0xDB8620` | detour |
| hover / think | `0xDBFB20 / 0xDA6300` | detour |
| local pawn / controller | `0x9698F0 / 0x9698B0` | 按购买菜单 scope detour |
| loadout / hover loadout | `0x903150 / 0x9030B0` | 按 scope detour |
| write buy-menu bit | `0xC93350` | detour |
| purchase / sell | `0xDA71E0 / 0xDA7520` | detour |
| model / select / SetPlayerModel | `0xDBE3D0 / 0xDB4150 / 0xE59BD0` | detour |
| EventOpenBuyMenu creator | `0xDAF500` | 仅调用 |
| symbol / item lookup / pawn model | `0x177E630 / 0x112DCC0 / 0x21C060` | 仅调用 |
| inventory manager / default loadout | `0x838C70 / 0x83B820` | 仅调用 |
| acquire / owned weapon | `0x8BFCA0 / 0x8FE030` | 状态诊断 helper，仅调用 |

原生 `0x903D00` 明确遍历 inventory `+0x88` count、`+0x90` pointer，记录 56 字节，team/slot/definition 在 `+48/+50/+52`，缺失时回退 `0x83B820`。新版 default table 有 58 个 slot、item stride 1456（旧为 57 / 1136）；POV 调用原生 lookup，不自行索引这个表。

显式字段已迁移：controller inventory `0x818 → 0x820`、pawn weapon/item services `0x1208/0x1210 → 0x12F0/0x12F8`、buy-menu bit `0x150A → 0x15EA`。面板 `520` preview、`528` item cache、`568` donate flag、五组 `128+56*g` count / `136+56*g` vector 与 88-byte records 保持。preview 当前槽/数量/指针从 **2252/2256/2264 → 2284/2288/2296**，槽记录 **152 → 160** 字节，agent item ID 仍为 record+40；`0xE599F0/0xE59BD0/0xE56960` 分别佐证 item ID、model 与槽数量。dirty 标志 **2208 → 2240**。删除状态输出中未建立新版依据的旧 preview render/map 内部字段读取。

UI engine 槽 `0x2729660`；cant-afford / cant-buy symbols `0x25BDDF4/0x25BDDF8`。弱 handle getter/resolver 仍为 UI engine slots 33/34，dispatch slot 47；从 native open/refresh、原生符号注册器、模型函数及 Panorama 的弱 handle 实现交叉核对。

### 未改动但重新检查的契约

- TeamHealth builder `0xEB3FB0`、presentation `0xEC74A0`：全部四处 pawn calls（builder+0x55；presentation+0xFB/+0x183/+0x1F4）仍到 `0xC7AAD0`；controller resolver、五处 builder visibility calls、三处 observer gates、health member hash 与 publish call `presentation+0x329` 均保持契约。health `+0x0C`、flags `+8`、state spectate byte `+0x17` 保持。
- TeamID context `0xEAE557`，relationship helper `0x8D80E0` 的四处 call 为 `0xEAE826/0xEAE839/0xEAEB9A/0xEAECA9`，仍满足原四处门控。VoiceBan `0x89FE77` 的两个相对调用和 blocked immediate `+7` 保持。
- DeathCam 入口 `0xD0BC70`；实际 deathTime/headshot 字段仍由 schema 读取，native 新字段读取亦存在。CSource2Client frame notification slot 36 为 `0xB6C320`；CGameEntitySystem add/remove slots 15/16 为 `0x9F6A00/0x9F7480`。这些共享入口没有因本次错误盲目整体加槽位。
- Panorama style setter 两个签名命中 `0x100F25/0x100F6F` 均调用 `0x193970`；height/visible property 布局保持，CPanelStyle slot 151 为 `0x1A1E10` 单属性清理，CUIPanel class slots 144/147/157/160 保持。height 清理继续只作用于 POV 所有权标记，名称仍归入 hud 开关。
- 动态 schema 路径保留：按类名/字段名解析，禁止把旧字段数值当新版本常量。SchemaSystem scope count/pointer `+0x190/+0x198`、scope declared collection `+0x470/+0x478` 在新版原生实现中仍存在。SchemaSystem 的 native 查询和 client 静态字段记录属于离线证据，不代表已经执行运行期初始化。

- EngineClient vtable `0x540AE8` 的 IsPlayingDemo / GetDemoFile slots 42/69 分别为 `0x75E80/0x76170`；后者返回 demo-player 接口，demo tick slot 3 `0x35620` 仍计算当前 tick 减起始 tick。SoundEventManager vtable `0x4AD738` 的 name→ID / valid / name slots 0/1/2 为 `0x4EFD0/0x4F2F0/0x4F2E0`；字段数量/value slots 48/52 为 `0x50220/0x50690`，保持声音圈现有参数契约。以上是调用 helper 的静态检查，未执行引擎接口。

### 静态证据、控制与边界

最终离线扫描覆盖 69 处 MirvPov 源码中的字节签名，均有命中；多命中位置另按实际搜索范围、相对 call 目标或 RTTI 消歧，不能把全模块多命中直接当作成功。SchemaSystem.cpp 的 49 个字段名均找到新版静态 descriptor 候选，保存在 schema-field-audit.json；候选不代替运行时按类限定的 schema 查询。

本次沿用所有既有 Release feature 开关及其清理/生效语义，没有新增用户效果或独立 playernames 开关。声音圈新增 getter detour 只扩展新版原生调用链覆盖，仍受 soundcircle 和 POV 总开关及返回地址约束；无线电入口仍受各自已有 radio_* feature 约束；buy-menu 仍受 build hash、schema 与 buymenu 开关约束。

证据目录 `diagnostics/pov-update-20260923/` 保存 DLL 副本、IDA 数据库、pattern-audit.json、函数反编译和 buymenu-mappings.json。签名存在不等于语义正确：本次确实发现了 TextMsg 误命中与多个通用 emitter 克隆，分别通过新函数内容和 RTTI 身份修正。相似度报告只用于辅助定位，不作为独立 ABI 证明。收尾时 client IDB 的额外保存请求超时，未确认最后一轮分析状态已全部落入 IDB；已导出的 JSON 证据保留。

**用户明确要求修改后不要测试。本次仅做离线字节、反汇编、反编译及源码核对；没有编译、没有运行单元/集成/游戏测试，没有启动或重启 CS2/HLAE，也没有替换运行 DLL、提交或推送。** 新 hook 组合、启停、seek、语音呈现和购买菜单运行效果仍未实测，不能把本节静态结果描述为运行通过。

### 同日后续：按用户要求编译并替换 DLL

用户随后明确要求替换 DLL。Release x64 `AfxHookSource2` 构建成功；编译时发现 HUD buy-zone 函数的签名初始化与 SEH 共处触发 MSVC C2712，已将签名初始化移入独立 helper，保留原判断及异常保护。未运行测试，未启动或重启游戏。

根据当前 HLAE 进程路径与 `hlaeconfig.xml` 的 `x64\AfxHookSource2.dll` 配置，确认 CS2 未运行后替换 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`。旧文件备份为同目录 `AfxHookSource2.dll.backup-20260923-194125`；新文件与构建产物 SHA-256 一致：`2f0231c620d46275deea04d977d878da6b5cf10a1ee69d80d9c367251f2e7453`。此为构建及文件部署核验，运行效果仍未验证。

## 2026-09-23：非默认探员投掷语音与玩家语音范围

### 身份、原生资源与确定的问题

本轮重算安装的 client.dll，仍为 `40bce8206f51b92ee05d6121c6e42c717bf3fa0cc0edeeb6698744b1c4799feb`；image base `0x180000000`。没有增加二进制 hook 或改动既有调用地址。资源来自当前游戏 VPK，经本地 Source2Viewer CLI 提取；目录 VPK 哈希和全体探员覆盖见 [探员语音资源表](cs2-agent-voice-catalog.md)。原生资源响应逻辑与 POV 自行补播区别如下：

- `items_game.txt` 的 `vo_prefix` 可以覆盖模型家族；`scripts/talker/shared.vrr` 的 model 条件再选择对应响应库。`ctm_st6` 对应 `seal`，`ctm_gsg9` 对应 `gsg9`。自定义库包括 `professional_epic`、`swat_fem`、`gendarmerie_male`、`seal_diver_01`、`jungle_fem` 等，不能通过短字符串包含判断折叠成默认声音。
- 投掷采用各家族的 `Radio.Smoke/Flashbang/FireInTheHole/Molotov/Decoy` 响应组，实际文件名可以是 `ct_smoke01`、`throwing_smoke_01`、`fm1_throwing_smoke_01` 等。编号不保证连续，原生组还带上下文条件，POV 没有完整重建其冷却和随机权重。
- 旧 `MirvPovRadio.cpp` 仅接受 `5000 <= id < 7000`，遗漏真实 `4613/4712/4751/4771` 等 ID；旧硬编码映射也遗漏 5105 以后多个可交易变体。找不到时 CT 被替代为 professional，而不是该探员真实声音。
- `FindVoiceFamilyInText` 旧包含匹配会把 `professional_fem` 归为 `professional`，并完全遗漏 seal/gendarmerie/jungle。旧 fallback 拼接固定编号，例如 `professional.t_smoke02`、`leet.t_flashbang02`、`fbihrt.ct_molotov01`，这些事件不在当前对应声音库；CT 诱饵也被错误拼成 `t_decoy01`。这些是静态确认的错声/漏播路径，但没有用户具体 demo/tick，不能将其宣称为每次报告现象的唯一运行原因。

### POV 修复与接入

`MirvPovAgentVoiceData.h` 用完整定义 ID 表替代区间猜测；涵盖 141 个定义，其中 63 个非 legacy 可交易探员。`ReadPawnCharacterDefIndex` 使用动态 schema 偏移，窄读 uint16，避免邻接字段影响；保持该字段缺失或未知时的降级路径。`FindVoiceFamilyInText` 改为完整事件前缀或路径组件匹配，保留 epic/fem/diver 等身份。`PickThrowVoiceStem` 从 28 个库各自原生响应组与实际声音事件的交集中选择，不拼不存在的连续编号；共 456 个事件及其声音文件完成静态存在性核对。未知探员且无已观察声音时，按当前默认 CT=SAS、T=Phoenix 降级。

真实 RawAudio 已观察 cue 仍优先，`weapon_fire/grenade_thrown` 仍先排队，等待原生音频后再补播；没有改变既有时间窗口、去重和声音空间化策略。既有 RawAudio typed formatter 为 client RVA `0x11A2830`（实际 detour）；直接声音 helper RVA `0x3EB040` 只被调用。此处地址沿用同哈希上一节静态记录，没有重新执行或声称实测通过。

沿用 `mirv_pov_radio_audio 0/1`（默认 1）；关闭立即清空补播队列，重新开启只影响后续事件；主 POV 开关和 radio 模块门控仍有效。没有新增独立用户效果。

### 玩家麦克风语音模式

`MirvCommands.cpp` / `MirvPovVoice.cpp/.h` 的 `mirv_pov_voice` 支持：

```text
mirv_pov_voice team   // 默认，只听当前 POV 队伍（包含本人）的玩家语音
mirv_pov_voice all    // 所有有效玩家，包括有语音数据的观察者
mirv_pov_voice enemy  // 只听对立游戏队伍；T/CT 之外不算敌方
mirv_pov_voice off    // 停止接管，恢复进入接管前的语音掩码
```

不带参数显示帮助和当前模式。保留 `true/1/on` 恢复上次所选模式、`false/0/off` 关闭；初始模式 team。模式对 `tv_listen_voice_indices` 的低/高 32 位与补充说话 HUD 使用同一筛选；team/enemy 在无有效 POV 队伍时写零掩码，避免残留旧队伍。切换模式清理 `servervoice_clear` 和说话 HUD，然后立即更新掩码；POV 关闭时只保存配置。模式在本进程跨 POV 开关和 demo 重置保留，不写用户配置。

保留手动把 `tv_listen_voice_indices` 从插件非零值改为 0 时关闭接管、清高位且不恢复旧掩码的语义。`mirv_pov_debug_feature voice 0/1` 沿用既有模块控制；不增加每个阵营模式的独立开关。此命令只控制玩家麦克风语音，探员投掷无线电仍属于 radio 音效，不受该模式筛选。

### 证据与未验证项

本地 `diagnostics/agent-voice-20260923/` 保存本轮 items_game、28 个事件库、talker 规则、全体映射、各资源 SHA-256、声音文件清单和生成审计脚本；维护用资源表在 `doc/notes/cs2-agent-voice-catalog.md`。按用户先前要求不运行测试，不启动或重启游戏。未验证每个探员的实机听感、真实 demo 音频包可用性、模式启停/seek/手动静音交互；资源覆盖不等于这些运行效果已验证。

Release x64 `AfxHookSource2` 编译成功（`diagnostics/agent-voice-20260923/build.log`）；此轮未替换 HLAE 当前 DLL，也未提交或推送。

## 2026-09-23：同步官方 HLAE 2.192.3

先将探员投掷语音与 team/all/enemy 修改提交为 `c2ad0f94`，再合并官方 `advancedfx/advancedfx` main 的 `88ce1ad2ab6732398cc275a3bd66d33208fd9f97`（HLAE 2.192.3）。引入 build 14182 的场景、颜色、相机、附件、比赛结束/天空盒适配及渲染回调顺序修复；contrib 子模块同步至 `bdecc6c056fe77afa87896e6cf554c59f9a80e89`。

冲突处理：实体 pawn/controller 158/159 与 eye 173/174 槽位双方一致，保留上游版本注释；Panorama 双方均将源码行号通配，保留 fork 更完整的后续指令上下文；POV 的队伍 uint8 读取、独立功能和语音模式保留。

**纠正本文件此前“GetClientClass 保持 slot 48”的记录**：同一 client SHA-256 `40bce8206f51b92ee05d6121c6e42c717bf3fa0cc0edeeb6698744b1c4799feb`、image base `0x180000000` 下，应按上游更新调用 slot **49**。本轮静态复核 pawn vtable RVA `0x1C82E78` 的 slot 49 为 `0xC7B300`，返回 record `0x2230E90`，record+0x10 指向 `C_CSPlayerPawn`；controller vtable `0x1C05330` 的 slot 49 为 `0x88A2C0`，返回 record `0x2216BB0`，+0x10 指向 `CCSPlayerController`。slot 48 的 `0xC79FF0/0x8897B0` 是依赖实例状态的另一条查询，不能继续当作已核验的直接类信息 getter。该接口仅被调用，未新增 detour；证据 `diagnostics/agent-voice-20260923/upstream-class-slot49.json`。

本次合并不启动游戏或运行测试，也不替换已部署 DLL；先前未提交的队友切枪声调查及诊断文件仍留在本地。

合并后的 Release x64 `AfxHookSource2` 编译成功（`diagnostics/agent-voice-20260923/build-upstream-merge.log`）。保留上游 CRLF 格式，差异检查使用命令级 `core.whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol`；清除上游新增的一处尾随 tab，不修改全局 Git 配置。运行兼容性仍未实测。


## 2026-09-24：受击蓝色染屏的 Fade 参数布局诊断

- 当前 client.dll SHA-256：`40bce8206f51b92ee05d6121c6e42c717bf3fa0cc0edeeb6698744b1c4799feb`；本机安装文件哈希已核对。旧版对照：`a0c195f0b6ec00915ef08c548200a010ebbe7982d3a4bc468cad939b67c8c4e3`。两者 image base 为 `0x180000000`。
- 原生行为：当前 Fade 消息回调 RVA `0xC1DD40` 将 duration/hold/flags 写至栈 `+0x20/+0x22/+0x24`，颜色写至 `+0x28`（VA `0x180C1DD68`），再将 `rsp+0x20` 作为参数传给 `CViewEffects` vtable slot 6（VA `0x180C1DDC5`）。颜色在紧凑参数内的偏移为 `+8`，有效范围至少 12 字节。旧版回调 RVA `0xBD1C60` 的 VA `0x180BD1C88` 写至 `rsp+0x26`，对应旧偏移 `+6`。回调定位使用源码现有 `48 83 EC 38 0F B7 41 48 66 89 44 24 ?? ... 8B 41 54 8B 0D ?? ?? ?? ??` 签名，各版本唯一匹配。
- POV 实现：`RenderSystemDX11Hooks.cpp:271` 的 `NativeFade_Apply` 仍声明 pack(1) 的 10 字节 FadePayload，颜色在 `+6`；它直接调用未挂钩的原生 AddFade helper（vtable slot 6）。另一个独立挂钩是原生消息回调，用来学习原生模板。消息字段 `+0x48/+0x4C/+0x50/+0x54` 此次未变，变化发生在传给 AddFade 的中间结构。
- 后果：当前原生接口按 `+8` 取色时会错读颜色最后两字节，并读取旧 10 字节结构末尾之外的两个字节。因此受击回退红色 `0x180000FF` 不能按预期显示，蓝色和透明度受相邻栈数据影响；死亡 Fade 也共用此路径。用户截图与这一确定的布局错误相符，但未运行游戏逐帧验证截图中的具体栈值。
- 本次为原因诊断：静态比较当前/旧版 DLL 指令，未运行游戏或测试，未改动实现或部署 DLL。修复应使颜色偏移为 8、结构大小为 12，并用编译期布局断言约束，不应仅交换红蓝通道。

### 同日修复：参数布局与额外受击染色分开处理

- `NativeFade_Apply` 采用显式零初始化的两字节 padding，使颜色位于 `+8`，总大小 12 字节；`sizeof` 和 `offsetof` 编译期断言固定当前 ABI。上述当前/旧版 DLL 身份和调用点证据继续适用，未重新引入旧偏移。
- 源码此前将当前 POV 的每个 `player_hurt` 转为全屏 Fade；没有已学习模板时使用 0.125 秒、alpha 24/255 的固定红色。仅修正布局仍会保留这一额外全屏红色效果。
- 已移除上述合成受击 Fade 的事件入口、队列、红色模板学习和固定参数；原生 Fade 消息仍由拦截器原样转发。受击方向提示及其他反馈不受本次修改影响。死亡渐黑继续使用独立的原生模板/既有调度和修正后的 AddFade 参数布局。
- 证据边界：现有资料未确认原生服务器对不同伤害类型发送 Fade 的完整条件，也未证明每个受伤事件都应发送红色 Fade。因此本次不宣称游戏绝无全屏红色效果，仅停止 POV 根据受伤事件自行制造它。没有新增用户可见效果，未增加调试开关。
- 验证：Release x64 AfxHookSource2 编译成功，源码差异检查通过；按用户要求未运行游戏或自动测试。本次尚未部署 DLL，实际画面待用户验证。

### 同日继续核对：使用原生伤害消息还原受击反馈

原生证据（此处补充证据，不把上一节的未知条件继续视为已验证）：

- 当前安装 server.dll SHA-256 为 `4f5c59c1153eb5f455f9131f80458bc2b9a6d1d7f30d170a2030d800685c00e8`，client.dll 仍为本节 `40bce820...`，image base 均为 `0x180000000`。本次对安装文件重新校验哈希，静态反汇编保存在 `diagnostics/agent-voice-20260923/native-hurt-evidence.json`；复现脚本对这两个完整哈希作检查后才读取版本专用 RVA。
- 服务端伤害效果函数 RVA `0xCE71B0` 同时出现在 `CBasePlayerPawn`/`CCSPlayerPawn` 主 vtable slot 357。从伤害信息 `+0x4C` 读位掩码：bit 0 为真时使用颜色 `0x80000080`；否则仅 bit 14 为真时使用 `0x80800000`。二者经 VA `0x180CE72E7` 调用 Fade wrapper RVA `0xFE4D10`，duration=1.0 秒、hold=0.1 秒、flags=1。bit 1 的分支位于 VA `0x180CE7369`，不调用上述 Fade wrapper。这里证明普通 bullet 分支与这两种带色 Fade 分支不同，不将结论外推为所有地图/脚本都不会发送 Fade。
- wrapper 将秒数转换为原生定点时长，颜色位于中间参数 `+8`，再调 sender RVA `0xFE4FF0`；sender VA `0x180FE50F0` 从 `+8` 读取颜色，构造 `CUserMessageFade_t` 后发送。它们是分析对象，POV 不调用 server.dll，也不挂钩这些发送函数。
- 服务端实际伤害消息 sender RVA `0xA8E430` 构造 `CCSUsrMsg_Damage_t`：伤害量来自 result `+0x20` 的整数化值，victim index 来自受害者，source 是 damage info `+0x38` handle 对应 inflictor 的虚函数 `+0x2C8` 返回位置。VA `0x180A8E519` 开始解析该 handle，VA `0x180A8E575` 读取来源位置。这不是把 `player_hurt.attacker` 的坐标直接当作来源；投掷者和爆炸位置尤其不能互换。
- 客户端 Damage 回调 RVA `0xE80390` 校验目标/amount 后，在 VA `0x180E8049B` 调用方向 helper RVA `0xE75B70`，本函数没有 Fade/AddFade 调用。helper 拒绝零来源/空 pawn，从真实来源与 pawn 位置计算方向，并对 HUD `+0x60/+0x64/+0x68/+0x6C` 的四方向强度取 max；近距离分支可设为 1。无需 POV 另写颜色、强度或动画时长，也无需用帧数/64 单位距离猜测合并哪些原生伤害消息。

POV 实现调整：

- `MirvPovFeedback.cpp::New_DamageMessage` 保留原生回调，再仅对远端当前 POV 且 victim index 匹配、amount>0、来源有限的真实消息补调上述未挂钩原生方向 helper。本地真实玩家交给原生回调；原生 helper 的 max 累积语义保留。Damage 回调签名仍为 `48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B FA 48 8B E9`，helper 签名仍为源码中的 `48 89 5C 24 08 48 89 6C 24 18 ... 48 8B FA 0F 57 FF`。
- 移除 `player_hurt` 按 attacker origin 推测方向的 fallback，以及仅为该 fallback 存在的 HUD 构造器 hook、缓存和帧/距离去重。这样既不为普通受击制造全屏红色，也不把 HE 等间接伤害指向投掷者。原生 `CUserMessageFade` 继续原样转发，保留游戏实际发送的特殊染色。
- 现有 `mirv_pov_debug_feature feedback 0|1` 继续控制 POV 的伤害消息补偿；主开关关闭或该功能关闭时只执行原生回调。本次没有新增效果或开关，也没有追加需要清理的状态。投掷语音、闪光/HE 失聪和死亡渐黑未改变。
- 边界：如果某个 demo 根本没有对应 Damage/Fade 消息，仅有 `player_hurt` 不足以恢复原生来源和伤害类型；此次不伪造缺失信息。尚未做运行时逐帧对比，因此不声称全部 demo 的画面已实测一致。
- 本轮验证：Release x64 AfxHookSource2 编译成功（`diagnostics/agent-voice-20260923/build-native-hurt.log`），CRLF-aware `git diff --check` 通过；未运行测试、未部署 DLL、未提交或推送。

## 2026-09-24：受击圆回归与死亡面板下落动画的最小 hook 调查

本轮用户要求检查原生实现并寻找最小 hook 方案；仅更新调查记录/分析产物，尚未实现以下方案，也未构建、部署或运行游戏。用户澄清横幅指死亡后显示凶手信息的 DeathPanel。

### 新构建身份

- 本机 client.dll 已再次变化：SHA-256 `12e4a7522678a582b085e404b7bfa32f9290f7fcd045716642daa61c43e7c96f`，image base `0x180000000`。已 dry-run 并 prepare-only 保存至 `diagnostics/hurt-death-native-20260924/client-analysis/12e4a7522678a582/`；未将该静态局部调查标为 IDA 全量分析完成。
- 当前游戏 VPK 中重新提取 `panorama/styles/hud/huddeathpanel.css` 和 `panorama/scripts/hud/huddeathpanel.js`，不是直接复用 9 月 21 日的资源。模块 SHA、VPK index SHA、资源 SHA、原生汇编及唯一签名记录在 `diagnostics/hurt-death-native-20260924/native-hud-evidence.json`。跨构建函数比较见 `build-mapping.json`。
- 下述函数与 `40bce820...` 对应函数的标准化汇编一致，但地址已重新确认：Damage callback `0xE80390 -> 0xE80930`；方向 helper `0xE75B70 -> 0xE76110`；HUD lookup `0xE7B7A0 -> 0xE7BD40`；Damage HUD constructor `0xE71350 -> 0xE718F0`；DeathPanel OnThink `0xE85D10 -> 0xE862C0`；local pawn getter `0x9698F0 -> 0x969900`。这不是对其他 POV 偏移或签名的全面兼容性背书。

### 受击圆

- 上轮删除 `player_hurt` fallback 后，`MirvPovFeedback.cpp::New_DamageMessage` 成了唯一方向提示补偿入口。用户现报告方向圆消失，说明仅依赖消息不能满足该 demo 的实际效果；没有运行时消息捕获，因此“缺少或未派发 Damage”仍是与源码相符的解释，不能声称已实测消息不存在。此前为避免猜测而直接移除 fallback 的处理过于激进。
- 当前原生 constructor RVA `0xE718F0` 以 `this+0x20`、名称 `CCSGO_HudDamageIndicator` 调用 HUD 基类 constructor `0xBA0C30`；该函数注册 HUD，注册函数为 `0xE75A30`。RTTI 主表/次表 RVA `0x1CCFD80` / `0x1CD0028`，次表 offset=32。可用原生 `FindHudElement("CCSGO_HudDamageIndicator")` 取得 HUD 子对象，确认类型后减 `0x20` 作为方向 helper 的 this，无须 detour 构造函数或跨地图缓存裸指针。
- lookup 唯一签名：`40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 48 85 C0 74 ?? 48 89 5C 24 38 48 8D 88 58 02 00 00 48 85 DB 74 ?? 4C 8B C1 48 8D 54 24 38`，当前匹配 RVA `0xE7BD40`。此函数是只调用的原生 helper，不需要挂钩。
- 最小方案：复用已有 GameEvents hook，在当前 POV 的 `player_hurt` 事件后补调原生方向 helper；真实 Damage 消息优先，并避免事件与消息重复补偿。方向强度、绘制、衰减仍由游戏完成，不能恢复固定全屏红色。
- 方向数据边界：普通枪击可以由攻击者实体补来源，但仍须注明与原生 inflictor 的取点/时序差异；HE 等间接伤害应使用已有爆炸事件/投射物信息或真实 Damage 来源，不应把投掷者位置当作爆炸位置。若 demo 不包含足够信息，无法承诺逐像素完全等价。

### 死亡面板从上方向中央移动

- 当前 CSS `.DeathPanel` 是水平居中、垂直顶部对齐；`.DeathPanel--FadeIn` 的 `DeathPanelFadeInAnim` 只有 0.3 秒 opacity 动画。只看 CSS 会漏掉用户描述的真实纵向运动。
- 位移在原生 OnThink RVA `0xE862C0`。DeathPanel 的 HUD 次表 offset=32（RVA `0x1CCDA78`）slot 5 经 thunk 进入该更新函数；`+0x1A1` 为主面板可见状态，`+0x198` 缓存纵向位置，`+0x60` 为子面板对象。这些为原生读写证据，不是新增写入方案。
- 动态位置分支在 VA `0x180E86306` 调用 local pawn getter `0x180969900`，返回地址 `0x180E8630B`；随后读取 pawn `+0x1458` 的死亡时间，结合该 pawn 的游戏时钟计算 elapsed。`death_panel_delay_time` / `death_panel_travel_time` 的注册默认值均为 0.25 秒（运行时仍尊重实际 ConVar 值），`cl_deathcampanel_position_dynamic` 默认 1。
- 普通动态分支纵向位置为 `H * (clamp((elapsed-delay)/travel,0,1)-0.5)`，位置为负时原生隐藏面板，到正值后从顶部进入、最终停在半屏高度。另有动态关闭时 `H/2`、replay 状态启用时 `H*spec_death_panel_replay_position` 的分支；因此不得靠强写位置或改全局 ConVar 抹平这些原生条件。
- 动画 getter 调用点唯一签名：`33 C9 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 00 48 8B CF FF 90 F0 04 00 00 84 C0 0F 84 ?? ?? ?? ?? 48 8B 57 10 48 8D 8C 24 98 00 00 00`，匹配 RVA `0xE86304`，返回地址为匹配点 `+7`。应检查 call 目标与已解析 getter 相同，避免把唯一字节匹配当作完整 ABI 验证。
- 现状：`DeathMsg.cpp::DeathPanel_GetLocalPawn` 仅在 `g_MirvPovDeathPanelLocalPawnOverride` 临时设置期间返回死者；guard 在死亡事件/Show 之后恢复。`MirvPovDeathPanel.cpp::MirvPovDeathPanel_Update` 只修复可见性，没有把死者上下文提供给引擎之后自然调用的 OnThink。因此每帧动画可能读取真实观战者的死亡时间，跳过或错过下落过程；这是明确的上下文覆盖缺口，实际哪一分支发生仍需运行时确认。
- 最小方案：不 hook OnThink，不自写 CSS 动画；扩展现有 local pawn getter hook，仅在上述返回地址且 POV death panel armed 时，解析并校验保存的死者 handle 后返回该 pawn。沿用现有 hook return-address 传递机制，防止多层 getter detour 丢失最外层调用点；其他调用点原样返回。切视角/seek/round reset 继续清理 armed 状态，原生 Show 只负责开始显示，避免每帧重启淡入。
- Hook 数量：上述方案需要新增 0 个 detour；受击圆新增 lookup/helper 调用，横幅扩展已有 getter 的一个明确调用点白名单。若实施时新增独立调试项，应使用 `mirv_pov_debug_feature` 注册受击圆补偿和面板位移两个控制，不应让一个控制影响另一个效果；本轮未新增控制或修改实现。

验证范围：只做源码、当前 VPK 资源和 PE 指令核验；未运行游戏、未测试、未替换 DLL。最小方案已经具备当前构建的具体接入位置，不能表述为已实施或已在 demo 中验证。

### 同日后续实施：恢复受击方向与 DeathPanel 原生位移

用户随后授权实施并替换 DLL。以下实施基于上节同一 `12e4a752...` client.dll、image base 和已重新定位的签名，不改变此前调查时尚未实施的事实。

- `MirvPovFeedback.cpp::QueueHurtDirection/RecordDirection/FlushDirections` 复用现有游戏事件入口，将普通直接攻击的 `player_hurt` 来源保存为攻击者当前 origin。帧渲染阶段后、当前 POV handle 仍相同时才补调原生方向 helper。每帧最多保留 64 项；按完整 pawn handle、伤害值和帧一对一匹配实际 Damage，支持两种到达顺序，不以同帧/相同方向粗略吞掉多次命中。消息照常即时补偿，已匹配事件不再补偿。跨帧事件/消息顺序及伤害值不一致的情况未经运行时验证。
- `FindHudElement("CCSGO_HudDamageIndicator")` 每次补偿重新查询注册名称，返回 HUD base 减 `0x20`；不缓存跨地图裸指针。FindHudElement 和 AddDamageDirection 均是只调用的原生 helper。保留已有 Damage callback hook，新增 detour 数为零。没有重新加入全屏红色 Fade。
- 事件 fallback 不把 HE、火焰、C4 或 world 的攻击者当作 inflictor；这些伤害依赖真实 Damage 来源。直接攻击的 origin 仍属于缺消息时的近似信息，不能保证与原生 inflictor 逐像素一致。
- `MirvPovDeathPanel_ResolveAddresses` 解析 OnThink 中的专用 call，并检查相对 call 目标确实为已解析 local pawn getter。`DeathMsg.cpp::DeathPanel_GetLocalPawn` 沿用返回地址传递，只对该调用点提供当前 armed 面板的死者；`MirvPovDeathPanel_GetAnimationPawn` 重新按 handle 查找并核对序列号、pawn 类型与观察目标。其他 getter 调用保留原行为，未增加 OnThink hook、CSS 动画或全局 ConVar 修改。
- 独立 Release 控制均默认开启、即时生效：`mirv_pov_debug_feature damage_direction 0|1` 控制 POV 方向消息补偿和事件 fallback；`mirv_pov_debug_feature deathpanel_slide 0|1` 控制动画专用死者替换。前者切换及 all 切换清空待补偿项；已交给游戏的方向强度按原生规律衰减，不强行清零游戏自身状态。后者关闭后立即恢复原 getter，重新开启使用仍有效的 armed 死者及原生时间轴，不重播死亡事件。主 POV 关闭、seek/关卡重置沿用既有清理流程；两项互不关闭彼此。
- 验证：Release x64 AfxHookSource2 构建通过，源码静态复核及 diff whitespace 检查通过。遵照用户“不要测试”，未启动游戏、未做 off/on/re-enable 或 demo 效果测试；实际呈现仍待用户观察。DLL 部署另以备份和源/目标 SHA-256 一致核对。

## 2026-09-24：非爆头死亡过早黑屏的原生阶段调查

用户已确认上一轮受击圆/死亡面板正常，随后报告非爆头死亡也立即进入黑屏。本节仅调查、记录，不修改功能代码、不构建、不替换 DLL、不运行游戏或测试。

### 当前二进制证据

- 安装 client.dll SHA-256 仍为 `12e4a7522678a582b085e404b7bfa32f9290f7fcd045716642daa61c43e7c96f`，image base `0x180000000`。本轮局部指令证据保存在 `diagnostics/hurt-death-native-20260924/death-phase-evidence.json` 和 `death-phase-current.asm.txt`。先前 `0xD0BC70` 重新映射为当前 `0xD0BC80`，两构建该函数标准化指令相同。最初检索工具用错更早版本作为 OLD 导致无候选，不能据此断言入口失效；明确使用 Sept23 `40bce820...` 为 OLD 后核对成功。
- 原生死亡后处理更新函数 RVA `0xD0BC80` 是已有 `MirvPovDeathCam.cpp::New_NativeDeathCam` 的 detour 目标；本轮未新增 hook。入口签名 `40 55 53 56 57 41 56 48 8B EC 48 81 EC ?? ?? ?? ?? 0F 29 74 24`，完整局部签名/唯一匹配见 JSON。
- 更新函数在 `0xD0BE03` 调用死亡 pawn 选择 helper `0xC79860`（未 hook 的原生 helper）。后者从 local pawn getter `0x969900` 开始，经虚调用和 observer mode==2/target 等门控选择候选，最终还检查生死条件；不是无条件使用真实 local pawn。仅临时改 local pawn 的两个字段并不等于已证明原生选择器一定返回该 pawn。
- 当前 schema 静态字段描述表 `0x22311A0` 将 `m_bKilledByHeadshot` 映射至 `+0x1EC9`，`0x21EE8A0` 将 `m_flDeathTime` 映射至 `+0x1458`。原生 `0xD0BE35` 读取死亡时间，结合实体时钟求 elapsed；`0xD0BE57` 直接判断 headshot 字节。这证明爆头分支确实存在，不能把额外 Fade 路径不读 headshot 误写成整个 POV 系统不读 headshot。
- `cl_instant_death_anim` 注册函数 `0xECC50` 默认 false；更新函数 `0xD0BE14` 读取其槽 `0x256F788` 后判断对象 `+0x58`。开启时有直接设第一阶段强度为 1 的特殊分支，绕过下述普通时间计算。
- 普通路径第一阶段候选强度为 `clamp(4*t, 0, 1)`，在 `0xD0BF0C` 通过 max helper `0x1B93E0` 与已有强度合并。低暴力模式分别选择对象 `+0x120` 或 `+0x124`。因此第一阶段从死亡时刻开始，约 0.25 秒达到 1；爆头并非简单“不计算第一阶段”。
- 第二阶段：爆头 `clamp(2*t, 0, 1)`；非爆头 `clamp(2*(t-(spec_freeze_time-0.5)), 0, 1)`。分支 RVA `0xD0BE7B`；非爆头读取 ConVar 的位置 `0xD0BEB9`，减 0.5 于 `0xD0BED8`；共同乘 2 / clamp 在 `0xD0BEE4` / `0xD0BEF9`，一般路径结果写入对象 `+0x128`（`0xD0BF5D`），另有模式/观战条件会走回落分支，不能把公式外推至所有模式。
- `spec_freeze_time` 注册函数 `0xACEE0`，名称 RVA `0x1C02408`，默认常量 RVA `0x1ABF07C` 为 3.0 秒。默认非爆头第二阶段在 2.5 秒开始、3.0 秒到 1；爆头从 0 秒开始、0.5 秒到 1。这里是游戏时间，非墙钟时间，运行时 ConVar 可改变非爆头间隔。
- 当前模块资源注册函数 `0xF0EC0` 包含 `lighting/postprocessing/effects/death_cam_phase1.vpost`、`death_cam_phase1_low_violence.vpost`、`death_cam_phase2.vpost`，字符串 RVA 分别 `0x1C9A0F0/0x1C9A130/0x1C9A178`。上述更新函数按强度提交后处理对象（`0xD0C0A1` 起），属于死亡后处理链，而不是向 CViewEffects 添加一条普通受伤红色 Fade。资源的具体色彩曲线本轮未解码，红/黑的视觉对应结合用户观察，不声称已逐帧或逐像素验证。

### 与现有 POV 的冲突及修复方向

- `MirvPovDeathCam.cpp:150-210,283-317` 已读取死亡事件 headshot，并在原生更新调用期间暂存/替换/恢复真实 local pawn 的 deathTime/headshot；这条链存在原生阶段复用意图。但原生 helper 的实体选择/生死门控仍需审查，不能仅凭 hook 安装成功认定阶段已正常执行。
- 另一路 `GameEvents.cpp::UpdateDeathFadeFromEvent` 无条件为 POV 死亡调用 `RenderSystemDX11_DeathFade_Death`。后者（当前源码 367-397 行）使用观测延迟，缺省只等一个 tick；不区分 headshot，也不读取 spec_freeze_time。渲染前 `RenderSystemDX11_DeathFade_ProcessPending`（550-562 行）调用 CViewEffects AddFade，fallback 是 duration=307/512 秒、hold=0、flags=FadeOut|StayOut、RGBA=0xFF000000。
- 确定的实现差异：额外黑 Fade 的调度独立于上述原生两阶段时间轴，会远早于默认非爆头第二阶段到期。它足以提前遮住原生第一阶段；这与用户表现相符。未经运行时捕获，不能将某一帧是否实际加载 fallback、原生 selector 返回哪个 pawn、真实后处理是否运行等推断写成实测结论。
- 正确方向是让原生死亡后处理控制两个阶段，先核实并补齐其 POV pawn 上下文，再取消/约束重复黑色覆盖；不应恢复“所有 player_hurt 均添加全屏红 Fade”的旧做法，也不应仅随意延迟固定红黑模板。本轮没有实施该修复，未更改任何已有开关。

### 后续实施：原生死亡阶段接管

用户随后要求修复。此变更继续使用上节 SHA-256 `12e4a752...`、base `0x180000000` 的当前局部核验结果；未把其他构建地址当成已验证。

- `GameEvents.cpp::HandleDeathFadeEvent` 不再因 POV 的 player_death 排队 `RenderSystemDX11_DeathFade_Death`，去除独立黑色 AddFade 对原生第一阶段的提前覆盖。原生 Fade 消息转发不受影响，亦未恢复普通 player_hurt 全屏红色。
- `MirvPovDeathCam.cpp::New_NativeDeathCam` 复用已有 `0xD0BC80` detour，在有效死亡事件、当前完整 pawn handle 一致、POV/death feedback/独立效果开关均开启时，暂存并设置实际 POV pawn 的死亡字段。原生调用结束后用 SEH `__finally` 恢复字段及线程局部上下文；不再把死亡字段写到真实观战者并假设 selector 会使用它。
- 原生 selector `0xC79860` 的 getter call 位于 `0xC79868`，返回地址 `0xC7986D`，目标经当前 PE 核对为 `0x969900`。通过现有 `DeathMsg.cpp::DeathPanel_GetLocalPawn` detour 和 return-address 传递，在 native death updater 的线程局部作用域内只对该返回地址提供 POV 死者。后续原生类型、生死门控、实体时钟、headshot 分支、ConVar/低暴力处理及渲染提交照常执行。新增 detour 数为零，selector 本身没有被 hook。
- 原先选取的短 selector 签名匹配七处，静态核验后已扩展至 observer mode==2 分支，使当前构建只匹配 `0xC79860`。初始化另检查 updater 内相对 call 确实指向该 selector，以及本轮清理涉及的三项权重字段指令存在；失败时不安装该 updater hook。最终签名与匹配记录在 `death-phase-implementation-patterns.json`。
- 原生 phase1 使用 max 累积，故新死亡、seek/重置、关闭效果及目标变更时需要清理上次 POV 的权重。只在收到当前原生 updater 的有效 This 时清理 `+0x120/+0x124/+0x128`，不解引用跨调用保存的对象地址；随后仍执行原生更新。状态 epoch 区分新的死亡/重置，避免上一段 phase2 覆盖下一段非爆头 phase1。
- 新独立 Release 控制：`mirv_pov_debug_feature death_screen 0|1`，默认 1，即时配置、下一次原生 updater 调用生效。关闭仅停用 POV 死亡后处理补偿并清理其权重，保留原生普通观战行为；不影响受击圆或死亡信息面板。重新开启时保留当前死亡事件，从原生死亡时间轴继续，不从零重播；主 POV/关卡/seek 重置沿用事件清理。实际红黑时序由原生函数及运行时 ConVar 决定，未硬编码 2.5/3 秒。
- 验证：最终 Release x64 AfxHookSource2 编译通过；当前 DLL 的入口、selector、权重布局与 call 关系已做静态核对。遵照用户“不要测试”，未运行游戏、demo 或开关 off/on 生命周期测试。此轮只完成源码与构建，未替换已安装 DLL；视觉效果及特殊观战模式仍需实际观察。

### 死亡 phase 偶发被清除：POV 生命周期修正

用户报告同一次死亡重复播放时，红/黑后处理偶发消失，而死亡横幅正常。本轮是 POV 源码生命周期排查，不新增原生二进制结论；沿用上述 `12e4a752...` 的原生函数与字段记录，未修改原生地址、偏移、签名或 hook 数量。

- 确认的代码路径：旧 `MirvPovDeathCam_HandleGameEvent` 对所有 `spec_target_updated` 和所有玩家的 `player_spawn` 无条件 Reset；旧 `UpdateDemoTick` 把相邻渲染采样的正向 tick 差大于 2 当作 seek；这些 Reset 都清除 active、死者 handle 与事件字段。DeathPanel 对目标更新通知不会同样无条件隐藏，故两种效果生命周期不一致。这些是足以解释症状的确定代码路径，未做事件捕获，不能指定用户某次播放究竟命中了哪一条。
- 修正：不再从上述通知事件直接重置；`MirvPovDeathCam_UpdateLifecycle` 在原生 FrameStageNotify 完成后检查已提交状态。保存的完整死者 handle 失效、有效选择目标变更、自动跟随时的有效 observer target 变更或 roaming 模式会终止；观察到死亡快照后重新出现正健康值才认定重生，避免死亡事件先到、实体健康值下一帧才更新时误清理。其他玩家重生不再影响当前死亡阶段。
- 原生 updater 使用完整 handle 重新解析死者，不缓存裸指针；帧更新中的临时 GetCurrentPovPlayerPawn=null 不再直接清除 phase weights。正式目标切换由后帧检查决定，阶段权重在随后原生 updater 中清理。地图/断开/主开关关闭仍保留原有 Reset 路径。
- 正向 tick 间隔不再作为死亡效果 seek 判据，因此低 FPS/快放的正常推进不会取消阶段。死亡事件同时记录 demo tick 并更新采样基线，避免新死亡事件被上一渲染帧的时间跳变再次清除。负向跳转到死亡之前时 Reset；回退但仍位于本次死亡之后时保留事件身份，只清理 max 累积权重让原生时间轴重新求值。前向跳转依赖已提交目标、实体身份和生死状态结束无效的死亡生命周期。
- 此为现有 `death_screen` 效果的生命周期修复，沿用其独立开关，没有添加新效果或控制。Release x64 构建与 diff whitespace 检查通过；输出 SHA-256 `DAC7FC9DE3FEFB8F5E159C4ACC959CAB2BF5F57B1CCE139A9105CE2D74C92433`。按用户要求未运行测试或游戏。本轮未安装 DLL，重复回放效果尚未实测。

后续部署与用户验证：用户授权安装后，以上 SHA-256 的 DLL 已替换至 HLAE x64 安装目录，旧 DLL 备份为 `AfxHookSource2.dll.backup-20260924-152356`，源/目标哈希一致。用户随后反馈效果正常并要求提交、推送及同步上游；这是用户回放确认，不是代理自行执行游戏测试，也不代表覆盖全部地图、模式及开关组合。

## 2026-09-24：同步上游 mirv_deathmsg 适配

同步官方 `advancedfx/advancedfx` main 的 `efbca37e655e0a6276ccacc9710dd53a3945842e`（`fix: adjust mirv_deathmsg to cs2 update`）。该提交仅更新旧 `getDeathMsgAddrs` 中内部 player_death 处理器的签名。

fork 已移除该函数及其 `g_Original_handlePlayerDeath` 全局入口，改由 `HookDeathMsg` 调用 `MirvPovDeathPanel_ResolveAddresses`，并 detour 外层事件监听器。因此此处是旧函数删除与上游修改的冲突，不是给现有监听器替换字节签名：保留 fork 删除和当前监听器调用链，合并上游祖先关系，不恢复旧内部 hook 或为不使用的函数添加扫描。最新 POV 修复已先独立提交为 `be5e5ded`。

合并结果 Release x64 `AfxHookSource2` 构建成功，未运行游戏或测试，未替换用户已验证的安装 DLL。此节只记录源码合并决策，没有新增原生地址验证；之前的按构建分析边界继续有效。

### 同日继续同步 HLAE 2.192.4

上游 main 更新至 `55ebb712`（HLAE 2.192.4），包含 `b69a67f3`、`d355c74f` 的渲染回调/准星修复回退，以及 `a0c61bed` 的新准星捕获处理。采用上游在 CSGOHud SetupLightsAndViewConstants 阶段排队 BeforeUi 回调、BeforeUi 捕获隐藏 CSGOCrosshair 的实现，合入版本、安装包及 changelog 更新。

唯一冲突在 `SceneSystem.cpp::new_InitDrawingData` 的旧 PostProcessing command-list 路径。按上游删除该跟踪和额外回调代码，将 commit hook 安装留在 scene filter 有效分支；五参数调用及可选 name suffix 转发仍保留。SceneSystem 与上游最终内容仅有一处尾随空白清理差异，POV 受击/死亡效果改动不被回退。此次没有新增二进制逆向或地址核验结论。

Release x64 AfxHookSource2 构建成功（`diagnostics/hurt-death-native-20260924/build-upstream-21924.log`），合并无未解决冲突，diff whitespace 检查通过。未运行游戏或测试、未替换安装 DLL；旧队友声音调查及本地分析产物继续不纳入本次提交。

## 2026-09-26：CS2 1.41.8.5 后续构建的 POV 购买菜单适配

**2026-09-27 提交整理时的最终状态：** 用户已实机确认购买菜单正常呼出、PROMOTED Knife 隐藏、菜单与人物间距及鼠标经过后的显示正常。当前实际采用样式按需初始化与既有 -116 x 布局修正；该偏移是经本次用户画面确认有效的兼容处理，不声称已还原所有分辨率下的原生 live 布局。后续投掷物置灰现象由用户反馈自行恢复，期间没有修改行为代码或安装新 DLL，不能计为本次修复成果，也未确定其原因。下面保留历次调查、撤回和验证记录，早期“待验证”描述仅对应当时阶段。独立开关 off/on/re-enable 的完整实机矩阵仍未执行，不因最终画面正常而补写为通过。

### 构建身份与证据边界

- 当前安装 `client.dll` SHA-256 为 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，image base `0x180000000`，image size `0x2998000`。不得把本节地址用于其他哈希。
- 按该完整哈希建立独立 IDA 数据库 `.codex/client-dll-analysis/9b4f46dbd6a43316/client.i64`；基线识别 106969 个函数、66396 个字符串。`idalib_open` 与 `idb_save` 的前台调用均在 300 秒超时，但 worker 最终完成分析，warmup/survey 成功，保存文件存在且 finalize 成功；不能把前台超时写成分析失败或把保存成功外推为语义验证。
- 迁移以此前已核验的 `40bce8206f51b92ee05d6121c6e42c717bf3fa0cc0edeeb6698744b1c4799feb` 为 OLD。`diagnostics/pov-update-20260926/map_buymenu_current.py` 对 PE unwind 函数边界、RIP 相对引用及归一化指令做定向比较。相似度只用于定位；关键入口另核对原始指令、RIP 目标、schema descriptor 和调用用途。

### 游戏原生行为与地址

原生 `CCSGO_BuyMenu` 仍按 open → refresh/full refresh → hover/think → model/select → close 顺序工作；local pawn/controller getter、loadout、购买资格、买入/卖出、模型预览和 buy-menu bit writer 的 ABI 与上一构建保持。原生 open-event creator 仍分配 0x20 字节事件对象、写入事件 symbol、通过 UI engine weak handle 解析面板；本构建只是被链接到远离其余 BuyMenu 函数的新位置。原生路径与 POV 接入分开如下；analysis VA = image base + RVA。

| 用途 | RVA | analysis VA | 接入 |
| --- | --- | --- | --- |
| open / close | `0xDB73D0 / 0xD9FFF0` | `0x180DB73D0 / 0x180D9FFF0` | detour |
| refresh / full refresh | `0xDBAEC0 / 0xDBAF60` | `0x180DBAEC0 / 0x180DBAF60` | detour |
| hover / think | `0xDC2460 / 0xDA8B70` | `0x180DC2460 / 0x180DA8B70` | detour |
| local pawn / controller | `0x96B2A0 / 0x96B260` | `0x18096B2A0 / 0x18096B260` | 仅在 BuyMenu scope detour |
| loadout / hover loadout | `0x904AF0 / 0x904A50` | `0x180904AF0 / 0x180904A50` | 仅在 scope detour |
| write buy-menu bit | `0xC94CD0` | `0x180C94CD0` | detour，POV active 时阻止写回 |
| purchase / sell | `0xDA9A80 / 0xDA9DC0` | `0x180DA9A80 / 0x180DA9DC0` | detour，POV active 时抑制 |
| model / select / SetPlayerModel | `0xDC0D10 / 0xDB6A80 / 0xE5C5A0` | `0x180DC0D10 / 0x180DB6A80 / 0x180E5C5A0` | detour |
| EventOpenBuyMenu creator | `0xDB1E10` | `0x180DB1E10` | 未 hook，仅调用 |
| symbol / item lookup / pawn model | `0x1781C90 / 0x1131320 / 0x21C150` | `0x181781C90 / 0x181131320 / 0x18021C150` | 未 hook，仅调用 |
| inventory manager / default loadout | `0x83A6C0 / 0x83D270` | `0x18083A6C0 / 0x18083D270` | 未 hook，仅调用 |
| acquire / owned weapon | `0x8C16F0 / 0x8FFA80` | `0x1808C16F0 / 0x1808FFA80` | 状态诊断 helper，未 hook |

当前 open-event creator `0x180DB1E10` 与旧 `0x180DAF500` 的 ABI 和逐指令结构一致；其 UI engine RIP load 指向 RVA `0x272E7E0`（analysis VA `0x18272E7E0`）。UI engine weak-handle getter/resolver/dispatch vtable slots仍为 33/34/47，panel visible getter slot 34，UI HasClass/SetClass slots 157/160。cant-afford / cant-buy symbol 经旧引用函数 `0xDAB1B0` → 新 `0xDADA60` 的相同指令索引映射为 RVA `0x25C2F74 / 0x25C2F78`。

schema 字符串 `m_bIsBuyMenuOpen` 位于 RVA `0x1C812F8`，descriptor RVA `0x2234D40` 明确记录字段偏移 `0x15EA`；运行时仍以 `SchemaSystem` 的类限定动态解析为准。controller inventory `+0x820`、pawn weapon/item services `+0x12F0/+0x12F8`、inventory `+0x88/+0x90` 与 56-byte records、panel/preview 和五组 88-byte 菜单记录等立即数均在对应原生函数的归一化指令中保持。购买资格函数仅 RIP jump-table 目标随布局变化，实际分支指令与立即数保持。

### POV 实现、控制与清理

`AfxHookSource2/MirvPovBuyMenu.cpp` 的完整 SHA-256 门控、image 上界、UI globals、全部 detour/helper RVA 更新至本节构建；未知 hash 或运行时 `m_bIsBuyMenuOpen != 0x15EA` 时仍拒绝安装。状态输出不再另读硬编码字段，改用动态 schema 偏移。POV 仍只在其线程局部 scope 替换被观察 pawn/controller/loadout/model，阻止真实购买、出售及网络 buy bit 写回；原生 helper 与未挂钩调用点不被描述成 hook。

继续沿用 `mirv_pov_debug_feature buymenu 0|1`（Release 可用、默认开启模块）及用户级 `mirv_pov_buymenu 0|1`（默认关闭）两层控制。关闭 BuyMenu 或主 POV 时同步执行原生 close、恢复 `hud-colorize-wash` class、清除 pending/handle/panel/target；本次没有新增用户可见效果，故不新增独立 feature 名称。不同效果的开关不互相关闭。

### 验证与剩余不确定性

本节第一轮静态验证覆盖完整 hash、schema offset、image size、24 个函数/helper、UI engine 与两个 class symbol，但仅按函数形状迁移 creator，未核对事件注册字符串和 symbol 身份；这使第一版错误地选择了同形的 `0x10933C0`。Release x64 `AfxHookSource2` 构建成功，`git diff --check` 通过；第一版输出 DLL SHA-256 为 `70D612D27CF35D3117B3DA03543BFBC666617C91367C8742410B2F633F8FD95B`。`mirv_pov 1` 后单独出现的 `undefined` 不来自 `MirvCommands.cpp` 的 POV 命令输出；仓库中的 JavaScript loader/evaluator 会把无返回值结果转成字符串打印，通常是同批 `mirv_script_load` / `mirv_script_exec` 的异步结果，对本次 BuyMenu build gate 无因果关系。

后续用户明确授权安装。确认 CS2 未运行、HLAE 启动器未加载目标 DLL后，将旧安装文件备份为 `AfxHookSource2.dll.backup-20260926-191636`，再替换 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`。安装文件与构建产物 SHA-256 均为 `70D612D27CF35D3117B3DA03543BFBC666617C91367C8742410B2F633F8FD95B`；备份哈希为 `8B23F98BCA1E4ACD7E7F765EF756DB48F4FF6BFA573D62F90FCCB3B8AE0D3E71`。未启动或重启 CS2/HLAE，运行验证范围仍不变。

### 首次用户运行反馈：尚未进入 open dispatch

用户随后自行启动 `inferno_test.dem` 验证。`console.log` 记录 `[mirv_pov_buymenu] native simulation hooks installed.`，证明完整 hash/schema 门控和 Detours transaction 已通过；`mirv_pov_buymenu enabled` 后两次状态均为 `hooks=1 active=0 wanted=0 pending=0 visible=0 opens=0`，没有出现 `opened for ...` 或 `closed`。因此本次失败发生在 open event dispatch 之前，不能据此把 `0x10933C0` event creator 或原生 open handler 判为失效。

第二次状态位于暂停的 game tick 17645：全体实体扫描仅发现 `player=7 name=我颠颠又刀刀 open=1 team=2`，其他玩家 open=0。该逐实体扫描不表示当前观察目标就是 player 7；BuyMenu 的实际 gate 另取 `GetCurrentPovPlayerPawn/Controller`，并同时要求 pawn/controller 有效、当前 pawn health>0、当前 pawn 的 open bit 非零。旧版已通过的对照日志明确在 `spec_player 2` 后为当前目标输出 `opened for 狗子doggest` 及 `active=1 wanted=1 visible=1`，暂停 demo 本身不阻止打开；切到 open=0 的 player 9 后则回到 active/wanted=0。

本次日志没有记录当前 observer target、当前 pawn health 或 target controller，因此只能将原因界定为“当前 POV 选择/gate 未满足”，不能断言 player 7 的 open bit 已被当前 POV 使用。最小复核是在同一 tick 明确执行 `spec_player 7`，等待 marker 后查询 `mirv_pov_buymenu_status`；若仍为 active=0，再增加只读状态输出记录 current pawn/controller/name/health/open 与 observer handle，而不是继续猜测并修改已静态核验的 native RVA。CS2 在日志采集后已退出；代理未启动、重启或控制游戏。

为消除全玩家扫描与当前 POV 目标之间的歧义，`mirv_pov_buymenu_status` 随后增加只读的 `current_target` 行：pawn/controller 地址、玩家名、health、team、`m_bIsBuyMenuOpen`、pawn/controller handle。该诊断不改变模拟条件或 UI 状态，用于下一轮直接区分“观察目标解析失败”“目标未存活”和“目标 buy-menu bit 为 0”。

该诊断版 Release x64 构建成功，DLL SHA-256 为 `5C79B755EC09B69A73F729B3E4A8B40F19015D67B381109A8F54C7E65FD07659`。用户明确授权安装后，确认 CS2 未运行，将上一版备份为 `AfxHookSource2.dll.backup-20260926-204148`（SHA-256 `70D612D27CF35D3117B3DA03543BFBC666617C91367C8742410B2F633F8FD95B`），再替换 HLAE x64 安装目录中的 DLL；安装文件与构建产物哈希一致。未启动或重启 CS2/HLAE，仍待用户运行 `mirv_pov_buymenu_status` 采集 `current_target`。

### `pending=1` 的 creator 身份纠正

用户运行诊断版后，状态为 `active=1 wanted=1 pending=1 visible=0 opens=0`；`current_target` 明确是存活的 player 7，health 100、team 2、`m_bIsBuyMenuOpen=1`。这证明 POV 目标和 gate 均正确，失败发生在事件提交到原生 open handler 之间。

重新从当前 DLL 的事件注册字符串核验身份后确认：`EventOpenBuyMenu` 字符串 VA `0x181CB22B8` 的唯一代码引用位于 `0x1800FE2D5`，注册函数 `0x1800FE2C0` 把 creator `0x180DB1E10` 和事件 symbol `word_18223D68C` 交给注册例程。`0x180DB1E10` 分配 0x20-byte `CUIEvent<0>`、写入该 symbol，并通过 `qword_18272E7E0` 的 vtable `+0x108` 解析 panel weak handle；其 ABI 与旧构建已验证的 `0x180DAF500` 一致。

第一轮错误选择的 `0x1810933C0` 则由 `0x180113840` 注册为 `PanoramaComponent_MyPersona_RecurringSubscriptionStatusChange`，使用不同的事件 symbol `word_18224C4C4`。它与真正 creator 具有相同分配大小、vtable 和 weak-handle 结构，因此单纯归一化函数体产生了错误匹配。UI dispatch 的 vtable `+0x178`（slot 47）另有当前构建原生调用点 `0x1801513BE`、`0x180151660` 支持，保持不变。POV 实现据此将未 hook helper 调用从 RVA `0x10933C0` 改为 `0xDB1E10`；仍需重新构建、安装并运行验证 open/close、可见性、loadout 和 preview。

修正 creator 后的 Release x64 构建成功，DLL SHA-256 为 `14D09D8C456B0CCF41D373B15BEC451CB935E709C0927BB1F1D46D7BED14CE5B`，`git diff --check` 通过。该产物尚未安装或运行；因此静态根因已确认，但实际菜单恢复仍待授权替换 DLL 后验证。

用户随后明确授权安装。确认 CS2 未运行后，将诊断版备份为 `AfxHookSource2.dll.backup-20260926-creator-fix`（SHA-256 `5C79B755EC09B69A73F729B3E4A8B40F19015D67B381109A8F54C7E65FD07659`），再替换 HLAE x64 安装目录中的 DLL；安装文件与修正版构建产物 SHA-256 均为 `14D09D8C456B0CCF41D373B15BEC451CB935E709C0927BB1F1D46D7BED14CE5B`。未启动或重启 CS2/HLAE，实机行为仍待用户验证。

### promotion 商品与菜单/人物间距

用户实机确认修正 creator 后 BuyMenu 可以正常打开，但 1920x1080 画面存在两个彼此独立的问题：左下多出竞技模式正常游玩不显示的 `PROMOTED Knife`；五列购物菜单整体偏右，其右缘与人物 preview 重合。当前 client 中 `CCSGO_BuyMenu` 构造函数 RVA `0xD98740` 加载 `file://{resources}/layout/buymenu.xml`；五列绑定函数 RVA `0xDA08E0` 查找 `CategoryContainer1..5`，preview/item 绑定函数 RVA `0xDA3860` 查找 `promo-item`、`BuyButton`、`loadout_pos` 和 `SellbackButton`。这些原生函数没有写入购买网格或 preview 的 x/y/width/transform，布局由 Panorama XML/CSS 决定。

promotion 修复独立处理：资源树确认 `CategoryContainerPromo -> promo-item` 是五个普通分类之外的容器，POV 实现通过 `MirvPanorama_FindChildInLayoutFile` 定位它，使用 Panorama `visibility` style property 在菜单打开/刷新期间将其 collapse，并在 close、reset、seek、目标切换或主 POV 关闭时恢复 visible，不删除节点、不修改真实购买逻辑。独立 Release 控制为 `mirv_pov_debug_feature buymenu_promo_hide 0|1`，默认 `1`，即时生效且只控制 promo；设为 `0` 恢复该商品。

布局修复来自独立的像素基线比较。旧 1280x720 正常图中购物菜单约为 x=168..803；按相同 16:9 比例缩放到 1920x1080，预期约为 x=252..1205。用户图中实际约为 x=426..1378，故菜单整体比基线向右偏约 174 个屏幕像素，即 1.5 倍 UI scale 下的 116 Panorama px。人物头部和身体锚点与旧图按 1.5 倍缩放后的坐标一致，因此人物不应移动。实现递归定位 `.buymenu-left`，仅对该购物列设置 `x: -116px`，不修改 `id-buymenu-agent` preview。

当前 `panorama.dll` SHA-256 为 `fc3cd563995130dd88a0c92b15deca26bd0fb5504b96483ab98bb243ca7f091d`，image base `0x180000000`。IDA 中 `panorama::CStylePropertyPosition` vtable 为 VA `0x1804D7DF8`；parse VA `0x18017F320`、clone VA `0x18017F2E0`。对象为 40 bytes：公共头 `+0x00..+0x0F`，x/y/z 三个 `{float value; uint32 unit}` 分别位于 `+0x10/+0x18/+0x20`；pixel unit 为 1。实现按 RTTI 动态查找 vtable、按 symbol resolver 获取 `x` ID，不把该 VA 硬编码到运行时。独立 Release 控制为 `mirv_pov_debug_feature buymenu_layout 0|1`，默认 `1`，即时生效；设为 `0` 将 `.buymenu-left` 的 x 恢复为 0。close、reset、seek、目标切换和主 POV 关闭也恢复为 0。该控制不影响 promotion、人物模型或其他 BuyMenu 样式。

仍需实机分别验证：`buymenu_promo_hide` 默认隐藏、off/on/re-enable；`buymenu_layout` 的默认间距、off/on/re-enable；两个开关互不影响；以及主 POV 关闭后的状态恢复。

Release x64 构建成功，`git diff --check` 通过，产物 SHA-256 为 `435AF2688FE40F865CE05307823A3984E0284D20736F79915541C3E22285D8F8`。依用户的持续部署偏好，确认 CS2 未运行后备份上一版为 `AfxHookSource2.dll.backup-20260926-213048`（SHA-256 `14D09D8C456B0CCF41D373B15BEC451CB935E709C0927BB1F1D46D7BED14CE5B`），并安装新 DLL；安装文件与构建产物哈希一致。未启动或重启 CS2/HLAE。

分离 promotion 与布局修复后的 Release x64 构建成功，`git diff --check` 通过，产物 SHA-256 为 `3022F237F9482DFB0C18C0C1F6C1F28364CB0D80DD75C1FA7B939D58F0A54F82`。确认 CS2 未运行后，将上一版备份为 `AfxHookSource2.dll.backup-20260926-2206`（SHA-256 `435AF2688FE40F865CE05307823A3984E0284D20736F79915541C3E22285D8F8`），并安装新 DLL；安装文件与构建产物哈希一致。未启动或重启 CS2/HLAE。

用户实机反馈该版 `PROMOTED Knife` 仍可见，且鼠标移入菜单后购买列会回到中间。复核原生基线：1280x720 下菜单外框约 `x=168..803`、五列约 `x=178..792`；CT/T 人物主体约 `x=831..1095`，脚底 `y≈658..659`。同比到 1920x1080，菜单应约 `x=252..1205`、五列约 `x=267..1188`，人物主体约 `x=1247..1643`。用户图中的人物锚点符合同比位置，菜单则整体右偏约 174 屏幕像素，所以 `-116` Panorama px 的菜单偏移判断不变，人物不移动。

实机现象定位到状态缓存而非偏移量：原生 hover/refresh/Think 会重算 Panorama 样式；此前 `g_LayoutShifted` 和 `g_PromoHidden` 只记录“曾经写入”，原生覆盖 x/visibility 后仍使 apply 函数提前返回。修正为每次 `Update` 都重新确认并写入 `.buymenu-left` 的 x 与 `CategoryContainerPromo` 的 visibility，并在原生 `Hover`、`Think` 返回后再次施加。两个 bool 只用于 close/reset 恢复，不再充当当前 Panorama 实际状态的缓存。独立 debug 开关、默认值、作用域和清理行为保持不变。

该修正的 Release x64 构建成功，`git diff --check` 通过，产物 SHA-256 为 `49C8F1436EFDC4ABE14F2422ABA04C9EE99B2814FCF14E6020EE171846AEDB3B`。确认 CS2 未运行后，将上一版备份为 `AfxHookSource2.dll.backup-20260926-2221`（SHA-256 `3022F237F9482DFB0C18C0C1F6C1F28364CB0D80DD75C1FA7B939D58F0A54F82`），并安装新 DLL；安装文件与构建产物哈希一致。未启动或重启 CS2/HLAE，hover 后布局稳定性及 promotion 隐藏仍待实机验证。

### 原生 live 路径复核：撤回未经验证的缓存根因和固定偏移结论

用户再次确认上述版本两个问题均存在，并明确本轮只做静态分析、不启动游戏。因此前文“定位到状态缓存”“偏移判断不变”不能视为已确认根因；旧截图不是经本轮验证的 pristine live 基线，174 屏幕像素到 116 UI px 的换算也没有当前 UI scale 证据。本轮不修改或部署 DLL，不宣称问题已修复。

当前分析 client.dll SHA-256 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，panorama.dll SHA-256 `fc3cd563995130dd88a0c92b15deca26bd0fb5504b96483ab98bb243ca7f091d`，两者 image base 均为 `0x180000000`。从本机安装的 pak01 VPK 重新抽取资源：`panorama/styles/buymenu.vcss_c` SHA-256 `d8a608e60e0024ecf821334e12d453b0f22fa463ad23ebe10d02057fbd4c4fc7`；`panorama/layout/buymenu.vxml_c` SHA-256 `6e31de1fa8acc8aaf871b59304cf3ccd9c9b76ca0eaad34d9104b40ffb5c1987`。提取脚本及资源位于 `diagnostics/buymenu-live-20260926/`；XML 的 LaCo 块尚未解码，不能声称已复核完整父子布局。

原生资源明确：`.buymenu__contents` 是 `flow-children:right; horizontal-align:center; transform:translateX(-50px)`；`.buymenu-left` 是向下流布局，`margin-left:16px; margin-top:180px`；购物信息栏宽 950px；`.buymenu-right` 使用 `flow-children:none; padding-top:38px; z-index:-1`；`.buymenu-agent` 为 `width:100%;height:100%;horizontal-align:left`。这些是相对父容器的 CSS 值，不是人物在屏幕上的绝对坐标。原生整体居中取决于参与布局的子面板尺寸，不能由左栏宽度单独求出屏幕 x。

Panorama 原生流布局 RVA `0x100460` 在 VA `0x180101615`/`0x18010165F` 构造 Position 属性并写入子面板位置。Position allocator RVA `0x174010` 和原生 SetPosition RVA `0x102580` 使用 canonical property ID `byte_1805E01C8`；parser RVA `0x17F320` 将它与单轴 alias 分开处理。当前 `MirvPanorama.cpp::makeXProperty` 用 `x` alias 作对象 ID，而布局引擎会重新计算流式子节点位置。因此当前 `MirvPovBuyMenu.cpp::ApplyLayoutOffset` 的反复写 x 方案存在静态实现问题，但尚未证明它独自解释全部悬停行为。上述 Panorama 例程只用于分析，本轮未新增 hook。

原生 promo：client RVA `0xDA3860` 绑定 `promo-item`，在 BuyMenu+576 存 weak handle，+640 存特殊槽位 -2；FullRefresh RVA `0xDBAF60` 在五个普通分类之后显式调用 item refresh RVA `0xDADA60` 处理该记录。item refresh 对 slot=-2 且 `qword_1825288E0+88` 的布尔值开启时走独立促销路径（getter RVA `0x88BD30` 当前返回 definition index 12），否则调用 loadout getter RVA `0x904AF0`。对空 item 或 item+488 无效，原生通过 UIPanel vtable+1280 将 `Hidden` 类打开；类名由 RVA `0x51740` 确认为大小写精确的 `Hidden`。所以应该追踪特殊槽位和原生 Hidden 状态，而不是将它等同于普通 loadout 中的 Knife；本轮尚未确认该布尔变量名称，也未证明用户可见 Knife 的实际 item 来源。

当前 visibility 属性的编码与原生一致：Panorama RVA `0xF2150` 使用有效标志 byte+16=1、visible byte+17=bool，经 setter RVA `0x194510` 写入 panel+104。故不能仅因隐藏无效便断定 visibility 数值格式错误；尚需确认现有 `FindChildInLayoutFile` 是否定位到实际 promo 面板、style 初始化及写入是否成功。client Think 内 RVA `0xDC1540` 是地面武器枚举与数量更新，不是悬停布局函数；现有名为 Hover 的 RVA `0xDC2460` 也不能仅凭命名认定为所有鼠标事件的入口。

验证边界：本轮静态确认 CSS、促销刷新分支、Hidden 类及 Panorama 布局写入路径；未进行 live/offline 对照、没有启动游戏、未完成运行时根因闭环。修复方案应先恢复正确的原生布局与 promo 状态，不能继续用截图推算的固定偏移冒充 live 行为。

### 同构建续查：XML 层级、真实 mouseover 与促销开关身份

本节沿用上节完整 client/panorama SHA-256、资源 SHA-256 和 image base，不跨构建移植地址。仅静态分析，未修改实现、未构建或安装 DLL，未启动游戏。精简伪代码保存于 `diagnostics/buymenu-live-20260926/0x*.c`。

使用本机 `diagnostics/weapon-sounds-20260922/tools/cli/Source2Viewer-CLI.exe`（20.0.0.0）对当前 `buymenu.vxml_c` 执行 `-i <file> -a`，成功从 LaCo 重建 XML。人物 `MapPlayerPreviewPanel#id-buymenu-agent` 和 `.buymenu__contents` 是 `.buymenu__fullscreen` 的同级子节点；人物不在左右栏的横向流中。人物资源指定 `map="ui/buy_menu" camera="cam_buymenu" playername="vanity_character" pin-fov="vertical" mouse_rotate="false"`，因此确切人物屏幕投影还涉及该场景/相机，不能直接从 agent 面板 left 对齐推算。

`.buymenu__contents` 的直接子节点只有 `.buymenu-left` 和 `.buymenu-right`。右栏含 purchase-failure container、`#ItemDesc.buymenu-item-info.Hidden` 和 `.vline`；右栏无显式宽高，ItemDesc 自身宽350、高110、margin-left32。左栏包括 info、body、ground-weapons；promo 位于 body 中，在普通 CategoryContainer1..5 的分类容器之外。这建立了布局父子关系，但当前共享 `csgostyles` 中 Hidden 的确切声明及实际布局尺寸仍需核对，不能把350+32直接当作实测右栏宽度。

构造函数 client RVA `0xD98740` 在 VA `0x180D98AC6` 查找 ItemDesc，`0x180D98AEA` 将 client panel 保存至 menu+440。真实 mouseover RVA `0xDA8DC0` 写 menu+664=slot，播放 `buymenu_mouseover`，调用 Select RVA `0xDB6A80`；成功后在 VA `0x180DA8E34` 通过 UIPanel vtable+1176 移除 ItemDesc 的 Hidden 类。对应 mouseout/清理 RVA `0xDAA110` 将 menu+664=-1，在 VA `0x180DAA2D5` 经 vtable+1152 加回 Hidden。现有 POV Hook 名为 Hover 的 RVA `0xDC2460` 并非此 mouseover；Select detour 的 Scope 也在原生 mouseover 操作 Hidden 之前退出。这确认了悬停会切换右栏子面板状态，但未证明其本身就是异常右移的全部原因。本轮没有 hook 这两个事件函数。

促销开关名称已确认：字符串 VA `0x181C072E8` 的引用指向注册函数 RVA `0xAE980`，它在 VA `0x1800AEA0E` 为地址 `0x1825288D8` 注册 `mp_promoted_item_enabled`，描述为允许购买 promoted item。ConVar 引用初始化 helper RVA `0x19A87D0` 明确写入对象+8，因此 item refresh 读取的 `qword_1825288E0` 正是该引用的数据指针，而不是另一个相邻变量。未读取运行时值，未据此断言该 demo 开关实际为1。

排除一条 Knife 猜测：当前 `ObservedLoadout`（`MirvPovBuyMenu.cpp:240`）以 unsigned slot 接收-2，无法匹配 uint16 网络slot；但默认 getter RVA `0x83D270` 在 VA `0x18083D279` 明确检查 `slot > 57 || team > 3` 并返回0，因此该回退不会把-2映射成 Knife。原生 Select RVA `0xDB6A80` 自身有-2专用路径，绕过loadout getter、使用 definition12；它与 refresh 的促销路径需分别看待，不能将普通loadout命中数当成promo来源证据。屏幕上的 Knife 实际item来源、隐藏写入为何无效仍未闭环。

### 2026-09-27：Hidden 不折叠占位；缺失促销定义的 Knife fallback

再次计算本机 client.dll SHA-256，仍为 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，image base `0x180000000`；沿用同构建数据库。本轮仅提取分析资源和更新记录，未改实现、未启动游戏、未构建/安装 DLL。新增可读资源位于 `diagnostics/buymenu-live-20260926/native/`。

**布局反证**：当前解码 `panorama/styles/buymenu.css:797-800` 明确为 `.Hidden { opacity: 0; }`，没有 collapse。`ItemDesc` 使用的就是该类；它属于购买菜单 XML 显式 include 的 buymenu.vcss，不需要从 csgostyles 或 HUD 推测。purchase-failure label 的特定 Hidden 规则另有 height:0，不能套用到 ItemDesc。因此撤回“鼠标切换 ItemDesc Hidden 释放右栏宽度”的候选解释；原生说明栏仅变透明，仍保留布局占位。前文真实 mouseover/mouseout 对 Hidden 的切换证据仍有效，但它不等于尺寸改变。目前更需要检查 POV 写 x 与原生 flow 重排的冲突，而非移动原生人物相机。尚无运行时位置/尺寸读回，不能声称已确定最终重排根因。

**促销定义链**：从当前 csgo VPK 提取 `scripts/items/items_game.txt`，SHA-256 `bf542a67a0e2fa41a57aabefbcb9e0cd84fab1cc4168d435ea95f4ea4c545bfc`。根 items 块从4725行开始；4813..4830行同级 item11 后接 item13，没有 item12。4727..4736行的 `default` 明确是 `item_class=weapon_knife`、`item_name=#SFUI_WPNHUD_Knife`、`hidden=1`。这里不是将其他 paint/sticker 块中的12当作物品，也不是把12等同普通 Knife index42/59。

原生 item 获取 helper RVA `0x1131320` 先查 cache，未命中走 RVA `0x11505E0` 创建。创建过程在 VA `0x18115069B` 调用初始化 RVA `0x118E850`，将所请求 definition（此处12）写到 item+442，在 VA `0x18118E92B` 调用 schema lookup RVA `0x112CFB0`，第三参数为0。该 lookup 缺失条目时于 VA `0x18112D033` 返回 schema+328 的 default 指针。schema items parser RVA `0x11058E0` 在 VA `0x18110597F` 与字符串 default 比较，并在 VA `0x181105A0F` 保存该对象至 schema+328，完成 default 身份核验。初始化对非空 fallback 仍在 VA `0x18118E963` 设置 item+488=1；所以 BuyMenu 的“无 item/invalid item 则 Hidden”检查不会仅因 definition12缺失而隐藏此 fallback。证据保存为 `item-init.c`、`schema-lookup.c`、`schema-default.c`。

由此可建立静态条件链：促销刷新分支启用 -> 请求缺失的12 -> schema默认Knife -> item validity=1 -> 不走无效商品Hidden分支。它能解释 PROMOTED Knife，并且与此前排除的普通loadout负槽位回退不同；但用户 demo 的实时开关值和实际命中分支没有采集，因此“该次画面一定走此分支”仍保留运行时验证边界。

本机 `game/csgo/cfg/gamemode_competitive.cfg:110` 明确 `mp_promoted_item_enabled 0`（文件 SHA-256 `0fbfe276a6ccb8fcde924dc46914fc0a19bbc78562f6dbc12e0ee9d95547d551`），其他 casual/deathmatch/rush/competitive_tmm 配置也为0。这是正常竞技模式的配置证据，不等同于已验证 demo 客户端会执行这些 cfg。现有强制隐藏 helper 为何未生效尚无根因证据；不能用 schema fallback 解释一个已经成功 collapse 的面板为何仍显示。后续应分别核验 promo 面板定位/写入/读回，以及模拟上下文中的促销状态，而非全局修改竞技配置。

### 2026-09-27：Panorama ABI 排除检查及静态结论边界

沿用上节 client identity 和 panorama SHA-256 `fc3cd563995130dd88a0c92b15deca26bd0fb5504b96483ab98bb243ca7f091d`，image base `0x180000000`。只做静态复核，未新增 hook 或改实现。

通过 CUIPanel RTTI COL VA `0x1804ED928` 定位 vtable VA `0x1804CB2A8`。vtable49 对应原生 FindChildInLayoutFile RVA `0xF8660`，使用 children count+40、array+48、child flags+284 bit0x40；GetID vtable10 / RVA `0x112760` 从 panel+16 取字符串（空时返回空串）。这些与 `DeathMsg.h:25-30`、`DeathMsg.cpp::findChildInLayoutFile` 的0x10/0x28/0x30/0x11C偏移及边界逻辑一致。因此没有静态依据认定旧偏移导致本次 promo 查找失败；实际实例是否命中仍必须读回。

visibility symbol 注册位于 panorama VA `0x180061270`，将明确字符串 visibility 和 `byte_1805E1BD5` 交给 RVA `0x16D560`。原生 SetVisible RVA `0xF2150` 使用相同 Visible 结构编码和 panel+104 的 style。当前 setter RVA `0x194510` 后接 `0x194800`，后者会更新样式并经 panel vtable+584 触发 invalidation；因此“我们漏掉一条必要的额外 invalidation 调用”也没有证据。

**细化 x alias 判断**：原生 VA `0x18005BFF0` 注册 x symbol，紧随的 RVA `0x5C010` 确实给 x 注册 Position factory，allocator RVA `0x174010`、clone RVA `0x174060`，factory中还保存 canonical position ID。因此 x 不是没有注册的无效属性；撤回把“findSymbol(x)”单独等同于确定错误的说法。当前手工 Position 对象仍与原生 allocator 的 canonical ID/未指定值初始化不同，且流程布局会计算子位置；但尚未证明该差异一定造成用户所见右移。不能仅凭前述差异继续改偏移并宣称修复。

当前 helper 的 bool 成功只表示构造属性并调用 setter，不是从实际 UI 读回可见性/布局；g_PromoHidden/g_LayoutShifted 同样不提供验证。剩余关键数据是：运行时 promo container与menu+576弱句柄解析结果、promo record+64的slot/+72 item与definition、mp_promoted_item_enabled实际值、visibility/opacity写入前后值、左右栏及ItemDesc在mouseover前后实际尺寸位置。静态分析不能替代这些值。本轮结束时不声称已确定强制隐藏失效或重排的最终根因；下一步应是受控诊断采样，保持不自动启动游戏的边界。

### 2026-09-27：经用户授权加入运行时采样，不改变现有修复策略

分析身份：client SHA-256 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，panorama SHA-256 `fc3cd563995130dd88a0c92b15deca26bd0fb5504b96483ab98bb243ca7f091d`，image base 均为 `0x180000000`。本轮再次核对安装 client 的完整哈希。原生行为证据沿用前述 promo refresh / mouseover 链，不新增任何 native hook。

`MirvPovBuyMenu.cpp::CaptureBuySnapshot/PrintBuySnapshots` 在普通 Release 的 `mirv_pov_buymenu_status` 输出诊断版本 `20260927-1`。仅保留最近 12 条值快照：原生 open 返回后样式前/后、已存在的 Select hook（client RVA `0xDB6A80`）调用前/后、menu+664 的 hover slot 改变、随后两个 Update 帧和 status 时刻。Select 返回时 ItemDesc 的 Hidden 类尚可能未被外层 mouseover 移除，因此必须与后续帧比较；此处不把 Select hook 当成真实 mouseover hook。新开菜单清空旧采样；关闭/Reset 保留最后快照，打印不解引用历史地址。无逐帧控制台刷屏，无新可见效果、无新 debug 开关，也不主动控制游戏生命周期。

快照分别记录 `mp_promoted_item_enabled`（client RVA `0x25288E0` 指针的 +88）、promo slot（menu+640）、item（menu+648）、item definition(+442)/valid(+488)，以及 root/left/right/promo container/promo item/ItemDesc/preview 的地址、父面板、vtable34 Visible 和 Hidden class。`promo_setter/layout_setter` 仅表示最近一次 Apply 构造并调用 setter 成功，明确不是读取到的隐藏/移动成功；feature 请求值单独输出。保留原来的 -116 x 和 visibility 写入方式，仅修正不当的“已验证原生锚点”代码注释。

Panorama 私有字段读回先检查该模块文件完整 SHA-256，并核对 CUIPanel vtable100/103/115/117 的 RVA `0x111A20/0x1119E0/0x1127B0/0x1118C0`，不匹配则 `readback=0`。这些未 hook 的 getter 的指令以及 vtable VA `0x1804CB2A8` 提供布局字段依据。输出保留原始偏移名：`native_1b0/1b8/1c0/1d0` 为四组 float2，`native_1f0` 为 float3。原生 RVA `0x102CF0/0x102EA0` 还会根据 position property 的 unit、父面板尺寸以及 +0x1C0/+0x1C4 组合结果，故原始字段不能冒充屏幕像素或最终变换后包围框。`visible/Hidden` 同样不能代替 opacity/computed visibility 的完整级联结果；此版尚未读取 effective opacity，不能凭 Visible=1 单独认定最终绘制可见。

验证：Release x64 AfxHookSource2 构建通过（SDK 既有 C4819 警告），`git diff --check` 通过。首次沙箱构建被 NuGet.Config 访问权限阻止，提升权限后成功。尚未启动游戏、未做实机采样，不宣称 Knife 或 hover 布局已修复。用户复现后执行一次 status 即可提供有界历史；若需原生布局对照，可由用户通过既有独立 feature 开关关闭 layout/promo hide 后另行采样。

安装：确认 CS2 未运行（保留已运行 HLAE，不操作进程），将旧安装备份为 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll.backup-20260927-diag-1103`，备份 SHA-256 `49C8F1436EFDC4ABE14F2422ABA04C9EE99B2814FCF14E6020EE171846AEDB3B`。新诊断 DLL 已复制到同目录 `AfxHookSource2.dll`，安装文件与 Release 构建产物 SHA-256 一致：`EB59DFD5912E9BB2AB2C0A0D06588C8B15116400D133CE9FED7FF7A4A52BADA9`。未启动、关闭或重启 CS2/HLAE，未提交或推送。

### 用户测试日志：2026-09-27 11:06

只读检查本机 `game/csgo/console.log`：第794行 build `2026-09-27T03:01:19Z`，1438行确认诊断版本 `20260927-1`、panorama_supported=1。1439–1534行为同一菜单保留的12条快照；用户在菜单关闭后执行 status，故1437行 active=0 不代表历史采样时没有运行。此次没有再读取二进制或产生新的 VA/RVA 结论，构建身份以该诊断版的既有完整 client 哈希门和 panorama 哈希门为依据。

已实测：全部快照 hide_requested=1、shift_requested=1，但 promo_setter=0、layout_setter=0；promo_container 与 left 指针均非空、readback=1。promoted_enabled=1，promo_slot=-2，definition=12，valid=1；promo container/item 持续 Visible=1。结合 `MirvPanorama.cpp::makeVisibleProperty/makeXProperty`，当前证据将失败定位至 setter 之前的属性构造/依赖初始化，而非“调用写入成功后被 hover 覆盖”。日志没有分别输出 vtable 和 symbol，不能从这些0单独断定是哪一个依赖缺失。

源码进一步找到初始化缺口：`DeathMsg.cpp::getPanoramaAddrs` 中唯一的 `MirvPanorama_InitStyleProperties` 调用被 `DeathMsg_ShouldProcessPanoramaPath()` gate 包围；该 gate 依赖当前 POV 或 deathmsg color 配置。`MirvPovCore.cpp` 默认 POV=false；HookPanorama 在已 hooked 时直接返回，没有后续按需补初始化，而 SetVisible/SetX 也没有 ensure-ready 路径。因此普通启动后才输入 `mirv_pov 1` 可以跳过全部样式属性初始化，符合两个 setter 同时失败。该控制流缺陷已由源码确认；本次没有读取进程内各全局值以排除部分初始化失败等其他分支。

布局的新增实测：sample6 hovered=-2（促销商品），sample7 Select 返回后 ItemDesc Visible 从1变0；sample8右栏 native_1b8 从382x818变为32x38，并持续到sample11。左栏记录仍950x816、native_1c0=(16,180)，preview仍为全屏面板。证据支持“促销项使详情面板退出布局，右栏宽度缩小，可能触发共同居中容器重排”的机制，而非人物面板自身宽度改变；共同父容器坐标/最终屏幕边界本次未采样，不能量化全局横移或宣布最终位置结论。此轮只更新调查记录，未修改行为、未编译或替换DLL、未控制游戏进程。

### 2026-09-27：投掷物全灰的新反馈（静态证据，待运行时返回码）

用户确认 Knife 隐藏和布局已正常，新增截图显示余额3700但五种投掷物全灰。最新 console.log 1525行确认11:17:00样式按需初始化成功，未找到本次 `slot=... acquire=...` 输出，不能由价格或截图直接断言购买判断错误。

重新校验安装 client SHA-256 为 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，复用 client-current 数据库，image base `0x180000000`。原生 item refresh（未hook的 helper，RVA `0xDADA60`）在 VA `0x180DAE187` 调用 CanAcquire（未hook的 helper，RVA `0x8C16F0`），参数为 pawn+0x12F8 的服务和 acquire method 1/2；VA `0x180DAE19C` 将余额够用与 acquire返回0进行逻辑与，VA `0x180DAE1FC` 依此设置 cant-buy class。因此有钱不保证原生菜单亮起。

CanAcquire 对 type=9 的 grenade 在 VA `0x1808C1996` 读取 client RVA `0x24B7150` 指针+88，false返回9。注册函数 RVA `0x85130`/VA `0x1800851BE` 确认该引用的名字为 `mp_buy_allow_grenades`（对象起点 RVA `0x24B7148`，reference data为+8）。其后还有同类携带上限返回4、总携带上限返回5、每回合购买数量限制返回3、净购买总量限制返回15。总携带上限从 RVA `0x24B0410` 指针+88读取，实体库存遍历服务为 pawn+0x12F0；每回合购买记录从 pawn+0x1590 的服务读取。不能绕过这些条件或仅按价格移除灰色 class。

POV实现未对此购买判断加hook，目前依然复用上述原生 helper；`MirvPovBuyMenu_PrintStatus` 已打印逐槽 definition/price/acquire/owned_weapon。下一步需要用户保持菜单打开并执行 status（关闭后该分支不输出），同时只读查询 `mp_buy_allow_grenades` 和 `ammo_grenade_limit_total`。本轮未改行为、未编译/替换DLL、未操作游戏。静态JSON证据保存在 `diagnostics/buymenu-grenades-9b4f46db-20260927.json`，未另开IDA或覆盖旧库。

### 2026-09-27：修复启动后开启 POV 时遗漏样式初始化

用户授权修复后，`MirvPanorama.cpp::CUIPanel::setOpacity/setVisible/setX` 统一先调用 EnsureStyleProperties。缺少 panorama 模块时返回 false 但不锁死重试；模块存在时调用已有符号/RTTI discovery，成功和失败均按 HMODULE 缓存，避免每帧扫描。不修改原生函数、签名或上述 client/panorama 构建记录，不新增 hook。所有依赖解析完成才置 ready；部分解析失败时 setter 不执行，避免空函数指针。原来的 ErrorBox 改为单次控制台警告，position vtable 缺失仍仅阻止 position 属性构造。明确使用 byte-pointer + panelStyle，清零 Visible/Opacity 对象 padding。`DeathMsg.cpp` 仅更新启动发现路径的注释。

没有添加新效果；既有 `buymenu_promo_hide`、`buymenu_layout` 独立控制、关闭恢复逻辑保持不变。本次不改变 -116 偏移或促销开关，先修复导致既有隐藏/位置 setter 根本未执行的缺陷。隐藏 promo 的预期作用还包括阻止鼠标进入特殊槽位 -2；是否消除实际重排仍待用户验证，未将该预期写成实测。诊断版本更新为 `20260927-2`，保留前后快照和 setter/readback 的区别。

验证：Release x64 构建通过，SDK 既有 C4819 警告；`git diff --check` 通过。源码复核了缺模块后可重试、同模块成功/失败缓存、三个 setter 均先检查 ready，以及失败时不调用 setter。未做进程内断言或游戏内 off/on/重启测试，不能将源码复核替代实机测试。确认 CS2 未运行后安装到 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`，构建与安装 SHA-256 均为 `8953D1CBD21864F11CC2D4140ACCFA1507C985B7A2331F16A313880D00060F35`；旧版备份后缀 `.backup-20260927-style-init-1115`，备份 SHA-256 `EB59DFD5912E9BB2AB2C0A0D06588C8B15116400D133CE9FED7FF7A4A52BADA9`。未启动、关闭或重启 CS2/HLAE，未提交或推送。

## 2026-09-27：MVP 横幅存在但音乐缺失的原生上下文补齐

### 当前构建与原生行为

重新以 synchronizer dry-run 核验安装 client.dll，SHA-256 为 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，image base `0x180000000`。原库打开失败，复制已有 `client.i64` 到 `diagnostics/mvp-9b4f-20260927/client-mvp.i64` 后继续局部分析，没有重新分析 DLL 或覆盖原库。以下 RVA 的 analysis VA 均为 `0x180000000 + RVA`。

- controller 的 MVP 计数通知 RVA `0x89F3F0` 构造 `round_mvp`，填写 userid、reason，以及存在时的 nomusic、musickitid、musickitmvps，经 GameEventManager vtable slot 8 分发。音乐盒由获奖玩家的事件字段决定，不能固定播放观看者的音乐盒。
- 原生事件监听器 RVA `0xCEADB0` 先验证 splitscreenplayer 与监听器 slot，再取得库存全局和本地普通 player pawn；VA `0x180CEAEA1` / `0x180CEAEAA` 的判空分支在任一缺失时跳过普通事件处理。getter 调用点 RVA `0xCEAE52` -> helper `0xC7C460` -> local pawn getter `0x96B2A0`。helper 检查 pawn vtable slot 158 / `+0x4F0`。
- `round_mvp` 分支 RVA `0xCEB4F1` 取得获奖 controller 和 musickitid（默认 `0xFFFF`）；普通路径接受 unsigned 16-bit ID `2..65534`，特殊游戏模式还可进入其替代路径。原生没有有效获奖 controller 时退出。POV 不伪造缺失 ID、不补造事件。
- `0xCEB5C0` 调用音乐许可 predicate `0xEC4AC0`，保留 `snd_mute_mvp_music_live_players`、事件 nomusic、本地 pawn 观察状态和特殊游戏模式条件。通过后在 `0xCEB5DA` 调用 `0x76FF20`，音乐类型为 11；不通过则走 `StopSoundEvents.StopAllMusic`。监听器同时按原生方式更新 pawn `+0x1520/+0x1524` 的音乐类型/时间，并提交 OnRoundMVPShown；这些原生状态写入没有由 POV 自行复制或改布局。
- 播放 helper `0x76FF20` 使用类型表中 `Music.MVPAnthem`，从经济 schema 查询 music kit 名，组成 `Music.MVPAnthem.<kit>` 后提交 SoundSystem slot 13 / `+0x68`。demo controller `0xD35C70()` 的 suppress byte `+0x72` 为真时提前退出。POV 保留该门槛，不改音量、音乐资源或 ConVar。
- WinPanel 的另一监听器 RVA `0xEAF550` 处理横幅，与上述依赖本地 pawn 的音频监听器分开。故横幅显示不证明音乐分支已运行。本次静态确认缺少 POV pawn 上下文的路径；尚未采集用户失败时真实 getter 返回值，不能将其写成该次回放的唯一已实测根因。

### POV 接入与独立控制

新增 `AfxHookSource2/MirvPovMvpMusic.cpp/.h`，不新增二进制 detour，也不直接调用播放 helper或重发 round_mvp。复用 `GameEvents.cpp::New_CGameEventManager_FireEventClientSide` 的原有 slot 8 hook，仅在 native 分发期间保存完整 POV pawn handle；嵌套非 MVP 事件临时清除上下文，`__finally` 恢复前一层。原生调用可能销毁事件，返回后不再读它。

复用 `DeathMsg.cpp::DeathPanel_GetLocalPawn` 对 `0x96B2A0` 的既有 detour，仅在 `round_mvp`、有效 demo、有效 T/CT POV、主开关与 `mvp_music` 均开启且完整 handle 一致时，对两个返回地址提供该 pawn：

| 返回地址 RVA | 原生调用 | 用途 |
| --- | --- | --- |
| `0xC7C46D` | `0xC7C468 -> 0x96B2A0` | 普通 player pawn helper；作用域限制在 MVP 同步分发期间 |
| `0xEC4B2D` | `0xEC4B28 -> 0x96B2A0` | 让原生静音许可按当前 POV 状态判断 |

helper `0xC7C460`、音乐监听器 `0xCEADB0`、许可 predicate `0xEC4AC0`、音乐播放 `0x76FF20` 均未新增 hook，仍由原生事件链调用。初始化验证三个唯一签名 `kPlayerGetter/kMutePredicateCall/kListenerCalls` 及共同 getter 目标/监听器调用关系；完整字节签名保存在源码和 `static-audit.json`。不匹配则禁用该修复并发出警告，不使用旧 RVA 强行安装。

`MirvPovBuyMenu.cpp::GetPawn` 的透传分支新增原始 return-address 传递，防止购买菜单与共享 getter 的多层 detour 丢失游戏调用点；`__finally` 保证异常时也恢复线程局部地址。购买菜单自己的 scope 行为保持不变。

Release 独立开关 `mirv_pov_debug_feature mvp_music 0|1` 默认 1、即时配置，只影响后续 MVP 分发。关闭不会改横幅、投掷语音或其他效果，也不强制停止已经交给原生系统的声音；已播放音乐自然结束。重新开启不重播旧事件；主 POV 关闭后补偿无效，重新启用保留 feature 配置。没有跨帧队列、历史 event 指针或缓存实体地址，dispatch 返回即恢复上下文，seek/换图不额外补播；原生音乐状态和 suppress 路径继续负责其生命周期。

### 验证与边界

- `verify_mvp.py` 对上述完整 DLL 哈希、三个签名的唯一匹配、相对 call 目标和实际 MVP 许可/播放调用点做离线断言，通过。
- 使用新增源码中的实际 PushEvent/PopEvent/GetLocalPawn 函数，配 mock engine/entity 编译运行 21 项断言，通过：两个许可 caller、其他 caller、嵌套非 MVP、关闭/恢复 feature、主开关关闭、无 demo、无目标、目标 handle 改变、旁观队伍、非法类型和未解析地址。此为控制流单元核验，不能冒充游戏音频或 Detours 实测。
- 独立代码复核发现购买菜单透传异常时可能遗留 caller，已补 `__finally`；最终非诊断 Release x64 构建成功，`git diff --check` 通过。最终 DLL SHA-256 `EB1DEAD79B1C3E2AABAD9D7125BB30DF57A5F6BAA8C4973290340D52DC47F06C`。
- 证据目录 `diagnostics/mvp-9b4f-20260927/`：`native-evidence.json`、`static-audit.json`、`context-test.log`、`build-final.log`。首轮构建因沙箱不可读取 NuGet.Config 失败，提升权限后的构建成功。
- 未启动、关闭或重启 CS2/HLAE；未执行实机 MVP 听感、不同音乐盒、seek、开关矩阵或 live/demo 音频对比。若 demo 没有有效音乐盒 ID、原生静音许可不通过或音量为零，本修复不会绕过这些条件。当前结果为可回放验证的实现，尚不能宣称与 live 完全一致。

部署：确认 CS2 未运行后安装至 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`，目标 SHA-256 与上述最终构建一致。旧文件备份为同目录 `AfxHookSource2.dll.backup-20260927-153806-mvp`，备份 SHA-256 为 `C2BE5C34BDEC8A59ED554D6E49BC11EA3D58CE20E792A5CDFA904BCE8EA3FB97`。证据 `installation.json`；未提交或推送。

### 用户首次回放无 MVP 音乐：增加有界运行诊断

用户明确反馈：连续播放到 MVP 时没有音乐修复效果，其他声音正常，并非跳转到横幅之后。首版不能据此标记为成功。当前 `console.log`（保存为诊断目录的 `user-first-test-console.log`）358 行为 `inferno_test.dem`，831 行为 POV enabled、build `2026-09-27T07:34:37Z`，与首版构建时间一致；没有 `native context validation failed`，但旧版不输出事件/命中数据，缺少 warning 不能证明 hook 或播放分支执行。检查时 CS2 已退出，未读取实际已加载模块或运行时变量，因此也不以安装文件哈希代替 loaded-module 核验。

独立源码复核确认初始化次序 `HookDeathMsg -> HookClientDll -> Hook_CGameEventManager`，未找到新的确定性接入缺陷。没有重新假设不同音乐盒或直接补播固定音乐，本轮保留现有行为，新增诊断版本 `20260927-2`：

- 普通 Release 每个 POV 下的 `round_mvp` 在原生分发返回后自动打印一条 `[mirv_pov_mvp_music]` 日志：tick、事件 musickitid/nomusic、上下文 gate、完整 pawn handle、共享 getter 总调用数、两个允许 caller 的查询/实际覆盖次数，以及只读的 master/MVP/其他模式 MVP 音量。音量读取失败记 -1；不修改 ConVar。kit 缺失记 -1，不将其当作确认无音乐盒。
- Trace 是栈内值快照，线程局部指针仅在该次同步分发期间有效；嵌套其他事件暂停采样，`__finally` 恢复，不在 native dispatch 后访问事件对象。日志中的覆盖次数只证明 POV 返回值替换，不能冒充 SoundSystem 已播放或用户听见。
- `mirv_pov_mvp_music_status` 只读输出解析 ready、feature active、POV 启用期间分发次数与 MVP 事件次数，用于区分根本没收到事件与后续分支阻断。ready 仅代表签名/调用关系解析成功，不代表 hook 安装或音频输出已验证。没有新增二进制 detour、用户效果或额外播放请求。
- 更新后的 21 项原有上下文断言、静态签名核验、非诊断 Release x64 编译及 `git diff --check` 通过。没有执行游戏或验证新增日志的实际输出；该版是定位用诊断版，尚非经确认的二次修复。

确认 CS2 未运行后备份并安装诊断版，构建/目标 SHA-256 `6E1E08E4CCA2EE5608026A1F061C73DE6D61157FF7F3F8F444D5FA17F15CE185`；旧版备份 `AfxHookSource2.dll.backup-20260927-155057-mvp-diag`，SHA-256 `EB1DEAD79B1C3E2AABAD9D7125BB30DF57A5F6BAA8C4973290340D52DC47F06C`。证据 `installation-diagnostic.json`、`build-diagnostic.log`、`context-test-diagnostic.log`。未操作 CS2/HLAE 生命周期，未提交/推送。下一步需用户连续复现一次并查询 status，之后读取自动日志确定真实阻断位置。

### 实测上下文未命中：补齐多层 getter 的外层返回地址

用户返回 `ready=1 active=1 dispatches=508 mvp_events=1`。读取当次 `console.log`，905 行记录：`tick=15815 kit=74 nomusic=0 gate=armed pawn=00530297 getter_calls=1 player_query=0 player_override=0 mute_query=0 mute_override=0 volume=0.500 mvp_volume=0.160`；其他模式 MVP 音量也为0.160。日志保存为 `diagnostics/mvp-9b4f-20260927/user-diagnostic2-console.log`。这实测证明事件及 POV 上下文已建立，实际共享 getter 被调用，但没有命中首版允许的两个 caller；不能再将事件未到达、音乐盒缺失或上述音量为0列为该次失败的解释。旧日志未记录具体 caller，仍不能仅凭这些计数直接断定运行地址。

本轮 dry-run 再次确认 client SHA-256 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`，image base `0x180000000`。原生音乐监听器 call RVA `0xCEAE52 -> 0xC7C460`，返回点 `0xCEAE57`，内层 call `0xC7C468 -> 0x96B2A0`，返回点 `0xC7C46D`，保持上节已核验关系。新增离线确认：TeamHealth builder RVA `0xEB6990` 的 call `0xEB69E5`，presentation RVA `0xEC9E80` 的 calls `0xEC9F7B/0xECA003/0xECA074`，全部也指向 `0xC7C460`。analysis VA 为 image base + RVA。

源码的确定缺陷：`MirvPovTeamHealth.cpp::New_GetLocalPlayerPawn` 在调用原始无参数 getter 前执行 `MirvPov_PushHookReturnAddress(_ReturnAddress())`；Core 的该函数只在上下文为空时记录地址，因此跨入内层 `0x96B2A0` 时保留的是音乐监听器外层 `0xCEAE57`，而非内层 `0xC7C46D`。SoundCircle 的同类无参数 getter 包装也使用这一约定。首版只接受内层地址，前一轮独立复核只核对 slot getter/BuyMenu 的传递，遗漏了这个外层共享 getter。原先21项测试同样没有包含真实 Core return-address 保留规则。

修正 `MirvPovMvpMusic.cpp`：在已有唯一 `kListenerCalls` 匹配和 call 目标校验通过后，保存 `listener+20` 为额外的允许 caller（RVA `0xCEAE57`）；同时保留无外层 hook 时的内层返回点和静音 predicate 点。仍只在当前 MVP 同步分发、有效 demo/POV 和独立开关开启时替换。没有放宽为全部 getter 调用，没有新增 hook，也不改变原生事件次数、音乐选择、音量或静音许可。日志版本为 `20260927-3`，额外记录首次调用点的 client RVA；不在 client 范围内则为0，避免把跨模块地址冒充 client RVA。

验证：静态扫描核验上述所有调用目标与外层返回点；C++ harness 加入实际 `MirvPovCore.cpp` 的 Push/Get/PopHookReturnAddress 函数，覆盖外层与内层嵌套及开关启停，28项通过。将 harness 中唯一 caller 判断恢复为旧逻辑时，新外层 caller 断言返回预期失败码23，复现旧漏判；没有通过游戏进程执行测试。证据 `static-audit.json`、`context-test-outer-caller.log`、`context-test-pre-fix.log`。Release x64 构建及 `git diff --check` 通过。

确认 CS2 已退出后备份并安装修正版，构建/安装 SHA-256 `521A150935A6577B3FC9E510B91745EBF8A5213D8D32707112587AA79C805622`；备份为 `AfxHookSource2.dll.backup-20260927-162408-mvp-outer`，其 SHA-256 `6E1E08E4CCA2EE5608026A1F061C73DE6D61157FF7F3F8F444D5FA17F15CE185`。证据 `installation-outer-caller.json`、`build-outer-caller.log`。未启动、关闭或重启游戏，未提交/推送。代码缺陷已复现并修复，但实际原生分支命中及最终听感仍待新日志和用户回放确认。

### 外层上下文已命中但仍无音乐：观测原生播放链

用户再次反馈无音乐。2026-09-27 16:26 的 console.log 显示 diagnostic `20260927-3`；两个事件分别在 tick 15906、15813，均为 kit=74、nomusic=0、gate=armed、first_caller_rva=0xCEAE57、player_query=1、player_override=1，master=0.500、MVP=0.160。确认上一处 caller 漏判已经修复，但它并非全部根因。mute_query=0 本身不能说明许可失败：nomusic != 1 时原生许可直接返回 true，不查询 pawn。

继续分析同一完整 SHA-256 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`、image base `0x180000000` 的既有 IDB 副本，以下 analysis VA 为 base + RVA：

- 音乐监听器调用 `0x11A54F0`，函数直接读取库存全局 slot `0x25EFCB0`；本地 pawn 覆盖后仍有此判空门槛，以及 MVP controller/kit/许可门槛。尚未实测用户该次库存是否为空。
- `0xD35C70` 返回静态 CCSDemoController 对象 `0x223B9C0`，音乐 helper 在 `0x76FF85` 检查其 `+0x72`（地址 RVA `0x223BA32`）。发现初始化方法 `0xD38500` 在 `0xD3850A` 清除此字节；读取参数值 `overwatch` 后在 `0xD3858D` 将对象 `+0x71` 的 word 写为257，连带置 `+0x72=1`。清理方法 `0xD38830` 在 `0xD38941` 清除此字节。因此它至少与 overwatch 模式有关，不能把它直接认作所有普通 demo 或 seek 的通用静音标志；并未穷尽所有写入来源。本次保留该条件，不沿用 Radio 注释的推断来强行清零。
- 播放 helper `0x76FF20` 的 type11 路径，valid kit 直接走 `0x77015C -> 0x118BB00 -> 0x112ED00` schema 查询，无需观看者音乐盒。`0x770226` 读取 SoundSystem global slot `0x2797770`；`0x770260` 经 vtable slot13 / +0x68 提交 `Music.MVPAnthem.<kit>`，参数为 system、输出对象、name、entity=-1、false。调用返回后仅清理字符串。提交不等于声音资源成功加载或音频设备已经输出。

诊断版本 `20260927-4` 在 `MirvPovMvpMusic.cpp` 增加只读观察 detour：许可函数 `0xEC4AC0`（原始返回 char 完整透传），音乐函数 `0x76FF20`（原参数透传），以及首次 MVP 音乐调用时从 SoundSystem 实例的 slot13 动态取得的提交函数。这里纠正前述历史版本“音乐 helper 未 hook”的状态：本版它被观察 hook 包装，但不主动调用/补播、不修改返回值、音量或 suppress。slot13 无固定代码地址，安装使用当前实例的虚表目标；暂未对声音模块实现作内部反编译。原生监听器 `0xCEADB0`、helper `0xC7C460` 仍未新增 detour。

初始化用唯一签名 `kMvpPlaybackCalls/kMusicEntry/kMusicDemoGate/kMusicSubmit` 验证 call 目标、音乐函数内部相对位置、库存/控制器 getter 的 RIP 相对指令及 RET，并检查许可函数+0x68与既有 muteCall 相同；完整签名在源码与 `static-audit.json`。失败时 playback_observers=0，不猜旧 RVA。日志按短行输出 inventory、controller、demo_suppress、eligibility_calls/eligible、music_calls/native_kit、sound_observer/sound_calls/sound；status 保存最近一次值快照，不保存 event/entity 指针，也不借此重放。sound_calls=0 只有结合 sound_observer=1 才能判断已挂接观察器未看到提交；初次未进入音乐函数时 sound_observer=0 属正常。

验证：当前 PE 上所有七个签名唯一匹配，call/RIP 全局目标核验通过；原28项上下文断言通过；生产观察函数的 mock native 回归验证原参数/返回值、非 MVP 路径透传、请求名记录、suppress 不修改、嵌套其他音乐类型和异常后的 TLS 恢复。Release x64 构建通过，只有已有 SDK 编码警告；这不是游戏音频验证。本轮不宣称修复最终无音乐原因，需实机日志区分原生前置门槛、音乐函数和声音提交三个阶段。

部署：确认 CS2 未运行后安装 diagnostic `20260927-4`，构建/目标 SHA-256 `3F76FEC1DAFBDE97020659E343D69B618D4B68D8A687AEB9B476D59F15AF93D4`。旧版备份 `AfxHookSource2.dll.backup-20260927-164223-mvp-playback`，SHA-256 `521A150935A6577B3FC9E510B91745EBF8A5213D8D32707112587AA79C805622`。证据 `user-diagnostic3-console.log`、`static-audit.json`、`observer-test.log`、`context-test-playback-observers.log`、`build-playback-observers-final.log`、`installation-playback-observers.json`。未启动、关闭或重启 CS2/HLAE，未提交/推送。

### 2026-09-28 实测请求已提交：发现遗漏的音乐总音量

用户继续确认听不到音乐。保存的 `diagnostics/mvp-9b4f-20260927/user-diagnostic4-20260928-console.log` 记录三次 MVP（tick 17875、15831、15813）均为 inventory/controller=1、demo_suppress=0、eligible=1、music_calls=1、sound_observer=1、sound_calls=1，实际请求名 `Music.MVPAnthem.knock2_02`、kit=74。最后一次在 Demo Skipping finished 后约12秒出现。由此确认本地 pawn、许可和声音提交路径已执行；不证明后端实际发声。

本轮重新计算安装 client.dll SHA-256，仍为 `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`；没有新增代码地址推断。当前 `csgo/pak01_dir.vpk` SHA-256 为 `24bf0b1ff2b16466a17eae3179bd8528c64885ab5cbb74966e6d0999440de1d4`（只标识目录包，不是所有分卷）。用本地 Source2Viewer-CLI 从该包重新提取音乐事件及 music stack 到诊断目录 assets，未改游戏资源。旧的2026-09-22提取文件只作定位，不作为本轮版本证据。

原生资源证据：`assets/soundevents/music/knock2_02/game_sounds_music.vsndevts:399` 定义实际 MVP 事件，type=csgo_music、volume=0.9、volume_convar=snd_mvp_volume、priority=5、loop_track/should_queue_track=true，声音资源为 `sounds/music/knock2_02/roundmvpanthem_01.vsnd`。当前 `assets/soundstacks/soundstacks_csgo_music.vsndstck:793` 的 music_total 算子把 system_globals.output_snd_musicvolume_convar 与分项 volume_convar 相乘，随后 convar_total 再乘 snd_gain；所以分项 MVP 音量非零不排除音乐总音量静音。

日志56行的 USRLOCAL 与实际测试账号的本地配置目录一致；该目录 `cfg/cs2_machine_convars.vcfg:244` 保存 `snd_musicvolume$2=0`，同时主音量0.5、MVP分项0.16，与日志相符。这里确认的是本次账号的持久化配置，前版诊断未读取运行时 snd_musicvolume，不能冒充该变量在事件时的直接采样；配置为0已足以解释请求提交却无声。此前诊断遗漏音乐总音量，不能再因 master/MVP分项非零而排除音量原因。

POV 行为：继续尊重原生音乐总音量，不自动将其改成1，也不绕过原生混音播放文件。本轮只记录证据，不修改用户配置、不更换 DLL、不控制游戏生命周期。下一次验证应在控制台先查询 `snd_musicvolume`，再按用户意愿执行 `snd_musicvolume 1` 并连续播放到 MVP；听感结果仍待用户确认，无需先换新的诊断 DLL。

### 2026-09-29：用户确认与提交整理

用户在上述音量排查后确认“这下完美了”，验证当前 demo 的回合结束 MVP 音乐已可正常听见。该结论结合前次 `player_override=1 / eligible=1 / music_calls=1 / sound_calls=1` 形成该场景的闭环，不扩大为所有音乐盒、模式或新版本的覆盖证明。

最终实现保留已验证的原生事件调用链、获奖者音乐盒选择、共享 getter 外层 caller 兼容、嵌套上下文恢复、独立 `mvp_music` 即时开关及原生静音条件。`mirv_pov_mvp_music_status` 和每次 MVP 的音量输出补齐 `snd_musicvolume`、`snd_gain`，音乐总音量为0时明确解释原因；仅查询，不改用户设置。音量行反映查询时的当前值，最近事件计数反映保存的事件快照。播放链版本标识仍为 `20260927-4`，本次整理仅扩充只读诊断。

提交前重新通过当前 client 签名/调用关系静态核验、28项上下文断言、观察函数透传/嵌套/异常恢复回归，以及 Release x64 构建和 `git diff --check`。用户实测针对此前部署的同一播放逻辑，新增音量提示经过构建验证，未再自动启动游戏。提交仅包含本次 MVP 源码与本节记录，之前未提交的队友武器声音调查、诊断目录、测试产物和本地分析库保留在工作区。

## 2026-10-03：CS2 更新后的闪光、玩家语音、购买菜单和拾取提示适配

### 构建身份和证据

安装 client.dll SHA-256：`d7db25d48f1d10c5e0b0296e20ed803426eb9509da41760daeda39dd35ba89b9`，PE image base `0x180000000`，image size `0x2998000`；下列 analysis VA 均为 base + RVA。本次记录不重新验证历史构建地址。独立输入、manifest、IDA 数据库及 finalize 状态位于 `diagnostics/client-dll-analysis/d7db25d48f1d10c5/`。IDA baseline 为 106937 functions / 66343 strings；open/save 前台调用各在300秒超时，worker随后完成分析，warmup/survey成功，目标 `client.i64` 已落盘且 finalize成功。只对下列局部函数进行语义分析，不表示所有函数均已手工核验。

离线 Capstone/PE 指令与 unwind 比较用于定位，IDA反编译和字符串注册引用用于身份核验。`diagnostics/pov-update-20261003/` 保存 `probe.py`、`mappings.json`、`native-evidence.json`、`verify.py`、`static-audit.json`、构建和安装证据。`native-evidence.json` 中 `0xDB1550` 的反编译是被排除的初始候选，不是真正的 BuyMenu creator；最终身份以注册函数和静态断言为准。

### 游戏原生行为

- 闪光两个渲染路径仍调用同一 bool predicate：compact call `0x11C856B`、return `0x11C8570`；per-view call `0x11DD3E5`、return `0x11DD3EA`；target `0xCE1EB0`（VA `0x180CE1EB0`）。per-view 后续 LEA 从旧结构成员 `+0x368` 变为 `+0x1398`，因此旧完整签名失效。该字段并非 POV 直接读取的状态。原生 predicate 保留 controller/pawn、观察状态和原生条件判断，POV只对这两个 caller 改返回结果。
- ServerVoiceData 入口 `0xB46740`（VA `0x180B46740`）重新分配寄存器/栈，并新增采样率白名单（8000/16000/24000/44100/48000/96000）。仍从 message+`0x40` 的 has-bits 先检查 `0x100`，取 entity index+`0x6C`；否则检查 `0x80` 并取 legacy slot+`0x68`，转换为一基 entity index后校验，再处理语音包。POV继续完整透传原始消息、返回值和原生解码/播放条件，不绕过采样率校验。
- 原生 BuyMenu open → refresh/full refresh → think/select/model → close 的 ABI及主要菜单字段保持。schema descriptor `0x2234960`、字符串 `0x1C813B8` 明确给出 `m_bIsBuyMenuOpen=0x15EA`；inventory/weapon/item services descriptors `0x221B460/0x21F2030/0x21F2050` 分别为 `0x820/0x12F0/0x12F8`。旧 RVA、globals 和 class symbols 已迁移，完整哈希保护仍必要。
- `EventOpenBuyMenu` 字符串 VA `0x181CB0D40` 唯一代码引用在 `0x1800FE495`，注册函数 RVA `0xFE480` 注册 symbol `0x223D624`、creator `0xDB0100` 和 callback `0xD9FE20`。creator分配32 bytes、写同一symbol并使用UI engine weak-handle getter slot33。与旧creator同形的 `0xDB1550` 使用不同symbol `0x223D750`，不能靠归一化形状选用。
- model `0xDBE380` 仍以 `(menu,item)` 调用、使用 menu+`0x208` preview，在 `0xDBE39E` 调 local pawn getter。新增本地库存槽41（立即数`0x29`）的有效item检查与preview vtable+`0x360`提交，处于模型/武器装备之后、原有槽57路径之前；本次保留该原生路径，不假定其所有 cosmetic 显示已符合观察目标。pawn model getter ABI及 SetPlayerModel ABI保持，现有模型替换仍作用于同一preview。
- 拾取提示 active builder 入口 `0xEB0180`；caller函数 `0xEAB6E0` 的 call `0xEAB78A -> 0xEB0180`，return `0xEAB78F`，caller以RCX=HUD、RDX=hint buffer传入。寄存器和栈布局改变，双参数 char返回ABI保持。

### BuyMenu 当前 RVA 表

| 用途 | 当前 RVA | 接入方式 |
| --- | --- | --- |
| open / close | `0xDB54D0 / 0xD9E740` | detour |
| refresh / full refresh | `0xDB8820 / 0xDB88C0` | detour |
| hover / think | `0xDBFB70 / 0xDA6A90` | detour；hover名称不等同真实mouseover |
| local pawn / controller | `0x96AAD0 / 0x96AA90` | scope内detour |
| loadout / hover loadout | `0x904330 / 0x904290` | scope内detour |
| buy bit / purchase / sell | `0xC94420 / 0xDA79A0 / 0xDA7CE0` | detour |
| model / select / SetPlayerModel | `0xDBE380 / 0xDB4D70 / 0xE5C3B0` | detour |
| EventOpenBuyMenu creator | `0xDB0100` | 未hook，仅调用 |
| symbol / item lookup / pawn model | `0x1782790 / 0x1130C10 / 0x21C620` | 未hook，仅调用 |
| inventory manager / default loadout | `0x839F00 / 0x83CAB0` | 未hook，仅调用；manager为8-byte leaf |
| acquire / owned weapon | `0x8C0F30 / 0x8FF2C0` | 未hook，状态诊断helper |
| UI engine / cant-afford / cant-buy / promoted ref | `0x272EE40 / 0x25C3704 / 0x25C3708 / 0x2528E20` | 原生globals；RIP引用核验 |

### POV 实现和验证边界

`MirvPovHud.cpp::MirvPovHud_ResolveFlashContexts` 将未被POV读取的per-view成员位移设为通配，并增加后续MOVUPS指令限定；新旧三个保存的client构建均唯一匹配，运行时仍要求两个E8指向同一predicate。`MirvPovVoice.cpp::MirvPov_ResolveVoiceHud` 更新ServerVoiceData入口签名（完整字节见源码与static-audit）。`MirvPovBuyMenu.cpp` 更新完整hash及上表全部入口/global，保留schema `0x15EA`检查和未知构建拒绝安装；库存/购买隔离、状态清理和共享getter caller传递保持。`MirvPovPickupPrompt.cpp::MirvPovPickupPrompt_Initialize` 更新active builder/caller唯一签名，原有E8目标一致性检查和相对call thunk保持。

无新增用户可见效果。沿用既有Release独立开关 `voice`、`pickupprompt`、`buymenu`、`buymenu_promo_hide`、`buymenu_layout`（`mirv_pov_debug_feature <name> 0|1`），以及默认关闭的 `mirv_pov_buymenu 0|1`；本次没有修改其默认值、关闭恢复、seek/换图/目标切换清理和主POV开关关系，也没有把闪光开关从原有hud作用域拆出。

专项静态断言通过：完整hash/schema、20组归一化一致函数、8-byte leaf、creator注册身份、语音字段、闪光共同target；82条POV源码签名在当前安装模块扫描均有匹配（该总扫描不等同每个模块所有语义均完成验证）。拾取active caller唯一匹配且rel32指向新builder。item refresh原生指令语义保持、CanAcquire只有jump-table布局迁移；model新增slot41原生逻辑保留，仍需观察实际preview。Release x64构建成功，`git diff --check`通过。首轮构建因进程环境同时含PATH/Path而失败；仅在构建子进程环境去重后成功，没有更改系统环境。

确认CS2未运行、HLAE启动器未加载目标DLL后，备份并安装到 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`；备份后缀 `.backup-20261003-152249-client-update`，旧hash `3F76FEC1DAFBDE97020659E343D69B618D4B68D8A687AEB9B476D59F15AF93D4`，新构建/安装hash均为 `E8B722FADCC3373DDC4150C959F08125B967F2A10FE0DE16BD7A8B59C040DF3C`。未启动、关闭或重启CS2/HLAE，未提交/推送。尚未完成游戏内闪光/玩家语音、BuyMenu开关矩阵、preview或拾取提示回放验证；安装hash一致不能替代实际加载模块和游戏行为证据。

## 2026-10-09：CS2 新构建的 POV DLL 同步

### 构建身份与证据

重新发现并复制本机 Steam 安装文件，未以历史地址代替新版验证。以下模块 image base 均为 `0x180000000`；本节地址为 RVA，analysis VA = base + RVA。

| 模块 | SHA-256 |
| --- | --- |
| client.dll | `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008` |
| panorama.dll | `4b3eb751f78bcbea82b146dcc5e2668d8fa19263c82dd35321441feb2be43c24` |
| engine2.dll | `4b8fe29ecdef4f492a602d9013cbfe5226561c9c0ba4361ce82e6f46d4f9bc04` |
| schemasystem.dll | `f362c13aada09ee03c402e3a0de2b66bdebfe57bbb15d23c437971b21f32b6e8` |
| soundsystem.dll | `4517ee246256f9720ad2a2c0c5d0d7b231c3207e8644753efefef363e7105155` |

client image size `0x2994000`；独立输入、manifest 和基线报告在 `diagnostics/client-dll-analysis/44af2aa0d2c64f90/`。IDA baseline 识别 106837 functions / 66311 strings，warmup 确认 auto-analysis 和 Hex-Rays ready。open 前台请求300秒超时，但 worker 已完成，后续复用同一 session；没有重新启动重复分析。定向反编译 EventOpenBuyMenu 注册器/creator、default loadout 和 model；其余通过 PE/unwind/Capstone 及独立只读核验。此范围不代表全模块手工语义分析。

### 游戏原生逻辑与迁移

原生 BuyMenu open → refresh/full refresh → think/select/model → close 顺序保持，menu 字段、preview 2284/2288/2296 与160-byte records、控制器 inventory `0x820`、pawn services `0x12F0/0x12F8` 和 buy bit `0x15EA` 均有新版依据。schema descriptors 分别为 `0x2217840/0x21EC1F0/0x21EC210/0x2230FD0`；运行期继续按类限定查询。

原生库存管理器的本地库存成员从 `+0x52900` 变为 `+0x528D0`，loadout/hover/model 指令除该成员及重定位外保持。default loadout helper 仍校验 slot<=57、team<=3，58 slots、1456-byte stride，但对象中的表起点从 `+0x180` 变为 `+0x150`。POV 调用新版 helper，不自行索引这些管理器字段。稀疏服务器56-byte loadout records 的 `+48/+50/+52` 及 inventory `+0x88/+0x90` 保持。

`EventOpenBuyMenu` 注册器 `0xFE450` 引用字符串 `0x1CAF400`、symbol `0x223962C`、creator **`0xDAFBF0`**。creator 分配32 bytes、写同一symbol、从 UI engine global `0x272ABC0` 调用 slot33 取得 weak handle。creator 未 hook，仅由 POV Dispatch 调用；注册器也未 hook。身份由注册字符串与symbol交叉核验，不用同形函数直接迁移。

| 用途 | 新 RVA | 接入 |
| --- | --- | --- |
| open / close | `0xDB4FC0 / 0xD9E230` | detour |
| refresh / full refresh | `0xDB8310 / 0xDB83B0` | detour |
| hover / think | `0xDBF660 / 0xDA6580` | detour；hover名称不代表真实mouseover |
| local pawn / controller | `0x969A30 / 0x9699F0` | scope内detour |
| loadout / hover loadout | `0x903280 / 0x9031E0` | scope内detour |
| buy bit / purchase / sell | `0xC94090 / 0xDA7490 / 0xDA77D0` | detour |
| model / select / SetPlayerModel | `0xDBDE70 / 0xDB4860 / 0xE5BEA0` | detour |
| symbol / item lookup / pawn model | `0x177FAA0 / 0x1130860 / 0x21C550` | helper，仅调用 |
| inventory manager / default loadout | `0x838DD0 / 0x83B980` | helper，仅调用 |
| acquire / owned weapon | `0x8BFE20 / 0x8FE210` | 状态诊断helper，仅调用 |
| UI engine / cant-afford / cant-buy / promoted ref | `0x272ABC0 / 0x25BF4D4 / 0x25BF4D8 / 0x2524BB0` | 原生globals，经RIP引用核验 |

`MirvPovBuyMenu.cpp:21` 更新完整hash、image范围、上述detour/helper/globals；未知版本仍拒绝安装，不放宽门禁。Panorama诊断更新完整hash及 CUIPanel slots100/103/115/117 的 getters 为 `0x111AD0/0x111A90/0x112860/0x111970`，四者指令/布局与旧版一致，实际vtable RVA `0x4CCFF8`。这些仅是诊断读回依据，不 hook；style/body/default布局修正策略没有改变。

### 其他 POV 关键契约

- 闪光 compact/per-view calls `0x11C3DDB/0x11DA825` 均到 predicate `0xCE19A0`；玩家语音入口 `0xB45A80` 仍读 has-bits `+0x40` 与entity `+0x6C`，内部slot数组地址迁移由原生函数处理。保留现有动态签名。
- 拾取 active builder `0xEAFC70`，caller `0xEAB27A` 目标一致。pickup updater `0xC9E300` 的 pawn 内部成员由 `+0x36E8` 移到 `+0x36F8`，插件只调用原函数，不直接读写该成员。
- SoundCircle producer/queue calls `0xEAE005/0xEBAD55` 到 `0xC7B070`，position call `0xED03C9` 到另一getter `0xC7B720`。声音入口 `0x3D0370 +0xEC` 到 `0x3AE8B0`，后者 `+0xB8` 的SoundSystem load及固定尾字节保持。
- TeamHealth builder/presentation `0xEB61D0/0xEC9310` 的4处pawn、2处slot、5处visibility、3处observer目标各组一致；publish点 `0xEC9639` 与member hash保持。
- MVP listener `0xCEA0C3`、player getter `0xC7B720`、mute call `0xEC4348` 的目标关系保持；playback `0xCEA835` 到 music `0x76C9F0`，gate/submit仍为其 `+0x60/+0x306`。死亡 updater `0xD0C9A0` 到 selector `0xC7A4A0`，selector和DeathPanel animation `0xE88036` 都经 `0x969A30` 共享getter。无额外detour或播放路径。
- entity add/remove slots15/16 为 `0x9F6E40/0x9F78C0`；client FrameStageNotify slot36 `0xB6C780`；engine IsPlayingDemo/GetDemoFile slots42/69 为 `0x760C0/0x763B0`。相关类型/eye/class/style槽位保留当前源码用法，未发现必须整体平移的依据。

### 控制、验证和边界

无新增效果或控制。保留所有既有Release feature默认值与生效/清理关系，尤其 `buymenu`、`buymenu_promo_hide`、`buymenu_layout` 及默认关闭的 `mirv_pov_buymenu`。关闭/seek/换图/目标切换仍关闭菜单、恢复样式、清理句柄；真实purchase/sell/buy bit隔离及共享getter caller传递保持。未重新验证server端闪光/HE阈值，不将历史server结论当成本轮新build实测。

证据目录 `diagnostics/pov-update-20261009/`：pattern-audit、function-comparison、mappings、buy-references、native-evidence、static-audit及脚本。82条POV签名均命中；其中77条client唯一匹配可比较，19组BuyMenu函数在去重定位后相同，另核验leaf/三个库存成员变化/default table变化及事件身份；51个schema字段名找到descriptor候选。候选不替代运行时类限定schema查询，签名命中不代表所有ABI已验证。

Release x64完整依赖构建成功。首轮只构建DLL遗漏新增scale shader；补齐依赖后NuGet临时缓存沙箱权限不足，提升构建权限后成功，保留两次失败日志。审计修正旧Capstone normalization对16-bit RIP memory operand误把disp32当disp16的问题，不修改游戏代码。未启动、关闭或重启CS2/HLAE；未运行demo、音视频/off-on/re-enable矩阵。构建及静态核验不等于实际加载/游戏呈现通过。本次未提交或推送，保留工作区已有调查改动。

部署：确认CS2未运行、已运行的HLAE启动器未加载目标DLL后，安装到 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`。原DLL备份为同目录 `.backup-20261009-123603-client-update`，旧SHA-256 `14F4A703B255CF485C605FFDF3C804B77208795A5A6C1AE9BCC425B68C78D343`；构建/安装SHA-256均为 `A4FB7DF208162DDE8AD75E7F23E51E598171A225E51CC4F503BE5BF469DDFA95`。`installation.json`记录部署值，`git diff --check`通过。IDA save前台请求也在300秒超时，但新 `client.i64` 已实际落盘（620425819 bytes），后续finalize完成；只关闭本轮分析worker，不操作游戏进程。

## 2026-10-11：离开购买区后购物车图标常驻

### 当前构建与原生行为

同步器 dry-run 再次计算安装 client.dll 的完整 SHA-256 为 `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008`，与10月9日库一致，复用已有 `client.i64`。image base `0x180000000`、image size `0x2994000`；下列地址为本构建 RVA，analysis VA = base + RVA，不沿用其他构建地址作为当前证据。当前安装 `pak01_dir.vpk` 中定向提取的 hud.xml/hudmoney.xml/hudmoney.css 保存到 `diagnostics/buyzone-20261011/assets/`；HUD 左下角实例 id 为 `HudMoney`，购物车图片 class 为 `money-text-buy-icon`，CSS 在祖先含 `money__in-buy-zone` 时将 opacity 从0改为1。

IDA 确认 `CCSGOMoneyPanel` 构造器 `0xEA73C0`、vtable `0x1CD52D0`，UI panel 在 client+8；构造器注册 class symbol `0x223CA0A` 并初始化 client+`0x28` 为0。原生更新 `0xEC7410` 先取控制器/账户并更新余额，再由控制器解析 pawn、检查其原生 virtual slot343；购买状态链从 `0xEC769D` 开始：GameRules slot `0x255BE80`，call `0xEC76A4 -> 0x7595B0`（购买开启）、`0xEC76B0 -> 0x7690A0`（购买时间已过）、`0xEC76C3 -> 0x8D7F40`（区域，第二参数0）。三个 predicate 本轮仍为未 hook 的原生 helper，仅调用。区域 helper 读取 pawn+`0x15E0`，并允许 `mp_buy_anywhere=1` 或等于 pawn 队伍时返回true；不把“在实体买区外”直接等同于任何模式都不可购买。GameRules购买状态和时间门槛保持原函数。

原生 `0xEC76D2` 比较缓存 client+`0x28`；**仅状态改变时**，经 panel vtable slot160 / +`0x500` 写 class（call `0xEC76E9`），随后 `0xEC76EF` 写回缓存。原生 Panorama listener `0xEAC680` 在面板可见时调用此更新函数（call `0xEAC72A`）。更新函数是本次新增的 detour；上述三个判断 helper、构造器和 listener 均没有新增 detour。

### POV 修复与控制

用户明确反馈是走出购买区就常驻。源码的确定缺口：旧 `MirvPovHud_RefreshBuyZoneIcon` 只改 Panorama class，不同步原生 client 缓存；并且只在 FRAME_RENDER_PASS 之后更新，没有覆盖后续原生 Panorama 更新。存在“POV先清除，原生随后按观战者重新添加”的覆盖窗口；该先后顺序在回归模拟中复现，实际用户回放的调用时间尚未采样，不能称为已经实机证明的唯一根因。

`MirvPovHud.cpp` 通过唯一更新入口签名、现有唯一购买判断链、`RtlLookupFunctionEntry` 的同一函数边界、完整 cache compare/store 指令及 RTTI vtable 联合验证后安装 `New_MoneyUpdate`，不硬编码本构建 RVA。每次原生更新先完整执行，再仅对 HUD 当前 money panel按当前 POV pawn重算class并同步 client+`0x28`；其他 money实例保持原样。每帧重应用也同步该缓存。client 由当前 panel 的 slot8重新取得，校验RTTI类型和client+8反向关联，不保留跨seek/换图的对象指针。没有找到有效目标时清除；主POV关闭和level init显式清理class/cache，后续原生更新可重新建立正常状态。入口/布局校验失败时不写未知client缓存。

新增独立普通Release控制 `mirv_pov_debug_feature buyzone 0|1`，默认1、即时生效、受主POV和hud控制。0只清除购物车class/cache，不关闭其他HUD效果；1立即按当前目标重算，设置在主POV启停间保留。沿用共享feature registry，不依赖诊断宏。区域判定继续尊重原生 `mp_buy_anywhere`，没有修改ConVar、网络字段、地图或购买逻辑。

### 验证与限制

当前IDB中入口及购买链签名均唯一匹配，cache+`0x28`和panel+8布局经反编译/反汇编核实；证据 `diagnostics/buyzone-20261011/native-evidence.json`。从生产源码抽取client校验、class/cache setter、refresh和native wrapper的C++回归通过21项断言：入区/出区、晚到的原生状态覆盖、buyzone off/on/re-enable、目标为空、清理后主POV恢复原生状态、hud关闭透传、其他panel透传、setter失败不写cache、无效client类型拒绝。模拟不等于游戏显示验证。Release x64构建通过；沙箱内NuGet配置读取受限，提升构建权限后成功，没有修改用户NuGet或系统环境。未启动、关闭或重启CS2/HLAE，实际demo走出买区的图标、原生callback命中和加载模块仍待用户回放验证。

专项PE静态断言 `static-audit.json` 通过。原生函数使用CHAININFO分段展开记录，覆盖购买链的段从 `0xEC768A` 开始，经链 `0xEC742F -> 0xEC7410` 回到入口；初版直接比较段入口会拒绝安装，最终校验有界跟踪CHAININFO并核对各记录位于本模块映像内。当前目录VPK SHA-256为 `48018be3f8da306ce9b48bf746bce08ceff19fe4dff15c9d5e2e53b3c3e9bbad`，只标识目录包，不代表全部分卷。

部署前再次确认CS2未运行、HLAE未加载目标DLL，原子替换 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`；备份 `.backup-20261011-132935-buyzone` SHA-256为 `5511B587C075547AD358EECD8D9286445DDAB00F108BB0BACA1D8C0B328887E0`，最终构建/安装SHA-256均为 `003B4268991E1DBF1544FBE7129E4F217010FCB3AD0B2836BFE3BEA06C26E529`。安装记录在 `diagnostics/buyzone-20261011/installation.json`；没有启动或重启游戏。按用户后续要求，提交范围仅包含购物车修复、独立控制及对应文档；原有队友武器声音调查保留为未提交改动。

## 2026-10-12：回合结束顶部头像伤害统计

### 构建身份与原生逻辑

同步器 dry-run 核验安装 client.dll SHA-256 仍为 `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008`；复用该完整哈希的 `client.i64`，image base 为 `0x180000000`。以下 RVA 的 analysis VA = base + RVA，只对本构建有效。本轮从当前安装 VPK 重新提取 hudteamcounter XML/CSS/TS，保存在 `diagnostics/round-damage-20261012/assets/`，资源哈希见 manifest.json，未修改游戏资源。

- 默认开启的 `sv_dev_damage_use_netvars` 决定数据路径（注册器 RVA `0xB46A0`，reference data 槽 `0x252C608`，bool 为对象 +0x58）。网络路径使用 **CCSPlayerController_DamageServices**，不是 ActionTrackingServices 的累计伤害。
- `OnDamageListUpdate` 回调 `0x90F9D0` 比较 services+0x40 的更新序号，更新后仅在 GameRules 存在且 round win reason 非零时调用 TeamCounter reporter `0xEB8B10`。服务 schema 字段为 `m_nSendUpdate=0x40`、`m_DamageList=0x48`；controller 的 `m_pDamageServices=0x830`。静态 descriptors 为 `0x221A4B0/0x221A4D0/0x2217880`，运行期仍按类限定查询。
- Reporter 在 call `0xEB8B3F` / return `0xEB8B44` 取得真实本地 controller（getter `0x9699F0`），要求其为 T/CT。观战者不能提供当前观看玩家的数据，且仅依赖网络回调不能补齐启用 POV、切换目标或 demo seek 后的显示。这是源码/原生控制流的确定缺口，尚未采样用户原失败时的实际分支。
- Reporter 首先调用属性清理 `0xEBD8E0`，然后遍历 damage services 的 count+0x48、data+0x50；`CDamageRecord` stride 为120字节。记录 attacker/victim 的完整 entity handles 为 +0x38/+0x3C，通过 helpers `0x903D00/0x903D60` 校验序列号并解析；按当前 controller 选择 given/taken，匹配顶部20个 avatar rows（TeamCounter+0xF0，stride0xE0，client panel+0x10）。
- `0xEC9000` 负责将记录发布为 avatar 属性 `health_removed`/`num_hits` 或 `return_health_removed`/`return_num_hits`，并选择原生 default/headshot/blast/burn/slash/shock killtype。Reporter 写 order_index，再经 UI engine slots33/47 派发 `RevealPostRoundDamageReportPanel`。本轮没有手写伤害累加、100点截断、命中计数或图标映射。
- 当前原生 TS 从 avatar 读取这四个属性，写入 `PostRoundDamageReport` 的 dialog variables、given/taken/friendlyfire 与 won/lost class；仅任一 damage>0 时按 `order_index*0.1s` 延迟加入 `show-prdr`。原生 CSS 保留绿色 given、红色 taken，以及图标与布局。
- 旧消息路径 `0xED3F30 -> 0xEC5800` 在 netvars=false 时处理 PostRoundDamageReport，保持原样；本轮只实现当前默认网络记录路径，未伪造缺失记录。某 demo 没有目标玩家的网络 DamageList 或使用旧消息路径时，不能承诺恢复本图统计。

### POV 实现、接入与控制

`MirvPovHud.cpp` 新增 reporter 与 controller getter 两个 detour。Reporter 在主 POV、hud、round_damage 开启、有效 demo、回合结束且非新回合冻结期、有效当前 T/CT controller 和已校验 DamageList 时，保存完整 controller handle 到线程局部作用域，仍完整执行原生 reporter；getter 仅对 slot0、上述唯一返回地址、当前 handle 完全一致时替换 controller。其它调用与 inactive reporter 透传，`__finally` 恢复上下文，兼容嵌套和异常。`MirvPovBuyMenu.cpp::GetController` 透传分支补齐共享 return-address Push/Pop/SEH 恢复，防止 getter detour 链丢掉外层原生 caller；购买菜单自身 scope 不变。

Reporter入口由唯一完整签名定位，并检查实际 services load、vector count/data、record stride、row起点/stride/20项上限等指令；另验证 getter E8 目标处于本模块 text、TeamCounter RTTI 和现有 FindHudElement 签名。运行期 schema 不符合上述布局时不接管。入口字节和签名见源码及 static-audit.json；本轮修正了初版两项相对偏移，在当前 PE 上重新断言通过后才用于最终构建。

每个 render pass 完成后，重新查询注册 HUD 并校验其 RTTI，不缓存跨图/seek 的裸对象。比较完整 controller handle、更新序号、记录指纹及20行slot/panel identity，变化时才调用 reporter；稳定帧不重发 Reveal 动画。反向 demo tick 强制重建；前向大间隔不当作生命周期终止。记录 count 必须在0..256、有记录时指针非空；这是 POV 的防御边界，不声称原生限制为256。

新独立普通 Release 控制为 `mirv_pov_debug_feature round_damage 0|1`，默认1，即时影响下一次 callback/render pass；关闭命令立即清理本功能拥有的 `show-prdr`，其它HUD/受击/音效独立。重新开启或重开主POV使用当前网络快照恢复，不依赖从回合开头重新累计。feature 配置跨主POV启停及换图保留，不写用户配置。无有效目标、新回合冻结/实战阶段、断开、主POV关闭和关卡初始化会清理；清理只作用于 `afx-pov-round-damage` 标记的行。

原生 JS 已排队的0.1秒逐行 Reveal 没有取消接口，也没有卡顿下的硬执行期限。最终实现取消初版2.2秒清理超时：关闭后将自有 `PostRoundDamageReport` 的 visibility 置为 collapse，保留所有权至重新启用或 demo/关卡结束，迟到 callback 即使加入 show-prdr 也不能穿过该可见性屏障。恢复时调用既有已核验的 CPanelStyle slot151 单属性清理，只清除本功能 visibility 覆盖并恢复原生 CSS（包括底部布局的 collapse 条件），不强制 visible。该清理也在主POV/framestage关闭时运行。没有清空真实 DamageList、改网络字段、ConVar或用户账户数据。

### 验证与边界

当前完整 client 哈希、唯一签名、getter call/return、四组布局指令、三个 schema descriptor 和本轮提取 TS 的四个属性/延迟事件断言通过。使用实际生产 reporter/getter/snapshot/update/clear 函数的 C++ mock 回归通过34项断言：稳定帧不重播、数据/序号变化、off/on/re-enable、迟到Reveal清理和不依赖帧/超时的可见性屏障、目标切换/handle重用、反向tick、正向正常推进、回合开始/结束、hud/主POV/无demo/无目标、未知schema、超大vector拒绝、其它caller/slot透传、过期handle拒绝、异常恢复TLS、覆盖释放和inactive reporter透传。模拟不等于游戏面板/网络回调实测。独立只读复核指出初版UI preparation在native入口缺乏异常捕获，现已将新增的panel遍历/写入单独置于 SEH 捕获，失败时不进入POV作用域并回退原 reporter；原 reporter 自身的异常保持原生传播语义，__finally仍恢复TLS。

首轮沙箱构建因读取 NuGet.Config 受限失败；提升构建权限后修正缺失 extern，最终 Release x64 构建成功，34项生产控制流断言、PE专项核验和CRLF-aware `git diff --check`通过。未启动、关闭或重启 CS2/HLAE；尚未进行游戏内截图、音视频、不同模式/分辨率或真实 off/on/seek 矩阵，不将构建与 mock 结果冒充实际显示通过。

部署：再次核验当前client完整hash、确认CS2未运行、HLAE未加载目标文件后，将旧DLL备份至 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll.backup-20261012-170917-round-damage`，旧hash为 `003B4268991E1DBF1544FBE7129E4F217010FCB3AD0B2836BFE3BEA06C26E529`。原子替换同目录 `AfxHookSource2.dll` 后，构建/安装hash均为 `F9C2E07C6BB61378D45A241CC6208AC1761F0C880B94ED0582B6FF27D9C1AA69`；见 `diagnostics/round-damage-20261012/installation.json`。首次Replace因PowerShell将空backup参数传为空路径而未执行，目标未改动；使用实际备份路径重试成功并验证原子备份hash。未提交或推送，原有调查文档及分析产物保留在工作区。

### 2026-10-12 后续：自动跟随下 controller getter 递归修复

用户运行后反馈卡死。Windows Application Error 1000 实际记录 17:14:43 和17:15:35 两次 `0xc0000005` 崩溃，fault module 均为上述 F9C2E07C 构建的 AfxHookSource2.dll。使用当时构建的匹配PDB和PE，RVA `0x3630BD` 映射至 BuyMenu `GetController` 的 PushHookReturnAddress 调用，RVA `0x368D0A` 映射至 `New_RoundDamageController` 同一调用；没有可读取的本次dump，不能将具体运行期栈耗尽视为已实证。事件、console、符号映射和反汇编保存在 `diagnostics/round-damage-crash-20261012/`。console最后一次seek到tick58232后结束；检查时CS2已退出、HLAE仍在。

源码缺陷已确认：原生 reporter 作用域设置目标handle后，getter又调用 `GetCurrentPovPlayerController`。默认auto-sync经 `ResolveObservedPlayerController -> ResolveObservedPlayerPawn -> GetRealSplitScreenPlayer(0)` 回到已经hook的native entry；共享return-address与目标handle尚有效，导致同一条件反复成立。先前34项mock将POV解析简化为固定指针，因此遗漏此路径。购买菜单wrapper仅转发并保留上下文，两个崩溃位置与递归路径一致，但运行期异常链尚缺dump核验。

修复保留原生reporter/getter接入和唯一caller限制：在reporter进入前解析当前POV，getter内部仅将已保存完整handle构造为SDK `CEntityHandle`，通过 `GetEntityFromIndex(handle.GetEntryIndex())` 找实体，再检查controller类型和完整handle（含serial）。这三个查询不经过本地controller getter；getter不再重新解析观战目标。失效实体、序列号变化、其他caller/slot和关闭功能均回退原getter；作用域仍用 `__finally` 恢复。没有缓存跨seek/换图的裸controller指针，也没有修改买菜单scope。

原生/live边界：本轮client完整SHA-256再次核验仍为 `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008`，image base `0x180000000`，上述本构建RVA与call/return不变。真正的reporter证据位于 `native-evidence.json` 顶层 `0x180eb8b10`；旧键名 `report` 实际保存 `0xEBD8E0` 属性清理helper，`clear` 保存 `0xEC9000` 逐记录发布helper，不能按键名误认函数。本实现已经完整调用正常对局的原生 `0xEB8B10` reporter、原生 `0xEC9000` 发布和Panorama显示；DamageList数值与命中数来自游戏，不由DLL重新累计或生成。`OnDamageListUpdate(0x90F9D0)` 的原生网络触发保留；DLL只为demo观战替换reporter的本地controller视角，补偿开关/换目标/seek后的重新发布和清理。完全撤掉这一适配时，原生live逻辑仍查询真实本地controller，而不是被观看目标；它也不会因POV开关或目标切换自然重发。缺少目标DamageList的demo仍无法只靠HUD原生函数补出记录。

验证：回归从实际生产round_damage函数和BuyMenu controller wrapper抽取代码，POV解析模拟回到被hook的真实本地getter入口。旧getter确定触发递归深度9的受控异常；修复版最大深度1、50项断言通过，包括原有开关/清理/seek测试、真实getter转发链、完整serial验证、缺失实体/错误类型、嵌套scope恢复以及auto-sync模式下独立和主POV off/on。当前PE唯一签名/布局/schema/资源专项校验通过；Release x64构建成功，SDK原有代码页警告仍存在。未启动、关闭或重启CS2/HLAE，未进行真实面板截图或游戏内数轮回放，不将mock和编译通过视为游戏回归已通过。独立Release控制仍为默认开启的 `mirv_pov_debug_feature round_damage 0|1`，作用范围及清理与上一节一致。

修复版部署：安装前确认CS2未运行，当前仍打开的HLAE未加载目标DLL；原子备份旧F9C2E07C构建至 `AfxHookSource2.dll.backup-20261012-174132-round-damage-recursion-fix`，然后替换同目录DLL。构建和安装SHA-256均为 `E7639F4E6A00D4F1D00EC562E9E2880AEBEF11180AC261B2D04F3D3F5C19DA24`，完整路径、时间和hash见 `diagnostics/round-damage-crash-20261012/installation-fix.json`。没有控制游戏/HLAE生命周期，未提交或推送。

### 2026-10-12 后续：GOTV 可见性与面板创建时序

用户测试E7639F4E版后仍无显示，并明确测试为GOTV；本轮授权为“只分析和修改，不启动游戏”。console核验加载构建时间09:38:25Z，17:44:03启用POV，seek58232后tick58361的原生MVP音乐命中。原日志没有round_damage入口、记录或面板证据，不能仅凭MVP出现确认伤害统计入口执行，也不能把随后播放结束误称为崩溃。

离线解析当前 `inferno_test.dem`（286101252字节）获得GOTV/SourceTV类型、network protocol14174、build10847，并能解析伤害事件。SendTables command4经Snappy解压后含 `CCSPlayerController_DamageServices`、`CDamageRecord`、`m_pDamageServices`、`m_DamageList`、`m_nSendUpdate`；证据 `diagnostics/round-damage-visibility-20261012/demo.json` 和 `schema.json`。这些证明录像有伤害事件和字段描述，不证明本次POV的运行期DamageList非空，不将“不支持GOTV网络字段”当作已确定根因。

当前client完整SHA-256仍为 `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008`，image base `0x180000000`。复用同版本IDB核验原生旧消息dispatcher `0xED3F30`（analysis VA `0x180ED3F30`）检查消息+80/+92两个killtype不超过6，FindHudElement后调用未hook的helper `0xEC5800`。该helper要求netvars开关为false、GameRules非空、`0xC7B720`返回对象team+999为2/3，按消息XUID+72匹配20行，读取+84/+88/+96/+100发布四个伤害属性并派发原生Reveal事件。默认netvars路径notify `0x90F9D0` 比较+0x40更新序号并在GameRules+0x9AC非零时调用hooked reporter `0xEB8B10`；两个路径具有互斥ConVar门控。本轮保存反编译结果至 `native-evidence.json`，没有新增legacy hook，没有修改ConVar或网络字段。reporter入口+6的RIP-relative load（displacement+9、下一指令+0x0D）仍指向reference RVA `0x252C608`；对象+0x58只用于读取日志，静态PE断言核验了该关系。

已证实的源码/资源缺口：当前VPK CSS具有 `.TeamCounter__Align--Bottom .prdr__root{visibility: collapse;}`，上一版有意恢复CSS会保留这一观战隐藏条件。另一个缺口是即使找不到任何 `PostRoundDamageReport` 子面板，UI preparation仍设置prepared=true并缓存快照；行/头像身份不变时，稍后创建子面板不会触发重试。缺少实机面板状态，不能断言这两个条件中的哪一个实际命中了用户测试。

修正上述CSS设计：仅在本效果启用且reporter接管时，对自有原生伤害面板设置inline visibility=true，覆盖GOTV布局的隐藏规则；数值、命中数、killtype、dialog变量和Reveal动画仍由原生reporter/TS处理。关闭功能/主POV/离开回合结束期时仍使用collapse屏障阻止迟到JS；释放所有权、断开或换图时清除inline覆盖恢复原生CSS。只在所有有效头像对应子面板可取得且visibility setter成功时进入POV作用域并缓存；未就绪时快照无效，后续render pass重新尝试。既有 `mirv_pov_debug_feature round_damage 0|1` 默认1、即时生效及独立范围不变。

普通Release新增状态变化日志 `[mirv_pov_round_damage]`，记录stage、tick、完整controller handle、DamageList count/update、成功准备的panel数、netvars模式、native reporter调用次数及controller override次数。stage区分hook/style不可用、主POV/hud/feature关闭、无demo、回合未结束、GameRules/HUD/POV/services缺失、schema/列表无效、面板未就绪、空DamageList、调用原生reporter和异常。空列表和“native-report-called”均不等于统计可见性验证。现有 `mirv_pov_debug_feature` 无参数查询也打印最近状态；没有增加单独effect开关或依赖诊断编译宏，稳定状态不逐帧刷日志。

验证：实际生产getter/report/update/clear与BuyMenu wrapper抽取回归通过65项断言，覆盖原有autosync最大深度1、off/on/主POV/seek/序列号/异常恢复，以及模拟原生GOTV CSS collapse、启用期间强制可见、释放恢复CSS、子面板延迟后重试、不把空列表日志当显示通过。当前PE/signature/layout/schema、ConVar引用及CSS/录像字段专项核验通过；Release x64编译和CRLF-aware diff检查通过。没有启动、关闭、重启或控制游戏，未做本版实际面板验证。

部署：确认CS2未运行、HLAE未加载目标DLL后，备份E7639F4E版到 `AfxHookSource2.dll.backup-20261012-180809-round-damage-visibility` 并原子替换指定HLAE DLL。构建和安装SHA-256均为 `E9BC6D9069507ECC478C56355CFA0507156CDDE51CA28E19E01CCF3CD5552EBB`，详见本轮 `installation-fix.json`。未提交或推送。

### 2026-10-12 后续：GOTV 空 DamageList 的录像事件补偿

本次重播日志终于提供实际失败分支：19:15:39、demo tick58363、controller `01240002` 和19:15:57、tick58591、controller `00288003` 均为 `stage=damage-list-empty records=0 update=0 panels=10 netvars=1`，native reporter/controller override计数非零。因此本份GOTV中原生面板、hook和POV替换已执行，真正缺口为被观看controller没有网络DamageList；先前的可见性修复和完整调用live reporter无法制造缺失数据。没有把正常退出日志当作新崩溃。

完整离线读取 `inferno_test.dem` 共141436 packet、20,771条type207事件、1,159条player_hurt。type205为Source2 legacy GameEventList，type207为legacy GameEvent，使用descriptor中实际key名与type解码。该录像无round_start/round_end事件，含23条round_prestart、23条round_freeze_end；不能只依赖round_start/end重建边界。完整message扫描没有目标原生PostRoundDamageReport消息，故不能通过解除旧消息helper门控恢复本份录像。本轮用PBDEMS2 framing、raw Snappy、protobuf wire和UBitVar消息流实现独立只读索引；不依赖csda.exe、外部Python、游戏启动或用户生成sidecar。command13 fullpacket只读取descriptor，不重计历史伤害；当前正常packet事件tick有序性和损坏/长度/复制偏移受检查。

构建身份再次核验为client SHA-256 `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008`、image base `0x180000000`。复用同版本IDB补查：publisher `0xEC9000` 的 `movss [record+0x68]` 通过XMM2写float属性，+0x6C为hits，+0x75为0..6 killtype。Schema静态descriptor `0x221A380` 的字段为 `m_flDamage` (+0x64)，`0x221A3A0` 为 `m_flActualHealthRemoved` (+0x68)；name strings RVA `0x1AEEAF8/0x1C0D7E0`，字段注册helper `0x8FE8C0` 的反编译亦确认名字顺序。原生UI读取实际扣血，不是包含致死过量的原始伤害。索引使用hurt的dmg_health及剩余health，按spawn/每轮初始HP和此前所有hurt（含世界/自己伤害）裁掉致死overkill，不把总量硬限制为100；分生命累计，death提供default/headshot/blast/burn/slash/shock类型。分析VA均为base+RVA，仅本构建有效。

原生reporter仍为hooked `0xEB8B10`；以下均是**未hook的native playback helper**：reporter+0x77 E8解析clear `0xEBD8E0`，+0x138解析publisher `0xEC9000`，+0x1E5解析allocator `0xC30A20`，+0x17D解析symbol interner `0x177FAA0`。RIP-relative refs +0x19A/+0x1F2/+0x256分别为UI engine槽 `0x272ABC0`、Reveal symbol `0x223C9C0`、CUIEvent vtable `0x1CB5BA0`。源码验证这些call/opcode、helper入口和+0x68/+0x6C/+0x75 load，代码目标必须在本模块text，数据/vtable必须在image范围。槽位和构造遵循原生：UI engine slot33取得avatar和TeamCounter的CPanelPtr handle，原生allocator分配32字节event，+0为final CUIEvent vtable、+8为event symbol、+0x0C为target handle、+0x18为avatar参数；slot47接受event指针的地址并接管所有权。native row slot经 `0xAC9D90` 解析为entity index=slot+1；该helper本轮另行反编译核验，无新增hook。详见 `diagnostics/round-damage-gotv-20261012/native-fallback.json` 和 `static-audit.json`。

实现来源明确分开：有原生DamageList时完整执行原生live reporter；只有列表为空才由 `MirvPovDemoDamage.h` 的录像事件索引提供当前回合、当前tick、当前POV与每个avatar的given/taken数值。`MirvPovHud.cpp` 把结果放入私有120字节参数buffer，调用原生clear/publisher和Reveal事件；UI dialog变量、颜色、图标、优势class和动画仍由游戏原生TS/CSS处理。**GOTV补偿的统计数据由DLL从真实录像事件恢复，不能继续称所有数值均来自原生网络DamageList**。没有修改controller/services/vector、ConVar、游戏资源或用户配置。

解析通过现有thread pool在后台处理，只持有路径/索引/共享cache，永不访问entity或Panorama；文件大小不会阻塞render线程。主线程用release/acquire状态读取完成后不可变的索引，结合ready状态、round start/end、已发生事件数、原有controller/row指纹触发重新发布；稳定帧不重复Reveal，未来tick事件不提前显示。回合是否结束仍由当前GameRules非freeze且win reason非零判断，GOTV未传round_end时index.end=-1仅是未知值。换路径、无demo和level init取消旧task并清cache；旧task持有自己的cache，不能污染新录像。相同文件重播/seek可复用不可变索引，新的level init会重建，即使路径相同。未启用主POV/hud/round_damage不发起新扫描；已读cache可用于off/on。面板未就绪会重试，新增publisher/Reveal的SEH异常尝试清理部分界面并使快照失效。

控制仍为普通Release默认1的 `mirv_pov_debug_feature round_damage 0|1`，即时生效、独立于其它效果；关闭/主POV关闭/新回合/离开demo沿用自有面板collapse屏障和释放策略，重新开启不要求从回合开始播放。日志新增 `demo-fallback=1`、`demo-index-queued`、`demo-index-loading/failed/unavailable`、`demo-report-called`、`demo-no-damage`、`demo-round-unavailable`、`demo-publish-exception`，发布另打印row数和回合边界；`native-report-called`/`demo-report-called`都是调用状态，不等于已实机截图确认显示。

验证：生产C++索引完整解析本份录像23个回合，406项断言通过；优化扫描单次约0.6秒，属于离线机器测量、不保证游戏运行时耗时。tick58363的slot1->10为实际扣血100、2hits、default kill；tick58355仅81、1hit、无kill；目标slot2对slot9为100、2hits、headshot。生产HUD/getter/cache/native事件控制流mock90项断言通过，包含loading->ready、空列表补偿、原生event字段和所有权模拟、稳定不重播、seek未来事件过滤、目标切换given/taken、off/on/主POV、重播cache reset、真实threadpool取消任务隔离、部分publish异常后清理和retry。当前PE signature/layout/calls/RIP refs/两个health descriptor专项断言及Release x64构建通过；独立复核指出未知frame seek可能越过EOF和事件乱序隐含假设，现分别使用ignore+gcount检查和建索引时有序性检查；不足14bit packet尾部仍按现有协议读取器处理，未把该兼容行为当严格padding验证。

局限：本轮按用户要求未启动、关闭、重启或控制CS2/HLAE，未确认真实原生事件的最终屏幕效果。GOTV fallback使用事件整数HP，不能声称逐位等同live服务端float累计；非标准初始HP、同slot中途换身份、特殊逐弹/同shot多事件的native命中合并仍需模式实测。普通本份录像的扣血/目标映射已离线验证。源码和分析未提交/推送，后续部署记录见本轮 `installation-fix.json`。

部署完成：再次确认CS2未运行且HLAE未加载目标DLL，于19:59:23备份旧版到 `AfxHookSource2.dll.backup-20261012-195923-round-damage-gotv` 后原子替换。安装路径为 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`，构建/安装SHA-256一致，均为 `DD020E062B65476E88742B1D59879C6332C4C37A344BF4DEA3560F559430844E`；备份旧hash为 `E9BC6D9069507ECC478C56355CFA0507156CDDE51CA28E19E01CCF3CD5552EBB`。CRLF-aware diff检查通过，未启动或重启游戏/HLAE。

提交前整理：将native publisher的私有参数buffer整理为 `PovRoundDamageArgument`，用sizeof/offsetof四项编译期断言锁定120字节大小及actual health/hits/killtype偏移，保留原有运行期instruction契约验证；没有改变累计或显示规则。去除解析器未使用的include，中英文README补充默认开启、hud依赖、即时开关、seek/目标切换、退出清理和GOTV伤害事件计数的限制。整理后Release x64构建、406项解析断言、90项生产控制流断言及PE专项检查再次通过；未启动游戏，仍不能将模拟验证写成实际显示已通过。当前工作区中2026-09-22切枪音效调查属先前未提交内容，保留在工作区，本次只提交2026-10-12伤害统计记录。

## 2026-10-12：暂停录像时购买菜单阻挡切人输入

### 构建身份与原生行为

安装 client.dll 的同步器 dry-run 完整 SHA-256 仍为 `44af2aa0d2c64f90d273444cb393b8b679094f0b02136dbdb89d4ec82a560008`，复用 `diagnostics/client-dll-analysis/44af2aa0d2c64f90/client.i64`。安装 panorama.dll 的完整 SHA-256 为 `4b3eb751f78bcbea82b146dcc5e2668d8fa19263c82dd35321441feb2be43c24`，本轮复制并建立同 hash 数据库；没有使用旧 `a607e0...` 库的地址。两模块 analysis image base 均为 `0x180000000`，下列 analysis VA = base + RVA，仅适用对应完整 hash。

原生 CCSGO_BuyMenu 构造 RVA `0xD97770` 在 VA `0x180D979CE` 通过 client global `0x2793D20` 的 virtual slot23 注册 `CSGOBuyMenu`，priority1000，handler 为 menu+32。handler vtable RVA `0x1CB0A68` 的 slot0 为 RVA `0xDAAB30`，ABI `intptr_t __fastcall(void *handler, void *event)`；入口先由 handler-32 取得 menu，并检查 menu+8 UIPanel slot34 的可见性。随后只处理 event+8 为0/1的事件，读取 event+32 的 key code，在数字类快捷键、关闭键、捐赠修饰键等分支返回1表示处理。此处理条件不要求 demo tick 前进；不能通过阻止真实购买/出售让它自动停止接收输入。

原生 open/close 仍为 client RVA `0xDB4FC0/0xD9E230`（已有 detour）。Open 先可见性判定，再显示、移动 TeamCounter、刷新装备等；close 隐藏并执行原生恢复。POV 的 Close 在 `g_Active && g_Wanted` 时拒绝 viewer-side 关闭，等待记录玩家关闭菜单。用户描述的暂停锁住与这组可见性/输入处理/关闭屏障相符，但本轮没有采集其实际暂停场景输入调用序列，不称为已实机证明的唯一根因。

Panorama 本轮反编译和 PE vtable 核验结果如下；这些 setter/getter 是未 hook 的原生 helper，仅调用：

| CUIPanel slot | RVA | 原生含义 |
| --- | --- | --- |
| 31 / 32 | `0x112B60 / 0x112B30` | parent / window |
| 165 / 166 | `0x1125A0 / 0x104440` | accepts-focus getter/setter，panel+554 bit4 |
| 178 / 179 | `0x112510 / 0x1117C0` | focus-context getter/setter，panel+554 bit2 |
| 203 / 204 | `0x1126B0 / 0x112570` | hit-test setter/getter，panel+554 bit8 |
| 206 / 207 | `0x112630 / 0x112560` | hit-test-children setter/getter，panel+554 bit32 |

CUIPanel vtable RVA `0x4CCFF8`；slot203/206会调用原生状态失效 helper，不能仅直接覆盖字节假装完整 setter。Window slot4 返回其 input。CUIWindowInput vtable RVA `0x4D4E98`；slot19 / RVA `0x13A4B0` 是 `SetInputFocus(input, panel, bool scroll, bool context)`，该入口本轮新增 detour。其 input+32 是实际 focused panel 弱句柄，+24 是 focus context 弱句柄，反编译确认 SetInputFocus(nullptr,false,false) 清除二者并执行原生焦点标志/事件清理；slot22/RVA `0x139B90` 为 context getter。本轮读取弱句柄并经现有 UI engine slot34 解析，不保存可在卸载后悬空的 focus 指针。

### POV 实现与独立控制

`MirvPovBuyMenu.cpp` 新增 `KeyInput` 和 `InputFocus` detour，前者仅对正在模拟的菜单 handler 返回0，让后续输入处理器继续处理；后者仅拒绝把焦点设置到该菜单或其子树，其他界面正常透传。Open 的临时 `OpeningInputScope` 在 native FullRefresh 和 CacheMenu 前就限定焦点请求范围；ApplyInputState 必须等 CacheMenu 获得可解析的确切弱句柄后才保存/修改标志，避免把原生 Open 内部刷新当成已拥有的菜单，丢失原始状态。

`ApplyInputState` 保存 root 的四个原始布尔值，关闭 root/children 的鼠标命中及 root 的 accepts-focus/focus-context；只有实际变化才调用 setter。已有焦点仅在 focused panel/context仍属于该购买菜单时通过原生 trampoline 清除，不清除控制台/demo UI的焦点，不抢夺其他overlay的焦点。按当前菜单弱句柄辨认所有权，父链遍历最多128层。检查完整 Panorama hash，并逐项验证使用的 UIPanel vtable slot，验证 input 的slot19/22再读取私有句柄；未知 Panorama 构建不安装两个输入 hook，状态报告 `input_supported=0` 并告警，保留原有购买菜单模拟。

普通 Release 共享 registry 新增 `mirv_pov_debug_feature buymenu_input_passthrough <0|1>`，默认1、即时应用、需主POV和购买菜单模拟。0只恢复保存的菜单输入标志和原生输入hook透传；1重新保存/禁用并释放仍属本菜单的焦点，不关闭 promo/layout/model 等效果。`all` 也触发即时reapply。配置在主POV启停间保留。Open返回后、Refresh/FullRefresh/Hover/Think返回后以及Update每帧重新施加；Update路径不依赖tickChanged，因此暂停时同样更新。原生 close前、Reset、切人、seek、demo/level teardown和主POV关闭恢复标志并清保存状态；layout已销毁时只清弱句柄状态、不解引用旧panel。

中英文 README 记录命令/default/scope/时机/清理及取舍：穿透开启时录像菜单只作展示，不能鼠标悬停或点击；关闭控制恢复交互处理。未修改购买判断、记录buy bit、游戏VPK、ConVar或其他UI的输入状态。

### 验证与边界

Release x64构建通过。`diagnostics/buymenu-input-20261012/test.py` 提取生产的apply/restore、弱句柄所有权、focus/key detour和opening scope，仅mock原生ABI边界，35项断言通过：Open内部刷新尚无handle不改原值，开菜单途中focus拦截，暂停重复帧，native覆盖后重施加，其他panel/handler透传，off/on/re-enable、主POV启停，关闭恢复，原值部分为false的恢复，销毁旧handle，未知build/panel拒绝，父链环路有界。`verify.py` 用实际PE独立核验完整hash、12个Panorama vtable slots、client handler vtable/入口指令以及生产集成点，通过。`native-evidence.json` 保存本轮原生伪代码，`static-audit.json` / `control-test.log` / `build.log` 保存核验结果。

实现阶段的35项检查属于静态和控制流模拟核验，不能当作游戏实测。本轮没有由代理启动、关闭、重启或控制CS2/HLAE。安装后用户反馈“成功了”，确认原先暂停demo切到正在购买的玩家后无法继续切人的问题已解决；未取得逐键测试日志，不将该反馈扩大为所有快捷键、控制台/demo面板、全部开关组合或游戏模式均已实测。

部署：21:12:21再次确认CS2未运行、HLAE未加载目标DLL，通过File.Replace备份并原子替换 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`。构建和安装完整SHA-256一致：`6A4D05D922D79E604E2311B5645E7A753A6800ACE9891DD24458A4CDD0E6B531`；旧版备份 `AfxHookSource2.dll.backup-20261012-211221-buymenu-input` 的hash为 `DD020E062B65476E88742B1D59879C6332C4C37A344BF4DEA3560F559430844E`。证据保存在本轮 `installation.json`，没有启动或重启游戏/HLAE。

数据库保存请求返回300秒超时，磁盘已生成 `diagnostics/buymenu-input-20261012/panorama.i64`（82,536,547字节），本轮未重开验证该数据库；已完成的原生伪代码另外保存在 `native-evidence.json`，ABI核验直接读取对应hash的PE，未依赖这次保存成功。

## 2026-10-15：游戏更新后的 POV 兼容性审计（仅诊断，未适配）

### 构建身份、范围及证据

本机安装文件重新计算 SHA-256 并将 client 输入复制到独立版本目录。以下模块 image base 均为 `0x180000000`；本节统一使用 RVA，analysis VA = image base + RVA，不将旧构建地址当成当前事实。

| 模块 | SHA-256 |
| --- | --- |
| client.dll | `659940c1db362ed981d1df10637cec8c2a3d99409b77fc3b73e435e4649e36b0` |
| panorama.dll | `b5028e9077f0176cd338e6cdac92b8d5024519e6833bd0e979a2f1062ccef412` |
| engine2.dll | `6456df18d8f954394024c2c07da8c848df15a5ace74ce7390d78970ceb39ee21` |
| schemasystem.dll | `d2a9b5164cb68953e77d0ef424c1263d2cc452a75e935316bc8b4c4500e63648` |
| soundsystem.dll | `4de8a596efcca2619fa55a46bedb151b7e835a52560c37849bbfb03ade7ad36b` |

client image size `0x299D000`。IDA 新基线识别106920 functions / 66381 strings，warmup确认自动分析及Hex-Rays ready；该基线不表示所有函数都已人工语义核验。独立输入位于 `diagnostics/client-dll-analysis/659940c1db362ed9/`。本轮源代码审计基于 HEAD `8ef281f476dfe5e521bf3cee4b8a95745fb864ee`；用户日志的构建标记为 `4f37e234-dirty`，未据此假定用户运行二进制与当前HEAD逐字节一致。

证据位于 `diagnostics/pov-update-20261015/`：`pattern-audit.json`（85条POV源码签名）、`comparison.json`（74条新旧client唯一命中的外围函数比较、迁移候选及schema候选）、`semantic-diffs.json`、`flash-calls.json`、`contracts.json`、`remaining-contracts.json` 与 IDA `native-evidence.json`。迁移投票只是候选，不可直接用于补丁；`compare.py` 的BuyMenu候选仅提取 `g_Base`（client），Panorama地址由RTTI/vtable单独读取。IDA batch的长反编译输出存在截断，关键指令另用PE/Capstone验证。schema候选检索不能替代运行时按类限定的schema查询。

### 游戏原生逻辑及已确认需要适配的接入

1. **闪光画面**：compact call `0x11C96CB` 和per-view call `0x11E00ED` 均调用 predicate `0xCE2420`，返回点分别为 `0x11C96D0/0x11E00F2`。per-view现在读取 `[rbp+0x238]`、`[rbp+0x220]` 并对 `r15+0x1398` LEA，旧签名的 `rbp+0x250/+0x238` 与r13编码不再成立。`MirvPovHud.cpp:1284-1308`因此拒绝安装flash predicate detour；需迁移上下文签名并保留共同E8目标验证。该结论是画面路径，不等于服务器闪光/HE失聪阈值改变；本轮未分析server.dll或验证听感。
2. **雷达阵营/C4显示**：原生bomb-package update迁移到 `0xEC71C0`，与旧函数同长但寄存器和部分观察/高度逻辑均变化；结合 `CCSGO_HudTeamCounter`、`PlantedBombAnimateRed` 等字符串及原生调用链核验身份。root `+0x2A0/+0x17808/+0x17810`仍在当前函数中出现，不能因此推定全部对象ABI均验证。`MirvPovRadar.cpp:665-729` 的package入口、HUD lookup、slot resolver、relation caller四个签名失配。relation所在更新函数迁移到 `0xEBE8C0`，原本 `[rsp+0x38]` 的controller改存 `[rbp-0x80]`，结果仍写 `[rbp+0xA0]`。现有安装失败关闭并回滚该组presentation patches，故CT视角C4红色/关系等修正需要适配，而非只替换一个入口签名。package updater与relation为既有detour；HUD lookup、slot和颜色resolver是仅调用的helpers。
3. **购买菜单及暂停输入穿透**：`MirvPovBuyMenu.cpp:21-24,678-706`仍绑定client `44af...`，新hash导致所有菜单模拟hooks拒绝安装。旧open/close/refresh/full-refresh/key handler分别映射到候选 `0xDB7E90/0xDA1100/0xDBB1E0/0xDBB280/0xDADA00`，大部分菜单函数归一化相同，但候选不是已完成的适配。`m_bIsBuyMenuOpen=0x15EA`、inventory `0x820`、weapon/item services `0x12F0/0x12F8`的descriptor候选仍存在。Panorama完整hash也改变，使 `DiagnosticPanoramaSupported()` 为false，单改client hash不能恢复输入穿透；当前RTTI实测CUIPanel/input已用的16个槽位目标地址保持，但完整布局、getter/setter指令和菜单事件creator/globals仍需专项核验。不得只放宽hash或沿用旧RVA。
4. **无线电文字、击杀奖励文字的共享demo门控**：原生HudChat formatter `0x11A5440` 在 `0x11A546E` 调用真正demo getter **`0xD36BC0`**，它返回对象RVA **`0x223F840`**；在 `0x11A5473` 检查对象 **`+0x82`**，非零则跳过本次格式化/notice。RadioText formatter `0x11A7CA0` 的 `0x11A83FA` 和TextMsg handler `0x11A8DC0` 均到该共享formatter。旧getter签名仍唯一命中 **`0xD36BD0`**，但返回的是另一个对象 **`0x223F940`**，不是原生formatter使用的对象。`MirvPovRadio.cpp:157,3081-3100,3182-3187,3284-3334` 与 `MirvPovKillReward.cpp:71,450-455,1301-1323` 会继续对误识别对象 `+0x72` 临时读写；这是比零命中更重要的静态身份错误。KillReward `:1261-1282`限定formatter前0x200范围的 `80 78 72 00 0F 85` 也不再匹配；全模块仍有一个同形命中 `0x83BA04`，与目标范围无关，不能把全局命中当作兼容。需要一起修复getter身份、字段偏移和局部补丁条件；不能仅修改常量而保留错误getter。此处是已有hook/patch内使用的helper，不新增hook。最终文字呈现、声音fallback是否掩盖缺口仍须实际demo验证。
5. **MVP播放诊断观察器**：核心player getter `0xC7C160`、listener call `0xCEAB43+15`、mute call `0xEC89D8`的调用关系仍通过，共享getter为 `0x96A8F0`。失配的是只读observer使用的music demo gate：music `0x76CD70` 在 `+0x60`调用 `0xD36BC0`，之后检查 `+0x82`；指令增长3 bytes，submit从music `+0x306`变为 **`+0x309`**。`MirvPovMvpMusic.cpp:73,156-173`仍要求旧gate签名、`+0x306`和诊断指针`+0x72`，因此playback observers不安装。核心上下文已在调用observer前建立，不能把此失配写成MVP音乐功能整体失效。修复范围是诊断定位/字段与observer契约；保持原生静音条件，不清除music suppress。

### 已通过的局部静态核验及剩余不确定性

- 回合伤害reporter `0xEBD170` 的controller services `+0x830`、record size120、rows `+0xF0`/stride`0xE0`、20 rows及fallback publisher的 `+0x68/+0x6C/+0x75` loads检查通过；getter/clear/publisher/symbol/allocator分别为 `0x96A8B0/0xEC1F70/0xECD690/0x17858C0/0xC319D0`。只有reporter和controller getter是既有detour，其余为仅调用的播放helpers。用户的 `hooks-installed`、`demo-fallback=1`是初始化成功状态，`controller=FFFFFFFF records=-1`这条安装阶段快照本身不是功能失败证据；不能据此承诺实际统计已经显示。
- 玩家语音 `0xB46960` 仍从message `+0x40/+0x6C`读取已用字段，签名和关键指令通过；差异主要为原生内部全局数组重定位。SoundCircle三处caller `0xEB25F5/0xEBF385/0xED4AB9`的前两目标同为 `0xC7B9D0`、第三为 `0xC7C160`；DoStartSoundEvent `0x3D06F0+0xEC`到 `0x3AEB40`，`+0xB8`接口load/尾部检查通过。未实际调用hash helper或验证声音输出。
- 拾取active builder `0xEB4260` 与caller `0xEAF86A`的E8目标一致。死亡画面使用的 `m_bKilledByHeadshot`从 `0x1EC9`变成 `0x1EC5`，护甲从 `0x1ECC`变成 `0x1EC8`；当前源码通过schema动态取得，未发现需硬改这两个偏移的证据。死亡updater对应差异也为原生字段迁移。其余签名命中、归一化比较与vtable目标记录不构成所有ABI/生命周期/图像/声音的全面验证。
- 85条签名中6条无任何模块命中（flash1、radar4、MVP observer1）；另有3条广义多命中：DeathCam只在已验证updater内部搜索、Radio emitter再按typed vtable筛选、Panorama style setter有两处call。多命中本身不新增判为故障，亦未对全部RTTI/接口/资源完成穷尽核验。

### 本轮改动与验证边界

仅添加本轮诊断脚本/报告、当前hash的分析库并追加此记录，保留原有未提交内容；未修改功能代码、未构建/安装测试DLL、未提交/推送，未启动/关闭/重启或控制游戏。无新增用户可见效果、feature或默认值变更。后续适配应先完成上述身份/布局核验，再做Release构建、实际加载hash和demo的off/on/re-enable、切人/seek/菜单输入及画面声音验证；本轮不宣称这些运行测试已通过。

保存补记：IDA open/save的前台请求分别返回300秒超时；open之后list/warmup/survey/定向分析均成功。save后实际生成 `client.i64`（621200817 bytes，文件头 `IDA2`），同步器finalize成功，未重新打开该库验证完整性；原生反编译和PE证据另行保存。其余四个模块的输入副本与复核hash保存于本轮 `modules/` 和 `input-verification.json`。当前HLAE安装DLL重新读取SHA-256为 `6A4D05D922D79E604E2311B5645E7A753A6800ACE9891DD24458A4CDD0E6B531`，本轮未改动该文件；磁盘hash不是实际运行进程加载模块的测量。

### 同日适配：用户授权修复并替换DLL

安装client完整hash复核仍为本节 `659940c1db362ed981d1df10637cec8c2a3d99409b77fc3b73e435e4649e36b0`。以下均为该hash的RVA，analysis VA = `0x180000000 + RVA`。前述“仅诊断”记录保留为修复前状态，本段记录后续代码及验证。

- `MirvPovHud.cpp::MirvPovHud_ResolveFlashContexts`更新per-view新栈位移和r15编码，继续验证compact/per-view的E8同到 `0xCE2420`，不改变原生闪光/HE的声音阈值。`MirvPovRadar.cpp::ResolveRadarNativeTargets`更新四条签名；relation call改为新序列 `+8`（`0xEBEC08 -> 0x8D9290`），HUD lookup `0xEC7386 -> 0xE81830`、slot `0xEC73BB -> 0xEB4690`，package updater为 `0xEC71C0`。仍保留唯一命中、范围检查和失败回滚；无新增patch或寄存器改写约定。
- BuyMenu仍按完整client hash和schema `0x15EA`门控，image范围更新 `0x299D000`。全部detour迁移：open/close `0xDB7E90/0xDA1100`，refresh/full `0xDBB1E0/0xDBB280`，hover/think `0xDC2530/0xDA9450`，pawn/controller `0x96A8F0/0x96A8B0`，loadout/hover `0x904140/0x9040A0`，buy-bit/purchase/sell `0xC949F0/0xDAA360/0xDAA6A0`，model/select/SetModel `0xDC0D40/0xDB7730/0xE5EDF0`，KeyInput `0xDADA00`。仅调用helpers：symbol `0x17858C0`、item lookup `0x11360B0`、pawn model `0x21C8C0`、inventory manager **`0x8396F0`**、default loadout `0x83C2A0`、acquire/owned `0x8C0CE0/0x8FF0D0`。特别排除初始投票候选 `0x839730`：它不是8-byte manager getter，最终getter由原生loadout调用确认。
- 原生EventOpenBuyMenu注册函数 `0xFE5E0`同时引用名字字符串 `0x1CB5288`、symbol `0x2241704`和creator **`0xDB2AC0`**。creator读取同一symbol、分配32-byte事件、使用UI engine slot33；它仅被Dispatch调用，不hook。UI engine global迁到 `0x2733F40`；cant-afford/cant-buy symbols迁到 `0x25C8834/0x25C8838`；promoted reference迁到 `0x252DE80`。原生刷新helper `0xDADEB0`（仅分析、不新增调用/hook）提供这些global的RIP引用身份；与旧helper差异仅为armor成员 `0x1ECC -> 0x1EC8`。源码不直接读取这两个armor偏移。
- Panorama完整hash更新为上表 `b5028e...cef412`，16项已用vtable函数归一化相同，包括input+24/+32和panel flag访问的getter/setter。保留原有vtable目标校验、未知构建拒绝、暂停帧重应用、weak handle所有权检查、off/on/seek/切人/退出清理及真实purchase/sell隔离；不绕过兼容性门槛。
- Radio和KillReward不再用会误识别的独立LEA/RET签名。改由唯一原生music `call -> cmp [rax+0x82] -> jnz`上下文解E8，核对getter在text内及LEA/RET入口；实际目标必须为 `0xD36BC0`，对象 `0x223F840`。已有临时门控保存/恢复改读写 `+0x82`，保留SEH/finally恢复和原生透传。KillReward限定formatter邻近范围的compare/JNZ更新为disp32编码，patch从compare `+7`开始，仍只覆盖六字节条件跳转；不使用另一个对象 `0x223F940`。
- MVP只读观察器同步新gate、music `+0x309` submit及诊断读取 `+0x82`；仍不清除该音乐门控。核心上下文和原生音量/许可/声音提交透传保持，无新增主动补播。

验证：`static-fix-audit.json`90项当前PE/源码检查通过，85条POV签名全部命中（广义多命中仍由各模块自身上下文筛选），包括flash共同target、雷达scoped helpers、完整BuyMenu地址集合、23组原生函数语义/归一化比较、事件注册身份、four globals、Panorama16槽和正确demo对象。原生armor成员差异在语义比较中显式列明，未把不同函数简单视为字节相同。普通Release `AFX_MIRV_POV_DIAGNOSTICS=OFF` x64构建通过；购买菜单生产输入控制流35项断言、MVP生产上下文28项断言及observer原参数/返回值/静音状态/异常恢复模拟通过。静态与mock检查不是实际游戏音视频验证。本次无新增效果或控制，沿用现有feature默认值、范围、即时/下次启用语义和清理行为。

部署：确认CS2未运行、已开的HLAE启动器没有加载目标DLL、目标文件可独占打开，并再次核对client完整hash后，用同目录candidate及File.Replace备份并原子替换 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`。旧版备份 `AfxHookSource2.dll.backup-20261015-114810-client-update`，旧版/备份SHA-256均为 `6A4D05D922D79E604E2311B5645E7A753A6800ACE9891DD24458A4CDD0E6B531`；新构建/安装SHA-256均为 `F10B3086F5B42536951E75C79AE9C32AC94F5BB4279E2CF928294E206B94CB0F`。详细记录及受保护的部署脚本为本轮 `installation.json/install.ps1`。未启动、关闭或重启CS2/HLAE，未提交/推送；本次仅验证磁盘部署，实际加载和demo画面/音频待用户回放确认。

## 2026-10-15：恢复顶部 HUD 当前 POV 目标高亮

### 构建身份与原生证据

用户在上述更新适配后反馈观战对象的HUD高亮消失。当前安装/分析client完整SHA-256仍为 `659940c1db362ed981d1df10637cec8c2a3d99409b77fc3b73e435e4649e36b0`，image base `0x180000000`；以下RVA的analysis VA均为base+RVA。复用该hash的输入做定向PE/Capstone验证，没有把旧build地址当成当前结论。

- 原生builder **`0xEBA820`** 在 `0xEBA96B` 读取player state **`+0x17`**，取最低位后七次翻倍，与其它行状态组合，于 `0xEBA9F7`写output **`+8`**的flags；因此对应mask **`0x80`**。builder input总大小仍使用已核验的 `0x38`。它属于已有TeamHealth detour，未新增hook。
- 原生通用avatar发布函数 `0xECC1F0` 在 `0xECC455` 引用当前字符串 **`Avatar__SpectateTarget`**（`0x1CDF620`），经symbol interner建立槽 **`0x25D77CC`**；`0xECCD53/0xECCD59/0xECCD67`读取本次解包的布尔值、同一symbol，调用panel vtable **slot160 / +0x500**设置class。此通用发布器、symbol helper和class setter均未新增hook，也未由POV直接调用。
- 已有equipment presenter **`0xECD9A0`**的四处pawn上下文（连同builder）仍到 `0xC7C160`；builder `+0x4B`和presenter `+0x107`仍共同调用controller slot resolver **`0xACAC70`**。该resolver仅被调用，返回controller；不能拿pawn替代controller。presenter的row结构与builder输入不同，本次只读取两者共同的开头slot，绝不把presenter row+0x17当成builder状态字段写入。

### 源码缺口与修复

`MirvPovTeamHealth.cpp::New_BuildPlayerData`此前无条件将副本的 `playerState+0x17`清零，当前POV行也被清除。此为确定的源码缺口，且新旧builder/presenter的归一化比较相同，不是此次游戏更新的签名失配。未采集用户原失败场景的实际callback/截图，不把代码结论扩大为已实机复现的唯一原因。

本次新增 `IsPovHighlightTarget`：使用当前controller和原生slot resolver做类型/队伍验证及对象身份比较，仅当前有效T/CT POV行返回true；不缓存entity或panel指针。builder仍只修改0x38-byte输入副本，把副本+0x17设为当前目标状态，完整调用原生builder，原输入和敌方血量清理保持。presenter对缓存output+8仅改mask0x80，使切人、seek及控制切换后不必等待新的player-state构建。敌方health+0x0C清零、原生health变量补发、武器字符串和其它flags均保持。初始化增加builder+0x14B读字段和+0x1D7写flags的原生指令校验，布局不符拒绝安装。

新增普通Release独立控制 `mirv_pov_debug_feature hud_target_highlight 0|1`，默认1、依赖 `teamhealth`及主POV。0只去掉POV目标高亮，1恢复，不关闭敌方血量隐藏或其他HUD效果。registry配置立即更新active，显示随原生HUD刷新更新；通用class发布位于equipment回调之前，已有缓存行的修正可能在后一轮原生发布反映，不宣称命令返回时同步重绘。偏好在主POV启停间保留。关闭主POV/TeamHealth后现有hooks完整透传，原生状态重建/发布恢复原生观战标记；无独立持久面板状态或跨seek指针需要释放。中英文README同步记录命令、默认、依赖及清理语义。此前适配段“无新增效果或控制”仅描述前一轮兼容修复，本轮新增控制单独记录于此。

### 验证边界

证据目录 `diagnostics/pov-highlight-20261015/`。`builder.asm/presenter.asm`和 `static-audit.json`记录当前hash、唯一入口、7次翻倍/0x80映射、四处pawn calls、controller resolver和原生class的symbol/setter链；`control-test.log`41项生产函数提取模拟通过：当前/其它队友/敌方行、输入副本不改原值、武器和其他flags保留、敌方health仍隐藏、缓存行目标切换/清理、off/on/re-enable、主POV及teamhealth停用透传、无效slot/目标/异常lookup等。普通非诊断Release x64构建与 `git diff --check`通过；独立只读源码复核未发现代码问题。没有启动/关闭/重启/控制游戏，未实际截取高亮显示或验证暂停场景刷新频率；mock/native指令证据不是最终屏幕呈现验证。

部署：再次确认CS2未运行、HLAE未加载目标DLL且目标可独占打开，复核安装client hash后原子替换既有HLAE路径。旧版备份为 `AfxHookSource2.dll.backup-20261015-121014-target-highlight`，备份SHA-256 `F10B3086F5B42536951E75C79AE9C32AC94F5BB4279E2CF928294E206B94CB0F`；新构建/安装SHA-256均为 `72C6BF3FAAF6B291E45BF43854EDC9F9642CDCBB868CFB26EC90107E339D44E0`，见本轮 `installation.json`。未启动/关闭/重启游戏或HLAE，未提交/推送，实际显示仍待用户回放确认。

命名整理：按用户要求，注册表、调用点、中英文README及验证脚本统一使用 `hud_target_highlight`，不保留旧名称别名，效果及默认值不变。当前PE静态验证、命名一致性、Release x64构建、DLL内新名称存在/旧名称不存在和diff检查通过。确认游戏未运行后再次备份/原子替换，备份 `AfxHookSource2.dll.backup-20261015-122829-hud-target-highlight` 保存上述72C6版；新构建/安装SHA-256均为 `E91C8D0840D100288B9E24310370597A2BB9903842A923DC403272341A21A638`，见 `installation-rename.json`。未控制游戏生命周期。

## 2026-10-16：购买菜单人物预览的录像手套（实现待实机确认）

### 构建身份与游戏原生逻辑

同步器重新读取本机安装client，完整SHA-256仍为 `659940c1db362ed981d1df10637cec8c2a3d99409b77fc3b73e435e4649e36b0`。复用 `diagnostics/client-dll-analysis/659940c1db362ed9/client.i64`，本轮已成功重新打开该数据库；未覆盖基线。image base为 `0x180000000`，下列为该完整hash的RVA，analysis VA=base+RVA。证据保存在 `diagnostics/buymenu-gloves-20261016/`。

原生BuyMenu model函数 `0xDC0D40`（已有detour）先检查pawn/preview和武器item缓存；发生装备重建时从inventory manager `0x8396F0` 返回对象的 `+338128` 取观看者库存接口，以virtual slot8查询team的agent槽38和gloves槽41。`0xDC14E6`查询槽41，`0xDC1562`通过preview virtual slot108 / +864提交手套item ID字符串。该接口绕过现有 `ObservedLoadout` 的controller inventory服务，因此现有“人物模型已换为被观察者”不代表手套也来自被观察者；本轮没有采集原失败场景的实机调用，不把静态来源缺口描述成已复现的唯一运行时原因。

`SetPlayerModel` / `0xE5EDF0`仍先于装备提交；该函数在需要换模时清空预览装备列表。单独再设置一次人物模型会影响武器等附件，不适合用来补手套。通用装备提交入口为 `0xE3DA70`，不是手套专用setter；`0x119B620`判断的是 `pet`，不能用来识别手套。上述两个函数仅分析，本轮不hook、不主动调用。

原生 `CUI_Player3dPanel` 的单人物更新函数 **`0xE68560`** 支持slot `0 <= slot < preview+2288`，通过 `preview+2296`的160-byte records和record+0完整entity handle找到 `csgo_previewplayer`，record+144表示生成尚未结束。装备字符串解析得到item后，`0xE68B8D`调用 **`0x119B150`**识别 `clothing_hands`并排除默认定义；`0xE68BAC`调用 **`0x1185620`**比较初始化状态、definition和拆分item ID；不一致时 `0xE68BBF`调用 **`0x1184AD0`**的原生 `C_EconItemView`赋值，将完整物品复制到preview pawn **`+0x1770`**，随后 `0xE68BC4`写 **`+0x176D=1`**要求重新应用手套。无有效手套时 `0xE68C24`调用 **`0x119B0B0`**清空preview物品，再置相同reapply flag。实际材质/模型渲染在后续游戏更新发生，调用成功不是画面确认。

原生赋值helper复制definition、item ID及动态属性（包括 `+0x208/+0x280`的属性列表），而非只按definition重新创建默认item；有自管缓冲区和属性对象，不能用memcpy替代。schema descriptors独立确认 `C_CSPlayerPawn.m_EconGloves=0x1770`、`m_bNeedToReApplyGloves=0x176D`、`m_nEconGlovesChanged=0x1D20`。definition/initialized在物品内的 `+442/+488`亦由当前原生路径确认，未沿用旧dumper常量。

### POV实现、控制与清理

`MirvPovBuyMenu.cpp::UpdatePreview`新增一个detour **`0xE68560`**：完整调用原生更新，再仅对当前模拟菜单的预览人物应用被观察玩家的手套。它可在BuyMenu的 `g_Depth`作用域以外被原生延后调用，因此用实时菜单weak handle及menu+520指针确认所有权，不扩大全局local-player替换范围。另在 `MirvPovBuyMenu_Update`的非tickChanged路径同步，覆盖暂停帧、同item ID但revision变化和预览实体晚到的情况。

`SyncPreviewGloves`重新解析源pawn完整handle，检查preview record、生成状态、目标handle序列号、`csgo_previewplayer`类名及source!=target。只读录像pawn，将完整手套物品通过上述原生赋值helper写入专用preview pawn，标记原生reapply；不改录像pawn、controller库存、武器或购买状态。原生比较helper用于稳定帧去重，revision或源/预览实体变化强制更新；原生preview更新返回后重新复制可避免观看者装备覆盖。源无效/默认手套时清除预览中的自定义手套，防止继承观看者皮肤。仅保存handle、revision和布尔状态，不缓存跨帧item或entity指针。

上述赋值/比较/清空/手套分类helpers均**未hook，仅调用**；只有单人物更新新增detour。既有完整client hash门控保持；三个新增schema字段必须同时与当前原生布局吻合才安装该detour，缺失或不符只禁用手套效果，不关闭现有购买菜单。未修改Panorama ABI门控。

新增普通Release共享控制 **`mirv_pov_debug_feature buymenu_gloves 0|1`**，默认1，需主POV、`buymenu`模块、用户级 `mirv_pov_buymenu 1`和demo。0同步执行原生预览装备更新以恢复原生饰品；1在预览实体就绪时执行原生更新并应用录像手套，暂停时同样生效；生成尚未完成则随原生回调/后续Update生效，不承诺命令返回即完成材质渲染。`all`包含相同重应用。偏好在主POV启停间保留，promo、layout、input与武器等效果不随之关闭。

原生close之前，`RestorePreviewGloves`按记录的preview handle调用原生更新恢复其原生装备状态；随后清空缓存。Reset、目标切换、seek、demo/level teardown、关闭菜单模拟及主POV均沿现有close/reset路径恢复；若布局已销毁则不解引用旧panel。`mirv_pov_buymenu_status`新增supported/enabled、copy/clear累计数及当前源definition/initialized/revision，用于区分“未命中预览”和“录像没有有效物品”。

### 验证与边界

`verify.py/static-audit.json`的22项检查通过，直接对当前hash PE核对上述四个原生调用、三个schema descriptor、手套/预览类、槽41、动态属性赋值和生产接入。`test.py`提取生产预览同步、所有权、原生更新包装和重应用函数，在原生边界使用mock；51项断言覆盖完整物品传递、源只读、武器字段保留、稳定帧去重、同ID revision变化、off/on/re-enable、主POV/module/demo门控、暂停路径、目标切换、默认/缺失手套、异步生成、错误类/失效handle/同一source-target拒绝及close恢复。该测试不执行真正的原生属性赋值或渲染，不能证明材质已显示。

首次直接MSBuild遭遇继承环境的PATH/Path重复键；本轮build.py仅为子进程合并环境键，普通 `AFX_MIRV_POV_DIAGNOSTICS=OFF` Release x64构建通过。实际回放、手套皮肤/磨损、暂停显示、切人/seek与完整开关矩阵仍待实机验证。录像若没有记录有效的手套属性，无法从默认物品推导原皮肤；此时按设计保留探员默认手套。未启动、关闭或重启CS2/HLAE，未提交或推送。

部署：2026-10-16 22:11:56（Asia/Shanghai）重新确认CS2未运行、已开的HLAE启动器未加载目标DLL且目标可独占打开，复核安装client完整hash后，备份并原子替换 `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`。备份 `AfxHookSource2.dll.backup-20261016-221156-buymenu-gloves` 的SHA-256与旧版一致为 `E91C8D0840D100288B9E24310370597A2BB9903842A923DC403272341A21A638`；新构建/安装SHA-256一致为 `E409364686DDBBF6DD4C824AAE5010C5D71E7C97654ED193C364A2AD44A5CC08`，见本轮 `installation.json`。仅验证磁盘部署；未控制CS2/HLAE生命周期，未验证实际加载或手套渲染。独立只读源码复核未发现明显问题，不替代上述实机验证。

2026-10-17提交整理：补充 `OwnsGlovePreview(nullptr)` 的拒绝条件，避免菜单已存在而preview尚未创建时切换开关读取空地址。新增空preview拒绝和此时切换开关不调用原生更新的两项断言，上述测试现为51项；Release x64、22项PE静态核验和diff检查通过。13:39:55（Asia/Shanghai）确认CS2未运行、HLAE未加载目标后备份并原子替换；新构建/安装SHA-256一致为 `CE85598B2B5810F234EEDC541C807CEE8ED7F48952DEDB3CA61F5E69640E9170`，旧E409版备份为 `AfxHookSource2.dll.backup-20261017-133955-buymenu-gloves-null-guard`，记录在 `installation-commit.json`。未控制游戏生命周期，实际回放验证边界保持。
