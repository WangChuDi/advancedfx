# HLAE / AdvancedFX — CS2 POV 修复版

[English](README.md) | 简体中文

本 fork 在 HLAE 上补充 CS2 demo 的第一人称（POV）回放表现：按当前观察玩家还原 HUD、雷达、玩家语音、投掷无线电、受击/死亡反馈、拾取与击杀奖励提示，并可同步录像中的购买菜单和计分板。它不是独立游戏客户端，也不保证与实战画面完全一致；具体效果取决于 demo 数据、游戏构建和所启用的模块。

**仅用于离线 demo 回放和视频制作。不要通过 HLAE 加入 VAC 保护的服务器，否则有被封禁的风险。画面可能包含快速闪光，光敏感用户请谨慎使用。**

## 使用方法

1. 从本 fork 的 [Releases](https://github.com/WangChuDi/advancedfx/releases) 选择需要的 prerelease，下载 **`AfxHookSource2-<12位提交哈希>-windows-x64.zip`**。`*-source.zip` 和 GitHub 自动生成的 Source code 包是源码，不是可直接安装的 DLL。
2. 先安装完整的 [官方 HLAE](https://github.com/advancedfx/advancedfx/releases)，优先使用该 prerelease 所同步的上游版本。此 fork 的 ZIP 只提供替换 DLL 和许可证，不能代替完整 HLAE。
3. **退出 CS2**，备份 `<HLAE>/x64/AfxHookSource2.dll`，把二进制 ZIP 解压到 **HLAE 的 `x64` 目录**并覆盖 DLL。最终路径应为 `<HLAE>/x64/AfxHookSource2.dll`，不要多套一层压缩包文件夹，也不要放进 CS2 游戏目录。保留随包许可证及 HLAE 其他文件，尤其是 `resources/AfxHookSource2/snippets/mirv_script_voice.js`。
4. 通过 HLAE 启动 CS2，播放本地 demo，切到需要的玩家第一人称视角，在游戏控制台输入：

```text
mirv_pov 1
```

总开关默认关闭；`mirv_pov 0` 关闭 POV 并恢复其管理的状态/CVar。需要同步录像中的购买菜单或计分板时，额外开启以下**默认关闭**的功能：

```text
mirv_pov_buymenu 1
mirv_pov_scoreboard 1
```

购买菜单跟随被观察玩家录像中的开菜单状态，并显示其装备配置与模型；不能替录像玩家实际购买/出售物品。计分板跟随录像中的计分板按键状态。将上述命令末尾改为 `0` 可分别关闭；它们都需要 `mirv_pov 1`。

## POV 功能与控制

### 常用开关

有独立命令的功能直接使用该命令，不重复列出对应的 debug 模块开关。

| 命令 | 功能 | 默认 |
| --- | --- | --- |
| `mirv_pov 0/1` | POV 总开关 | 关闭 |
| `mirv_pov_buymenu 0/1` | 同步录像中的购买菜单、被观察玩家装备与模型 | 关闭 |
| `mirv_pov_scoreboard 0/1` | 同步录像玩家计分板按键 | 关闭 |
| `mirv_pov_death_feedback 0/1` | 死亡画面与死亡面板总控制，配合原生死亡摄像机生命周期处理 | 开启 |
| `mirv_pov_voice team/all/enemy/off` | 玩家语音路由与发言 HUD：当前 POV 队伍 / 全部玩家 / 敌方 / 恢复原始路由 | `team` |
| `mirv_voicebanFix 0/1` | 通信滥用标记导致的自动静音兼容修正；不是解除服务器处罚 | 关闭 |

斜杠表示选择一个值，不是一起输入。例如 `mirv_pov_voice all` 表示全部玩家。玩家语音路由不控制角色无线电台词；路由期间将 `tv_listen_voice_indices` 设为 `0` 会关闭 POV 语音路由并清理掩码。

### 其他效果开关

没有独立命令的效果使用 `mirv_pov_debug_feature <feature> 0|1`。虽然名字含 debug，但这些开关在普通 Release 中也可用：

```text
mirv_pov_debug_feature deafen 0
mirv_pov_debug_feature deafen 1
```

下列 feature 默认均为**开启**，但仍受总开关和上层功能控制。“立即”表示配置即时应用（UI 通常下一次更新可见）；“重新开启”表示 POV 已开启时先保存配置，需执行 `mirv_pov 0`、再执行 `mirv_pov 1`。也可先配置后开启总开关。关闭致聋不会强制截断已经播放的耳鸣尾音。

| Feature | 功能 / 作用范围 | 生效时机 |
| --- | --- | --- |
| `hud` | 隐藏观战面板/热键提示，恢复 POV 生命/弹药、买区提示与顶部玩家名字显示/动画 | 重新开启 |
| `teamid` | 按观察玩家修正队友/敌方标识，遵循 `spec_show_xray` | 重新开启 |
| `teamhealth` | 隐藏顶部 HUD 的敌方血条，清理残留观战目标标记 | 重新开启 |
| `radar` | 队友竞技颜色、敌方红色、烟雾内队友可见性，以及 CT POV 的红色 C4 雷达表现 | 重新开启 |
| `soundcircle` | POV 雷达声音圆圈、脚步等声音来源处理；还承接部分无线电声音路径 | 重新开启 |
| `radio` | 队伍无线电、投掷物语音/文字，以及相关音频回退 | 重新开启 |
| `feedback` | 闪光、HE、受击方向与死亡攻击者等反馈的总模块 | 重新开启 |
| `deafen` | 闪光/HE 致聋与耳鸣反馈回退；依赖 `feedback` | 立即 |
| `damage_direction` | 受击方向指示；依赖 `feedback` | 立即 |
| `death_screen` | 死亡染屏/黑屏阶段表现；依赖死亡反馈 | 立即 |
| `deathpanel_slide` | 死亡面板滑入/重应用动画；依赖死亡反馈 | 立即 |
| `pickupprompt` | 当前 POV 玩家的原生拾取目标更新与武器拾取提示 | 重新开启 |
| `killreward` | 击杀奖励 HUD 通知，优先原生消息，缺失时使用事件回退 | 重新开启 |
| `buymenu_promo_hide` | 隐藏多出的 `PROMOTED Knife` 促销项；依赖购买菜单模拟 | 立即 |
| `buymenu_layout` | 调整购买列与人物预览间距；依赖购买菜单模拟 | 立即 |

死亡画面与面板动画属于可分别控制的子效果，并不重复替代 `mirv_pov_death_feedback` 总开关。Radio 只作为一个完整功能介绍，正常使用无需配置其内部细分路径。

### 高级设置与排查

以下共用设置通常保持开启：

| Feature | 作用范围 | 生效时机 |
| --- | --- | --- |
| `voice_script` | 开启时加载 HLAE 自带的 `mirv_script_voice.js` | 重新开启 |
| `cvars` | 应用 POV 所需 CVar，关闭总开关时恢复已保存值 | 重新开启 |
| `framestage` | 语音、购买菜单、计分板、反馈、死亡生命周期等逐帧调度 | 重新开启 |

不带参数执行 `mirv_pov_debug_feature` 可检查实际/已配置状态。上层模块存在依赖：关闭 `feedback` 会影响致聋/受击指示；关闭 `radio` 或其声音输入会影响无线电；关闭 `framestage` 会停止多项逐帧更新。使用上面的独立命令时，让内部模块开关保持默认即可。

## 兼容性与诊断

- CS2 更新可能改变签名、schema 或模块哈希；不支持的构建可能拒绝启用功能。请使用匹配版本，不要移除兼容性校验。
- `mirv_pov 1` 会打印 Git/构建信息。购买菜单异常时，在菜单仍打开的状态执行 `mirv_pov_buymenu_status`；标识异常可临时使用 `mirv_pov_teamid_debug 1`，用完设回 `0`。
- 购买项置灰可能由携带数量、每回合购买上限或游戏规则导致，不只取决于余额；本 fork 不会仅凭余额强制放行投掷物。
- 购买菜单间距修正包含固定偏移，不承诺所有分辨率/UI 缩放与原生 live 布局完全一致。功能列表描述实现范围，不代表所有 demo、所有开关组合都已实机验证。

## 许可证

`mirv_pov` 及其集成修改为 **AGPL-3.0-only**；包含它的组合 DLL 按该许可证分发，原 AdvancedFX 和第三方独立源码保留各自许可证。见[许可范围总览](LICENSE)、[AGPL 全文](LICENSE-AGPL-3.0.txt)、[NOTICE.md](NOTICE.md) 与 [POV 许可证说明](AfxHookSource2/MIRV_POV_LICENSE.md)。每次发布均提供同提交的 `*-source.zip` 对应源码。

## 原版 HLAE 文档

请展开[英文 README 底部的原版文档](README.md#original-hlae-documentation)，查看上游下载、支持及构建说明。
