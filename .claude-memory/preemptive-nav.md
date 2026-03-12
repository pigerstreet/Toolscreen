# Preemptive Navigation Guide (Minecraft Speedrunning)

Technique to find the portal room in strongholds using F3 pie chart.
Discovered by addlama. Primary reference: Mimi's document "A Deeper Look Into Preemptive Navigation".

## How It Works
- Silverfish spawner (portal room) is a block entity → gives unique pie chart spike
- Pie chart path: game renderer → level → entities
- Two values: Block entities (orange) and Unspecified (green)
- Looking toward a rendered silverfish spawner chunk produces a distinct spike

## Required Settings
- 8 render distance
- 50% entity distance
- 30 FOV (limits rendered chunks for precision)
- Thin/small window for even more precision

## Three Pure Readings
1. **Purest Reading** — only silverfish spawner rendered (rare)
2. **Average Reading** — chest rendered in front of spawner
3. **Big Orange** — chest rendered behind spawner (most common, orange >50% of pie)

## Spike Disruptors
- Chunk borders ON (F3+G) — unspecified value too high, unreadable
- Hovering a block close enough to mine — zeroes out unspecified, hides pure/average readings
- Hitboxes ON (F3+B) — slightly increases unspecified

## Scanning Methods

### Hover/Hitbox Method (modern meta)
- Hover blocks + hitboxes ON together
- Simplifies spike to just blockentities value
- Shows big orange + average reading, but pure reading looks like library
- Fix: **Hitbox wiggle** (turn hitboxes off — if spike changes, spawner is present)
- Credit: Chloe + Roobley

### Precision Methods
- **30/thin** — 30 FOV + thin window (fewer chunks, more precise angle)
- **EyeZoom** — tall window for extreme precision (needs OBS projector for pie chart)
- Both help avoid entity interference and narrow down direction

## Other Spawners (Fake Spikes)
- **Skeleton spawner** — higher blockentities (2 dungeon chests), unlikely to confuse
- **Spider spawner** — even higher than skeleton, same logic
- **Cave spider spawner** — tricky, no chests in mineshafts, pure spike ≈ silverfish with 5 front chests
- **Zombie spawner** — IDENTICAL to silverfish spawner spike (nightmare scenario)
- **Beds** — rare blockentities spike, avoid scanning while looking up

## When No Spike Found

### Chunk Culling
- No air on chunk border between you and spawner → chunk not rendered
- Solution: cross a chunk border and rescan
- Hidden rooms aligned with chunk borders can block rendering
- Lower sub-chunks may need to be checked (portal below you)
- Some spikes only visible with 30fov/thin but not 30+thin/eyezoom (two-chunk render path)

### Entities
- Monsters, minecart chests, dropped items affect spike when hitbox rendered on screen
- Solution: use more precise scanning (30/thin or eyezoom) to exclude entities

### Worst Case
- Portal far away + chunk culling = very hard
- Cross chunk borders + rescan with hover/hitbox
- Extensively scan all directions with precise methods

## Optimal Scan Order
1. Scan in portal with hitboxes ON, 30fov or thin (hover blocks in portal frame)
2. Use more precise method (30/thin or eyezoom) — refine direction or avoid entities
3. If no spike: try hitbox wiggle for pure reading detection
4. If still nothing: cross chunk borders and rescan
   - Check hidden rooms aligned with chunk borders
   - Check directions with low C value (few chunks rendered)
   - Try all 4 neighboring chunk borders
   - Follow downward staircases/spirals as last resort

## Preemptive Bug
- Spikes take up ~90% of pie (unreadable)
- Nvidia fix: NVIDIA Control Panel → Manage 3D Settings → Threaded Optimization = OFF
- Linux fix: use Prism/MultiMC with wrapper `env __GL_THREADED_OPTIMIZATIONS=0`
- No known AMD fix
- May also resolve by restarting instance/PC or waiting for world to fully load

## Relevance to Toolscreen
- EyeZoom feature is used for precise ender eye readings
- Thin window mode is used for preemptive scanning
- Mode hotkeys allow quick switching between Fullscreen → Thin → EyeZoom
- Toolscreen eliminates need for OBS projectors or manual window resizing
