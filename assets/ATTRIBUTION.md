# Asset attribution

Every third-party asset bundled in `assets/` is listed here. Licenses are CC-BY 4.0 or
CC0 only; anything else does not get committed. "Modified" notes conversions (format,
scale, retopo, rig) — CC-BY requires indicating changes.

| Asset | Author | Source | License | Modified |
|---|---|---|---|---|
| _(none bundled yet — engine-generated placeholders only)_ | | | | |

Process: `tools/stage_assets.py` copies an asset in, converts if needed, and appends a
row here. An asset without a row (or a row without a human-verified license) fails
`tools/audit_assets.py` and must not ship.
