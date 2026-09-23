# CS2 探员投掷语音资源核对（2026-09-23）

范围：当前 items_game 的 141 个 customplayer 定义（63 个非 legacy 可交易探员）及 28 个声音库。此为离线资源核对，没有游戏测试。

items_game.txt SHA-256：`bf542a67a0e2fa41a57aabefbcb9e0cd84fab1cc4168d435ea95f4ea4c545bfc`。每个资源文件的 SHA-256 和完整变体表保存在本地 `diagnostics/agent-voice-20260923/resource-audit.json`。

映射顺序：item 的 vo_prefix 优先；缺省时采用 model_player 的模型家族；通过 shared.vrr 的 model 条件转为响应家族（例如 ctm_st6 → seal，ctm_gsg9 → gsg9）。
每一类投掷只从该家族 Radio.Smoke / Radio.Flashbang / Radio.FireInTheHole / Radio.Molotov / Radio.Decoy 原生响应组中选择，并排除当前声音资源中不存在的事件。所有 141 个定义都有五类可用候选。数量不代表实际播放测试。

当前 `pak01_dir.vpk` SHA-256：`d552e2fe7630492f930b2082d6fff141f332166ac4cb1f70e392b1bc7f47dc2f`（仅目录 VPK 身份）。最终 456 个不同候选事件均找到事件定义和所引用的 `.vsnd_c` 文件；这是资源存在性检查，不是解码听音或运行验证。原生响应规则中另有 23 个未在对应事件库找到的引用，见下表，未纳入候选。

## 全部非默认可交易探员

| Definition ID | 探员内部名 | 语音库 | 烟 / 闪 / HE / 火 / 诱饵候选数 |
| --- | --- | --- | --- |
| 4613 | `tm_professional_varf5` | `professional_epic` | 3 / 3 / 3 / 4 / 3 |
| 4619 | `ctm_st6_variantj` | `seal` | 3 / 3 / 4 / 3 / 3 |
| 4680 | `ctm_st6_variantl` | `seal` | 3 / 3 / 4 / 3 / 3 |
| 4711 | `ctm_swat_variante` | `swat_epic` | 4 / 4 / 4 / 4 / 3 |
| 4712 | `ctm_swat_variantf` | `swat_fem` | 3 / 2 / 2 / 2 / 3 |
| 4713 | `ctm_swat_variantg` | `swat` | 3 / 3 / 4 / 4 / 3 |
| 4714 | `ctm_swat_varianth` | `swat` | 3 / 3 / 4 / 4 / 3 |
| 4715 | `ctm_swat_varianti` | `swat` | 3 / 3 / 4 / 4 / 3 |
| 4716 | `ctm_swat_variantj` | `swat` | 3 / 3 / 4 / 4 / 3 |
| 4718 | `tm_balkan_variantk` | `balkan` | 3 / 3 / 3 / 3 / 3 |
| 4726 | `tm_professional_varf` | `professional_epic` | 3 / 3 / 3 / 4 / 3 |
| 4727 | `tm_professional_varg` | `professional_fem` | 4 / 4 / 2 / 4 / 2 |
| 4728 | `tm_professional_varh` | `professional` | 4 / 3 / 4 / 5 / 3 |
| 4730 | `tm_professional_varj` | `professional_fem` | 4 / 4 / 2 / 4 / 2 |
| 4732 | `tm_professional_vari` | `professional` | 4 / 3 / 4 / 5 / 3 |
| 4733 | `tm_professional_varf1` | `professional_epic` | 3 / 3 / 3 / 4 / 3 |
| 4734 | `tm_professional_varf2` | `professional_epic` | 3 / 3 / 3 / 4 / 3 |
| 4735 | `tm_professional_varf3` | `professional_epic` | 3 / 3 / 3 / 4 / 3 |
| 4736 | `tm_professional_varf4` | `professional_epic` | 3 / 3 / 3 / 4 / 3 |
| 4749 | `ctm_gendarmerie_varianta` | `gendarmerie_male` | 3 / 3 / 3 / 3 / 3 |
| 4750 | `ctm_gendarmerie_variantb` | `gendarmerie_male` | 3 / 3 / 3 / 3 / 3 |
| 4751 | `ctm_gendarmerie_variantc` | `gendarmerie_fem_epic` | 3 / 3 / 3 / 3 / 3 |
| 4752 | `ctm_gendarmerie_variantd` | `gendarmerie_male` | 3 / 3 / 3 / 3 / 3 |
| 4753 | `ctm_gendarmerie_variante` | `gendarmerie_male` | 3 / 3 / 3 / 3 / 3 |
| 4756 | `ctm_swat_variantk` | `swat_fem` | 3 / 2 / 2 / 2 / 3 |
| 4757 | `ctm_diver_varianta` | `seal_fem` | 3 / 1 / 3 / 3 / 3 |
| 4771 | `ctm_diver_variantb` | `seal_diver_01` | 3 / 3 / 3 / 3 / 3 |
| 4772 | `ctm_diver_variantc` | `seal_diver_02` | 3 / 3 / 3 / 3 / 3 |
| 4773 | `tm_jungle_raider_varianta` | `jungle_male` | 3 / 3 / 3 / 3 / 3 |
| 4774 | `tm_jungle_raider_variantb` | `jungle_male_epic` | 3 / 3 / 3 / 3 / 3 |
| 4775 | `tm_jungle_raider_variantc` | `jungle_male` | 3 / 3 / 3 / 3 / 3 |
| 4776 | `tm_jungle_raider_variantd` | `jungle_male` | 3 / 3 / 3 / 3 / 3 |
| 4777 | `tm_jungle_raider_variante` | `jungle_fem_epic` | 3 / 3 / 3 / 3 / 3 |
| 4778 | `tm_jungle_raider_variantf` | `jungle_fem` | 3 / 3 / 3 / 3 / 3 |
| 4780 | `tm_jungle_raider_variantb2` | `jungle_male_epic` | 3 / 3 / 3 / 3 / 3 |
| 4781 | `tm_jungle_raider_variantf2` | `jungle_fem` | 3 / 3 / 3 / 3 / 3 |
| 5105 | `tm_leet_variantg` | `leet` | 3 / 3 / 4 / 4 / 3 |
| 5106 | `tm_leet_varianth` | `leet` | 3 / 3 / 4 / 4 / 3 |
| 5107 | `tm_leet_varianti` | `leet` | 3 / 3 / 4 / 4 / 3 |
| 5108 | `tm_leet_variantf` | `leet_epic` | 6 / 3 / 3 / 4 / 3 |
| 5109 | `tm_leet_variantj` | `leet` | 3 / 3 / 4 / 4 / 3 |
| 5205 | `tm_phoenix_varianth` | `phoenix` | 3 / 3 / 3 / 6 / 3 |
| 5206 | `tm_phoenix_variantf` | `phoenix` | 3 / 3 / 3 / 6 / 3 |
| 5207 | `tm_phoenix_variantg` | `phoenix` | 3 / 3 / 3 / 6 / 3 |
| 5208 | `tm_phoenix_varianti` | `phoenix` | 3 / 3 / 3 / 6 / 3 |
| 5305 | `ctm_fbi_variantf` | `fbihrt` | 3 / 3 / 3 / 3 / 3 |
| 5306 | `ctm_fbi_variantg` | `fbihrt` | 3 / 3 / 3 / 3 / 3 |
| 5307 | `ctm_fbi_varianth` | `fbihrt` | 3 / 3 / 3 / 3 / 3 |
| 5308 | `ctm_fbi_variantb` | `fbihrt_epic` | 6 / 6 / 7 / 4 / 3 |
| 5400 | `ctm_st6_variantk` | `gsg9` | 3 / 3 / 3 / 3 / 3 |
| 5401 | `ctm_st6_variante` | `seal` | 3 / 3 / 4 / 3 / 3 |
| 5402 | `ctm_st6_variantg` | `seal` | 3 / 3 / 4 / 3 / 3 |
| 5403 | `ctm_st6_variantm` | `seal` | 3 / 3 / 4 / 3 / 3 |
| 5404 | `ctm_st6_varianti` | `seal_epic` | 6 / 3 / 3 / 3 / 3 |
| 5405 | `ctm_st6_variantn` | `seal` | 3 / 3 / 4 / 3 / 3 |
| 5500 | `tm_balkan_variantf` | `balkan` | 3 / 3 / 3 / 3 / 3 |
| 5501 | `tm_balkan_varianti` | `balkan` | 3 / 3 / 3 / 3 / 3 |
| 5502 | `tm_balkan_variantg` | `balkan` | 3 / 3 / 3 / 3 / 3 |
| 5503 | `tm_balkan_variantj` | `balkan` | 3 / 3 / 3 / 3 / 3 |
| 5504 | `tm_balkan_varianth` | `balkan_epic` | 3 / 3 / 3 / 5 / 4 |
| 5505 | `tm_balkan_variantl` | `balkan` | 3 / 3 / 3 / 3 / 3 |
| 5601 | `ctm_sas_variantf` | `sas` | 3 / 3 / 4 / 4 / 3 |
| 5602 | `ctm_sas_variantg` | `sas` | 3 / 3 / 4 / 4 / 3 |

