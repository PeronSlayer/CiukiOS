# F5 acceptance runner evidence

Status: **not_run**. One workload edition, scene, profile and canonical image
SHA-256 per record. Use the whole-title checklist in
[f5-title-gate-template.md](f5-title-gate-template.md) alongside this tier record.

| Identity | Value |
| --- | --- |
| Tier / workload / edition / scene | pending |
| Image SHA-256 / build manifest SHA-256 / run ID | pending |
| Original archive URL / size / SHA-1 / reviewed SHA-256 | pending |
| Licence class / bundled terms / distribution verification | pending |
| Fixture input tree identity / media hashes before and after | pending |
| Script path / SHA-256 / qualified reference run | pending |
| Profile / machine / CPU / RAM / audio backend | pending |
| Result / serial / screenshot artifact paths | pending |

| Phase | Required observations | Result |
| --- | --- | --- |
| Install | Installer process start/exit; default choices; installed files against verified manifest; registry/link inventory; menu entry; no system writes; installed/desktop screens | not_run |
| Start | Launch kind and decoded path; process and map/imports/TLS/entry records; menu region within deadline | not_run |
| Play | At least 180000 guest ms; changing same-region frame hashes and increasing scene counters; distinct game frames/presents; FPS/sample counts; stream open; qualified non-silent audio/reference; input IDs and latencies; peak commit | not_run |
| Focus (selected three) | Ten Alt+Tab round trips; independent reference screen checks; no corruption | not_run |
| Exit | Documented process/kernel exit codes; desktop region; six ledgers restored within 2000 guest ms | not_run |
| Uninstall (when registered) | Original uninstaller; key/menu removal; independent residue inventory | not_run |

Record segment guest milliseconds and instruction counts separately from host
monotonic timestamps. Input QMP acknowledgements are host dispatch receipts,
not guest input latency. Each screenshot observation records the checkpoint,
host timestamp, region, reference pin, observed region/full-frame hashes,
tolerance and bad-pixel count. Pixels outside the progress region cannot prove
progress. A plain ISO or attached mixed-mode data track supplies no CD-DA proof.

The installer and title tiers are separate cold boots with separate overlays;
they do not preserve an installation between runs. A title fixture is the
lead's reference-installed tree. Passing these separately does not demonstrate
an original installer-to-Start-menu-to-game lifecycle on one writable overlay.
Keep that whole-title gate open until the lead supplies that evidence.

Verdict: **pass / fail / not_run**, with exact failed and missing checks.
A guest PASS alone cannot pass a tier. Reference-pending input, missing screen
pins, absent/unselectable probes and absent fixtures prevent execution.
Timeouts and explicit bad evidence fail; missing independent evidence is not_run.
The runner keeps result/serial on success and removes fixture media/screens;
failed/not_run runs retain bounded evidence under build/test-runs. Do not copy
private payloads into this document, the canonical image or a release.

Lead disposition: original driver/reference qualification, conditional
uninstaller policy, track-aware transport, audio reference capture, final
static/dynamic census and hardware Q7 timing/threshold reconciliation remain
explicit. Ten-minute whole-case limits and memory/swap caps stay unchanged.
