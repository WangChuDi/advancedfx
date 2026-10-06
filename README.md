# HLAE / AdvancedFX — CS2 POV Fixes

English | [简体中文](README.zh-CN.md)

This fork improves first-person CS2 demo playback: POV HUD and radar, player voice routing, grenade radio lines, damage and death feedback, pickup prompts, kill-reward notices, and optional replay of the recorded player's buy menu and scoreboard. It extends HLAE; it is not a standalone client or a guarantee of an exact live-match reconstruction.

**For offline demo playback and video creation only. Never join VAC-protected servers with HLAE: doing so risks a VAC ban. Fast-changing images and flashes may affect photosensitive users.**

## How to use

1. Open this fork's [Releases](https://github.com/WangChuDi/advancedfx/releases) and download **`AfxHookSource2-<12-character-commit>-windows-x64.zip`**. The `*-source.zip` and GitHub-generated Source code archives are source code, not installable binaries.
2. Install a complete, compatible [official HLAE](https://github.com/advancedfx/advancedfx/releases) first, preferably the upstream version used by the selected prerelease. This fork's ZIP contains a replacement DLL and licenses, not a complete HLAE installation.
3. **Close CS2**, back up `<HLAE>/x64/AfxHookSource2.dll`, then extract the binary ZIP into **HLAE's `x64` directory** and replace that DLL. The final path must be `<HLAE>/x64/AfxHookSource2.dll`, without an extra archive folder. Do not extract into the CS2 game directory. Keep the bundled licenses and the rest of HLAE, including `resources/AfxHookSource2/snippets/mirv_script_voice.js`.
4. Launch CS2 through HLAE, play a local demo, switch to the desired player's first-person view, and enter:

```text
mirv_pov 1
```

The main switch is off by default. Use `mirv_pov 0` to disable POV and restore the state/CVars it manages. Buy-menu and scoreboard replay are optional and **off by default**; enable them separately if desired:

```text
mirv_pov_buymenu 1
mirv_pov_scoreboard 1
```

The buy menu follows the observed player's recorded open state, loadout and model; it does not perform real purchases or sales. The scoreboard follows the recorded scoreboard key. Both require `mirv_pov 1`; use `0` to disable each.

## POV features and controls

### Main controls

Where a dedicated command exists, use it instead of a duplicate debug-module switch.

| Command | Feature | Default |
| --- | --- | --- |
| `mirv_pov 0/1` | Main POV switch | Off |
| `mirv_pov_buymenu 0/1` | Recorded buy menu, observed-player loadout and model | Off |
| `mirv_pov_scoreboard 0/1` | Recorded scoreboard-key synchronization | Off |
| `mirv_pov_death_feedback 0/1` | Death presentation and banner, with native death-camera lifecycle handling | On |
| `mirv_pov_voice team/all/enemy/off` | Player voice routing and speaking HUD: POV team / everyone / opposing team / restore original routing | `team` |
| `mirv_voicebanFix 0/1` | Compatibility workaround for automatic communication-abuse muting; not a way to lift server penalties | Off |

Choose one value: do not type the slash-separated alternatives literally. For example, `mirv_pov_voice all` selects all players. Player voice routing does not control agent radio lines. Setting `tv_listen_voice_indices 0` while routing disables POV voice routing and clears its masks.

### Additional effect controls

Effects without a dedicated command use `mirv_pov_debug_feature <feature> 0|1`. These controls are available in ordinary Release builds, despite the word “debug” in the command name.

```text
mirv_pov_debug_feature deafen 0
mirv_pov_debug_feature deafen 1
```

All features below default to **on**, subject to the main switch and any parent controls. “Immediate” applies the setting immediately (UI changes may appear on the next update); “Re-enable” requires `mirv_pov 0` followed by `mirv_pov 1` when POV is already active. You can also configure features before enabling POV. Existing ringing audio can decay naturally after disabling deafening.

| Feature | Effect / scope | Applies |
| --- | --- | --- |
| `hud` | Removes spectator panels/hotkey hints; restores POV health/ammo, buy-zone indicators and top-HUD player-name presentation/animation | Re-enable |
| `teamid` | POV-relative teammate/enemy identifiers, respecting `spec_show_xray` | Re-enable |
| `teamhealth` | Hides enemy health bars in the top HUD and clears stale observer markers | Re-enable |
| `radar` | Competitive teammate colors, red enemies, smoke-visible teammates and red C4 from the CT POV | Re-enable |
| `soundcircle` | POV radar sound circles and footstep/source handling; also feeds some radio audio paths | Re-enable |
| `radio` | Team radio, grenade-throw voice/text and related audio fallbacks | Re-enable |
| `feedback` | Parent module for flash, HE, damage-direction and death-attacker feedback | Re-enable |
| `deafen` | Flash/HE deafening and ringing fallback; requires `feedback` | Immediate |
| `damage_direction` | Damage-direction indicators; requires `feedback` | Immediate |
| `death_screen` | Death-screen color/fade phases; requires death feedback | Immediate |
| `deathpanel_slide` | Death-banner slide/reapplication animation; requires death feedback | Immediate |
| `pickupprompt` | Native POV pickup-target updates and weapon pickup hints | Re-enable |
| `killreward` | POV kill-reward HUD notices, preferring native messages with event-derived fallback | Re-enable |
| `buymenu_promo_hide` | Hides the spurious `PROMOTED Knife` entry; requires buy-menu replay | Immediate |
| `buymenu_layout` | Separates the purchase column from the agent preview; requires buy-menu replay | Immediate |

The death-screen and banner switches control separate effects, not duplicates of the broader `mirv_pov_death_feedback` command. Radio is presented as one user-facing module; internal radio paths are not needed for normal use.

### Advanced setup and troubleshooting

These shared controls normally stay enabled:

| Feature | Scope | Applies |
| --- | --- | --- |
| `voice_script` | Loads HLAE's `mirv_script_voice.js` when POV is enabled | Re-enable |
| `cvars` | Applies POV CVar settings and restores saved values when the main switch is disabled | Re-enable |
| `framestage` | Shared per-frame updates for voice, buy menu, scoreboard, feedback and death lifecycle | Re-enable |

Run `mirv_pov_debug_feature` without arguments to inspect active/configured state. Parent switches have dependencies: disabling `feedback` affects deafening/damage indicators; disabling `radio` or its sound inputs affects radio; disabling `framestage` stops several modules. Leave internal module gates at their defaults when using the dedicated controls above.

## Compatibility and diagnostics

- CS2 updates can change signatures, schema and module hashes. Unsupported builds may refuse to enable a feature; use a compatible build rather than bypassing checks.
- `mirv_pov 1` prints Git/build information. For buy-menu issues, run `mirv_pov_buymenu_status` while the menu is open. `mirv_pov_teamid_debug 1` enables identifier diagnostics; set it back to `0` afterward.
- A grey purchase item may reflect carry limits, per-round limits or game rules, not just money. Grenades are not forcibly enabled based on balance alone.
- The buy-menu spacing fix includes a fixed offset; exact native live layout is not guaranteed for every resolution/UI scale. This is an implementation inventory, not a claim that every demo and switch combination has been tested.

## License

`mirv_pov` and its integration modifications are **AGPL-3.0-only**; a combined DLL containing them is distributed under that license. Separate AdvancedFX and third-party sources retain their original licenses. See the [license overview](LICENSE), [AGPL text](LICENSE-AGPL-3.0.txt), [NOTICE.md](NOTICE.md) and the [POV license scope](AfxHookSource2/MIRV_POV_LICENSE.md). Each release includes a matching `*-source.zip` with corresponding source.

## Original HLAE documentation

<details>
<summary>Expand the original HLAE / AdvancedFX README (upstream downloads, support and building)</summary>

# Half-Life Advanced Effects - advancedfx

## About

Half-Life Advanced Effects (HLAE) is a tool to enrich content creation mainly for the Counter-Strike series.

## Download

**VAC warning**: The HLAE tool is technically a hack, therefore you should only use it for making gaming videos or watching demos. Joining VAC protected servers with HLAE will probably get you VAC banned.

**Epilepsy warning**: This software will cause fast-changing images and colors on your screen.

### Latest release

https://github.com/advancedfx/advancedfx/releases/latest  
Compatible with latest Steam version.

### Past releases

https://github.com/advancedfx/advancedfx/releases

* **Old HLAE versions for specific CS2 versions**  
  https://github.com/advancedfx/advancedfx/wiki/Old-HLAE-versions-for-specific-CS2-versions

* **HLAE 2.153.1** (2024-01-05T19:45Z)  
  https://github.com/advancedfx/advancedfx/releases/tag/v2.153.1  
  Release with AfxHookGoldSrc and related tools compatible with Steam version "steam_legacy - Pre-25th Anniversary Build.
  The latest HLAE release should also work with steam_legacy.

* **HLAE 2.34.5** (2017-07-08T14:48Z)  
  https://github.com/ripieces/advancedfx/releases/tag/v2.34.5  
  Last release with AfxHookGoldSrc and related tools compatible with Steam version before 10th of July 2017.

* **HLAE 2.2.21.14** (2013-02-09T14:16Z)  
  https://github.com/ripieces/advancedfx-prop/releases/tag/v2.2.21.14  
  Release compatible with Steam version from July 2009.

## System requirements

* Microsoft Windows 10 or newer
* Microsoft .NET Framework 4.6.2 or newer
* Genuine Steam version
* a lot of patience

## Tip

Using Steam's offline mode you can avoid retrieving updates that break HLAE during video production.

## Support

Please keep in mind, that even though we accept donations, HLAE is free software and a hobby for most in the HLAE team.
Thus the support we can provide is limited. We encourage you to read the Manual or help each other in the Discord instead.

### English

* **Manual**  
  https://github.com/advancedfx/advancedfx/wiki  
  HLAE wiki with FAQ, tutorials, command lists, and more. 

* **Discord**  
  https://discord.gg/NGp8qhN  
  Live text chat support from HLAE team members and community members.

* GitHub - **Issues**  
  https://github.com/advancedfx/advancedfx/issues  
  For bug reports and feature requests (enhancements).

### 中文 (zh-CN)

 * **HLAE中文站**  
   https://hlae.site  
   This site is run by a third party and one of the HLAE team members is involved.

## Releated tools

For a list of related tools see [related-tools.md](related-tools.md)

## Changelog

Newer changelog entries are replicated on the [releases page](https://github.com/advancedfx/advancedfx/releases).  
Further changelog entries can be found in the changelog XML files included in the download.

## Credits

[CREDITS.md](CREDITS.md)

## License

This repository remains under the AdvancedFX MIT license by default. The
`mirv_pov` feature and its integration modifications are licensed under GNU
Affero General Public License version 3 only (`AGPL-3.0-only`). See
[AfxHookSource2/MIRV_POV_LICENSE.md](AfxHookSource2/MIRV_POV_LICENSE.md) and
[NOTICE.md](NOTICE.md) for the exact scope.

A distributed `AfxHookSource2.dll` containing `mirv_pov` is a combined binary
and is conveyed under `AGPL-3.0-only`. Separate AdvancedFX and third-party
source material retains its original license.

## How to build

[BUILDING.md](BUILDING.md)

</details>
