# Open backlog schedule

A durable Grok scheduler (every **30 minutes**, fire-on-create) drives remaining
**agent-executable** MeatEngine work until open ROLLOUT items are done or only
human/risk-deferred work remains.

## Queue (agent order)

| Priority | ID | Item | Status |
|----------|-----|------|--------|
| 1 | B3-water | Underwater water plane | ✅ shipped (shader tint quad) |
| 2 | A6 | Blob shadows under feet | ✅ shipped (projected discs) |
| 3 | C8 | Lite profiler panel (F3) | ✅ shipped |
| 4 | A2-s | Skinned shadow casters | ✅ shipped (`shadow_skinned.*`) |
| 5 | C5+ | Details material/block fields | ✅ shipped |
| 6 | B3b-net | Gravity volumes net sync | open |
| 7 | ARCH | Deeper ARCHITECTURE reconcile | open |
| — | D1 | Binary greedy mesher | **deferred** (measure first) |
| — | D4 | EnTT full migration | **deferred** (large) |
| — | F1/F2 | Interest / lag-comp | **deferred** (scale) |
| — | human | Feel playtests, Linux CI | **human only** |

## Stop condition

When only deferred/human items remain: write `docs/BACKLOG_COMPLETE.md`, update
`NEXT_SESSION.md`, commit, and stop the scheduler (or note stop in NEXT_SESSION).

## Local commands

```text
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
git push origin main
```

Author: MysteryMeat-G. No AI commit attribution.