## 原生响应引用与资源不一致

以下响应名字出现在规则中，但未在本次提取的对应声音事件库内找到。POV 候选表已排除，不能将它们选为保证存在的变体：

- `gendarmerie_fem_epic.ff2_throwing_smoke_04`
- `gendarmerie_fem_epic.ff2_throwing_smoke_05`
- `gendarmerie_fem_epic.ff2_throwing_smoke_06`
- `jungle_fem.aff1_throwing_smoke_04`
- `jungle_fem.aff1_throwing_flashbang_04`
- `jungle_fem.aff1_throwing_molotov_04`
- `jungle_fem_epic.aff1_throwing_smoke_04`
- `jungle_fem_epic.aff1_throwing_smoke_05`
- `jungle_fem_epic.aff1_throwing_smoke_06`
- `jungle_fem_epic.aff1_throwing_molotov_04`
- `jungle_fem_epic.aff1_throwing_molotov_05`
- `jungle_fem_epic.aff1_throwing_decoy_04`
- `jungle_male.afm1_throwing_smoke_04`
- `jungle_male.afm1_throwing_flashbang_04`
- `jungle_male.afm1_throwing_molotov_04`
- `jungle_male_epic.afm2_throwing_smoke_04`
- `jungle_male_epic.afm2_throwing_smoke_05`
- `jungle_male_epic.afm2_throwing_smoke_06`
- `jungle_male_epic.afm2_throwing_molotov_04`
- `jungle_male_epic.afm2_throwing_molotov_05`
- `jungle_male_epic.afm2_throwing_decoy_04`
- `leet_epic.throwing_molotov_05`
- `leet_epic.throwing_decoy_04`

## 控制与边界

沿用现有 `mirv_pov_radio_audio 0/1` 控制，默认 1；关闭立即清空补播队列，再开启作用于后续事件。没有增加新的效果或开关。真实音频仍优先于延迟补播，缺少 demo 包、资源加载状态和队列去重的运行差异未测试。
资源表只描述该安装版本；未来游戏增加或修改探员/语音库时需要重新生成核对。它不保证所有条件、冷却和随机权重完全重现原生响应系统。
