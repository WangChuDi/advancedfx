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
