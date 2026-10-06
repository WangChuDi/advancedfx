# Licensing and provenance notice

## Default license and mirv_pov exception

The repository is licensed under the AdvancedFX MIT license by default. The
`mirv_pov` feature implementation and its integration modifications are the
exception: they are licensed under GNU Affero General Public License version 3
only (`AGPL-3.0-only`). The exact source scope is defined in
`AfxHookSource2/MIRV_POV_LICENSE.md`. The root `LICENSE` explains the mixed
license scope; the full AGPL text is in `LICENSE-AGPL-3.0.txt`, with the
provenance copy preserved in `LICENSES/MulNX-AGPL-3.0.txt`.

A distributed `AfxHookSource2.dll` containing `mirv_pov` combines that feature
with other AfxHookSource2 code in one binary. That combined binary is conveyed
under `AGPL-3.0-only`. Separate AdvancedFX and third-party source material
retains its original license and notices.

## AdvancedFX

This repository is based on AdvancedFX:

- Project: https://github.com/advancedfx/advancedfx
- Copyright: Copyright (c) 2021 advancedfx.org and its contributors
- Original license: MIT
- Preserved license: `LICENSES/AdvancedFX-MIT.txt`

The MIT-licensed AdvancedFX material remains available under its original
license. Its inclusion in this fork does not remove its original copyright or
license notices.

## MulNX_CS2

The `mirv_pov` implementation, including Radar, SoundCircle, and related POV
integration, is distributed on the conservative basis that it may contain or
be based on material from MulNX_CS2. That feature scope is therefore treated
as covered by `AGPL-3.0-only`.

- Project: https://github.com/Co1Swet/MulNX_CS2
- Copyright: Copyright (C) 2026 Co1Swet
- License option selected by this fork: GNU Affero General Public License v3.0
- License source commit: `c4d521f0cff8ec0c62a7904d00495355bf83726c`

This fork is not endorsed, sponsored, or maintained by the MulNX_CS2 author.

## Corresponding source

For every binary release distributed by this fork, the complete corresponding
source is provided as a matching source archive on the same release page:

https://github.com/WangChuDi/advancedfx/releases

The source archive identifies the exact repository commit and recursive
submodule revisions used for the build. No fee is charged for source access.

## Modification notice

This is a modified version of AdvancedFX. The `mirv_pov` implementation,
including its Radar, SoundCircle, HUD, voice, scoreboard, feedback, radio, and
associated integration work, was modified and reorganized in 2026. Git history
records the detailed changes and their dates.
