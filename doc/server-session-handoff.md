# Handoff: MCGalaxy server session ← ClassiCube client session

*Written by the client session for the Claude session working on
`UmbreoClaw/mcgalaxy` (branch `survival-support`). It tells you exactly what the
client now does, what was verified against your code, and what to build next.
Read `doc/networking-plan.md` (§25 wire format, §20 gating) and
`doc/survival-handshake.md` alongside this.*

## 0. Setup: read the client from your session

Your GitHub scope is the mcgalaxy repo, but this repo is public — clone it
read-only for reference exactly like the client session did with yours:

```
git clone --depth 1 --branch survival-test \
    https://github.com/UmbreoClaw/ClassiCube /tmp/classicube_ro
```

`src/SurvivalNet.h` is the wire contract; `src/SurvivalNet.c` is the reference
implementation of the client side. **Refresh your `doc/survival-support/reference/`
snapshots** — the client's `networking-plan.md` §25 changed (see §2 below).

## 1. What the client now implements (your phases 0 + 2 are fully consumed)

- **Capability**: `SurvivalTest` v1 CPE ext negotiated; `Server.SupportsSurvival`.
- **Activation**: `SURV_HELLO` applier — mode byte flips the client between
  classic / c0.30-survival / Indev **per map**, flags byte sets
  enhanced/creative/pvp/deathDrops. Mode 0 (your `/Survival` live refresh)
  deactivates mid-map. Activation clears on every map change (no `/goto` leaks).
- **Sim handover**: with activation up, `SurvivalNet_ServerDriven()` is true and
  the client stops running ALL local survival mutation: damage (fall, lava,
  fire, drowning — one central gate in `SurvivalTest_Damage`), mob spawner+AI,
  drops, arrows, paintings, TNT, furnace smelting, day/night clock advance,
  eating, tool wear, container GUIs, survival inventory UI, void death,
  farmland trampling. The client renders, predicts nothing, and sends intents.
- **`SURV_WORLDINFO`** (your v1 single-byte layout) → Indev out-of-bounds
  horizon planes.
- **`SURV_HEALTH`** → HUD + presentation: decrease plays the hurt tilt/sound;
  0 shows the death camera + Game Over screen; a rise while dead revives and
  removes the screen. **No local inventory drop on death** — drops are yours.
- **`SURV_TIME`** → `worldTime` drives the genuine Indev celestial day/night
  (sun/sky/fog colour scaling). Your `skyLight` ramp byte is ignored by this
  client — don't spend effort refining it for us.
- **Intents**: `SURV_RESPAWN`, `SURV_HELD_SLOT` (sent automatically on hotbar
  change), `SURV_DROP_ITEM` (Q key) are live today — your logs should show them.
  `ATTACK`/`USE_ITEM`/`SLOT_CLICK`/`RESULT_CLICK`/`CONT_CLOSE` senders exist and
  get wired as you stream the state they act on.
- Hack permissions: in MP the client defers entirely to your `HackControl`
  packets (it no longer force-sets fly/speed locally, even in creative).
  **Send `HackControl` fly/speed-allowed to creative sessions** so genuine
  creative flight works — resolve it from the same per-session creative
  decision as HELLO's bit1 so they never disagree.

## 2. Wire-contract corrections found while matching your code

- **`SURV_HEALTH` score is i32 BE** (your `SendHealth` bytes 2–5). The client
  and the plan's §25 now say i32; early §25 drafts said i16 — if your
  `reference/networking-plan.md` snapshot still says i16, re-snapshot.
- **`SURV_WORLDINFO` v1** single-byte heights layout (your `SendWorldInfo`) is
  now recorded verbatim in §25 next to the planned fuller int16 revision. When
  you upgrade to the fuller layout, **bump the SurvivalTest ext version**.
- `SURV_HELLO` flags byte matches your `HelloFlags` exactly
  (bit0 enhanced, bit1 creative, bit2 pvp, bit3 deathDrops).

## 3. Server TODOs the client's behavior now makes visible

1. **Death-screen dwell** (you already have this planned): `OnPlayerDied`
   currently sends `SetHealth(0)` then `SetHealth(MAX)` back-to-back, so the
   client's death screen appears and is revived away within a tick or two.
   Genuine flow: hold health at 0 (client shows Game Over + death camera),
   respawn only when the client sends `SURV_RESPAWN` (or after a timeout for
   safety), then send health 20. The client already handles exactly that state
   machine — its MP death screen shows a single **Respawn** button that sends
   `SURV_RESPAWN` and waits for your authoritative revive.
2. **Void death on floating maps**: the client no longer kills the player
   below the world in MP — your hazard detection owns dying. Floating Indev
   maps are bottomless; make sure your fall/void handling covers y below 0.
3. **Per-map world time**: your v1 clock is global; Indev worlds each keep
   their own `TimeOfDay` (it round-trips through the `.mclevel`).
4. **Burning state**: the client doesn't fake the on-fire overlay in MP and
   you can't signal it yet — consider a flag when you do mob/fire work
   (`SURV_MOB_STATE` has an onFire bit for mobs; the player needs a carrier,
   e.g. a reserved bit or a small `SURV_PLAYER_STATE` message — bump the ext
   version if you add one).
5. **Custom blocks on Indev maps** (your phase 1): until you send
   `BlockDefinitions` for the Indev ids (torch 50, fire 51, sources 52/53,
   chest 54, gears 55, diamond 56/57, workbench 58, furnace 61/62, farmland
   83/84, crops 85–92, wall torches 94–97 — the authoritative table is
   `IndevBlocks_Define` in `src/IndevTest.c`), those ids render as CPE
   defaults. The client does NOT locally define blocks on server maps.

## 4. What to build next (recommended order)

1. **Death dwell + `SURV_RESPAWN` round-trip** (small, completes phase 2
   end-to-end against the real client).
2. **Phase 3 mob streaming** (`0x10–0x13`) — the client has the full Indev mob
   render/animation stack ready to puppet; it needs spawn/move/state/despawn
   and will handle interpolation client-side. Positions: int16 fixed-point
   (coord × 32), yaw/pitch as uint8 (deg × 256/360), per §25.
3. **Phase 4 inventory** (`0x20–0x25`, `0x50`) — the client's survival
   inventory UI re-enables in MP once you stream it; `SLOT_CLICK`/`RESULT_CLICK`
   /`CONT_CLOSE` senders are already written.
4. **Phase 5 drops** (`0x30–0x32`) — `SURV_DROP_ITEM` intents already arrive.

Integration testing: the client session verified everything by build + code
audit; the natural end-to-end check is your CLI server on `SurvivalMode=Indev`
with this client connecting — you should see HELLO/WORLDINFO/TIME/HEALTH chat
lines on the client and HELD_SLOT/RESPAWN intents in your debug log.
