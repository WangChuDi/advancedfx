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
