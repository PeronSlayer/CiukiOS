# 2026-04-17 DOOM Milestone Roadmap Rewrite

## Context and goal
The roadmap already showed what was done, but the remaining path to the DOOM milestone was still too compressed. The goal of this rewrite is to make the missing work explicit, ordered, and milestone-oriented rather than leaving it as a short immediate queue.

## Files touched
- Roadmap.md
- docs/roadmap-ciukios-doom.md

## Decisions made
1. Kept the existing completed phases intact and expanded only the remaining path to the milestone.
2. Added a dedicated `SR-DOOM-001` track in the main roadmap so the missing work is visible alongside the other sub-roadmaps.
3. Rewrote the DOOM roadmap from a short 6-step queue into a full milestone closure plan grouped by dependency: target freeze, DOS extender path, BIOS/runtime gaps, graphics path, packaging/harness, and playability.

## Validation performed
1. Re-read the main roadmap and the DOOM roadmap after editing to ensure the remaining path is complete and ordered.
2. Kept the rewrite aligned with the current implemented baseline: current DOS/MZ/FAT/video/UI features remain described as complete, while all milestone blockers are listed explicitly as remaining work.

## Risks and next step
1. The roadmap is now more explicit, but it still depends on choosing the exact first DOOM target binary/runtime pair.
2. The next concrete engineering step should be the first real DOS extender slice beyond the current `INT 2Fh AX=1687h` smoke path.