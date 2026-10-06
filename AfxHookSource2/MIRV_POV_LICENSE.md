# mirv_pov license scope

The `mirv_pov` feature implementation is licensed under the GNU Affero General
Public License version 3 only (`AGPL-3.0-only`). The license text is available
at `../LICENSE-AGPL-3.0.txt`; the provenance copy is preserved at
`../LICENSES/MulNX-AGPL-3.0.txt`. See `../LICENSE` for the repository's mixed
license scope.

This scope includes:

- source files in this directory whose names begin with `MirvPov`;
- `mirv_pov`-specific code in shared integration files, including command,
  entity, event, frame-stage, Panorama, rendering, and lifecycle integration;
- modifications and generated code needed to build and operate those parts as
  one feature.

Code in this repository that is separate from the `mirv_pov` feature remains
under its existing license, normally the AdvancedFX MIT license or the license
declared by the applicable third-party component.

Because the distributed `AfxHookSource2.dll` combines `mirv_pov` with the rest
of AfxHookSource2 into one binary, that combined binary is conveyed under
`AGPL-3.0-only`. This does not change the original license of separable
AdvancedFX or third-party source material.
