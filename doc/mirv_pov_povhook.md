# mirv_pov POVHook pipeline

This implementation is based on the native transaction design in
[CS2-demo-live-hud](https://github.com/DrEAmSs59/CS2-demo-live-hud). It does
not use the POV implementation from the fork branch.

## Reference implementation

The reference project keeps CS2's demo playback path intact. It does not
replace the demo camera, network/entity snapshot, or Panorama UI. Instead it:

1. Reads the followed player from C_HLTVCamera's primary target. This is
   important for HLTV demos because m_hObserverTarget can remain invalid.
2. Publishes pawn, controller, slot, team, and generation as one immutable
   POV snapshot. A seqlock-style reader prevents a native callback from seeing
   half of one player and half of another.
3. Defines explicit pov::Scope domains for radar, team counter, money, voice,
   communications, view effects, HUD presentation, overhead, sound, and combat
   feedback. Slot-to-local identity adapters are effective only while a
   complete native transaction owns the scope.
4. Calls the original native function unchanged inside the scope. For the
   shared TeamCounter broadcast predicate and engine-to-client IsPlayingDemo
   virtual, it installs temporary vtable adapters that return the live value
   only while the matching native transaction is active. The game remains
   responsible for radar geometry, team filtering, voice identity/timeout,
   damage direction, death panel state, render ordering, and Panorama updates.
5. Pins the followed snapshot for death feedback so a camera cut to the killer
   cannot change identity halfway through a death transaction.
6. Matches player_hurt/player_death through the event's player entity, the
   reference player identity checks, and the event/pawn slot before entering
   combat feedback. A PlayerPawn event-listener vtable adapter applies the
   same gate; unrelated events call the original path unchanged.
7. Requires both client.dll and engine2.dll PE fingerprints and validates
   every fixed-RVA prologue. Any mismatch aborts installation and rolls back all
   entries already changed.

The reference uses near x64 trampolines, so the five-byte relative entry jump
can reach a local stub while the stub uses an absolute jump to the replacement.
Vtable and call-site adapters use compare-and-swap or expected-byte checks.

## HLAE port

The port is in:

- AfxHookSource2/MirvPovContext.*: snapshot, generation, scope, and pinning.
- AfxHookSource2/MirvPov.*: PE gates, near trampolines, identity/mode
  adapters, native transaction boundaries, lifecycle, and the mirv_pov command.

Damage-direction follows the reference call-site pattern: the internal client
CALL is redirected to a scoped wrapper, while the audited damage-direction
function itself remains unmodified and is called directly from the wrapper.

The event and presentation compensation follows the reference native-input
boundaries:

- `player_death` is matched to the followed victim through the native event
  entity, userid, controller/pawn, team, and slot checks before the original
  dispatcher enters the combat scope.
- The original `AttackerFeedback` input is emitted for the followed attacker.
  Native `PushNotice` receives translated kill-cash and teammate grenade-radio
  messages; no Panorama or economy state is written by the port.
- `player_hurt` is paired with the native damage-indicator transaction. Death
  events and the six-field last-killer summary are paired in a 120 ms window,
  replaying the native summary under the same pinned combat snapshot.
- The event dispatcher and three radar sound call sites are adapted at their
  existing native boundaries. Exact VSND submissions keep native priority;
  missing footstep, jump, weapon-fire, and weapon-zoom inputs are queued and
  submitted to the native radar sound path. Native radar creation, movement,
  and drawing remain the owner of the ring state.
- Voice receive masks are rebuilt from the followed team roster and written to
  `tv_listen_voice_indices[_h]`. Recorded audio activity is reconciled into
  native `VoiceStatus::UpdateSpeakerStatus`; native speaker identity, timeout,
  level, and HUD rendering remain unchanged.

This port carries the Pipeline V2 native POV core and its native-input event
compensation into HLAE. It does not add a separate launcher or a Panorama/
economy mutation layer; the original CS2 native transactions remain the owner
of their rendering and presentation state.

The command is opt-in:

    mirv_pov status
    mirv_pov 1
    mirv_pov 0

mirv_pov 1 installs the pipeline after Source2 client initialization.
mirv_pov 0 restores all entry points and invalidates the snapshot.

The supported fingerprint is the reference build dated 2026-08-13:

    client.dll  timestamp 0x6A7CE4FB  SizeOfImage 0x027B8000
    engine2.dll timestamp 0x6A7CE4F8  SizeOfImage 0x00962000

Unknown builds are refused. Every fixed address has a prologue or expected
call/vtable-byte check, and installation is transactional: a mismatch restores
all entries already changed. The DLL must be used with HLAE's normal
AfxHookSource2.dll injection/lifecycle and is intended for local `-insecure`
demo playback only. It is not an online/VAC component.

The CMake post-build step also copies the complete module to:

    mirv_pov_release/AfxHookSource2_mirv_pov.dll

This is a standalone release artifact in the sense that it is a separate
HLAE Source2 module build output. It is not an independent loader: the command
and Source2 lifecycle hooks are part of the HLAE module itself.

The algorithmic structure is adapted from the reference project's MIT-licensed
POV context/detour design. Its copyright and license remain with the reference
project; this repository's existing HLAE license also applies to the port.

## Verification boundary

The x64 Release target and the standalone copy were built successfully. This
environment did not run a live CS2 demo smoke test. The fixed RVAs were checked
against the exact reference-build fingerprint and guarded in the installer; an
updated game build must be re-audited before use.
