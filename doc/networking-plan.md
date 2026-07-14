# Networking Integration Plan — Multiplayer Indev (and c0.30‑s) over the Classic Protocol

> **Audience:** the next Claude session that will implement multiplayer for this
> fork. Read this whole file before touching any networking code. It is a *plan*,
> not finished work — the previous session did research + design only and made
> **no** network code changes.
>
> **Two repos are in play:**
> - **Client:** this repo — the ClassiCube fork with the Indev / c0.30‑Survival
>   singleplayer simulation (`src/SurvivalTest.c`, `src/IndevTest.c`, `src/IndevGen.c`).
> - **Server:** `https://github.com/UmbreoClaw/mcgalaxy` (a fork of MCGalaxy, C#).
>   It already speaks the Classic protocol + CPE. We modify it, we do **not** start fresh.
>
> **This first networking milestone focuses on Indev.** c0.30‑s comes later; the
> design must leave room for it but you only need to wire Indev end‑to‑end first.

---

## 0. The one rule that governs everything

**Never break vanilla Classic.** A normal Classic/CPE server (or a normal Classic
client) must keep working exactly as today. Concretely:

- All new network behaviour is **gated behind a newly‑negotiated CPE extension**
  (working name **`SurvivalTest`**, see §3). If the peer doesn't advertise it,
  the survival net path is *completely inert* and the client behaves as stock
  Classic (creative, server‑authoritative blocks only).
- Do **not** repurpose or resize existing Classic/CPE opcodes. Everything custom
  rides on the already‑standard **PluginMessages** CPE extension (opcode `0x35`),
  which both sides already support.
- Singleplayer must be untouched. The existing SP simulation (`Server.IsSinglePlayer == true`)
  keeps running locally; the net path only engages for `IsSinglePlayer == false`
  **and** the `SurvivalTest` extension negotiated.
- Gate at runtime, not compile time: `if (Server.IsSinglePlayer) { local sim }`
  vs `else if (IsSupported(survival_Ext)) { networked }` vs `else { stock classic }`.

---

## 1. Why this shape

Indev had **no** multiplayer protocol, so there is nothing to port 1:1. We take:

- **Transport** from **Classic + CPE** (what MCGalaxy and this client already speak).
- **The *set of things* that must be networked** as inspiration from **Beta 1.x**
  (the first survival multiplayer): health/damage, entity spawn/move/metadata,
  inventory windows + click transactions, held item, time‑of‑day, block metadata
  (chests/furnaces), item drops/pickups. We reproduce the *concepts*, not the
  byte formats — the byte formats are ours, carried inside PluginMessages.
- **Authority model** from Beta too: **the server is authoritative.** The server
  generates the world, runs mob AI + physics + health + containers + day/night,
  validates player actions, and streams state to clients. The client renders,
  plays sound, drives the GUI, does light prediction, and sends *intents*
  (move, attack, place/break, use‑item, craft, inventory‑click). This is exactly
  why the two server‑side tasks you're being handed (§8) are "port the Indev
  generator into MCGalaxy" and "add the server‑side checks/handling" — the
  simulation's centre of gravity moves to the server.

The current fork's `src/SurvivalTest.c` / `src/IndevTest.c` are a *complete,
source‑faithful survival simulation in C*. That is gold: it is the reference the
server‑side C# simulation must match, and much of the client code is reusable as
the **client‑prediction + rendering** layer. Do not throw it away — split it.

---

## 2. Background you must have loaded

### 2.1 Classic protocol (what already works)

Client packet handlers live in `src/Protocol.c` (`Classic_*` functions), wired in
`Classic_Reset()` via `Net_Set(opcode, handler, size)`. Opcode names are in
`src/Protocol.h` (`enum OPCODE_`). The base set:

| Opcode | Name (our enum) | Dir | Purpose |
|---|---|---|---|
| 0x00 | `OPCODE_HANDSHAKE` | both | ident / login (padding byte `0x42` ⇒ "I speak CPE") |
| 0x01 | `OPCODE_PING` | S→C | keepalive |
| 0x02 | `OPCODE_LEVEL_BEGIN` | S→C | start gzipped level stream |
| 0x03 | `OPCODE_LEVEL_DATA` | S→C | 1024‑byte level chunk + percent |
| 0x04 | `OPCODE_LEVEL_END` | S→C | dims, finalise |
| 0x05 | `OPCODE_SET_BLOCK_CLIENT` | C→S | player placed/broke a block |
| 0x06 | `OPCODE_SET_BLOCK` | S→C | authoritative block change |
| 0x07 | `OPCODE_ADD_ENTITY` | S→C | spawn player entity |
| 0x08 | `OPCODE_ENTITY_TELEPORT` | S→C | absolute pos/orient |
| 0x09–0x0B | rel pos / ori updates | S→C | movement deltas |
| 0x0C | `OPCODE_REMOVE_ENTITY` | S→C | despawn |
| 0x0D | `OPCODE_MESSAGE` | both | chat |
| 0x0E | `OPCODE_KICK` | S→C | disconnect w/ reason |
| 0x0F | `OPCODE_SET_PERMISSION` | S→C | op/user block‑bypass |

Positions are fixed‑point (5 fractional bits) unless `ExtEntityPositions` is
negotiated. Blocks are 1 byte unless `ExtendedBlocks`.

### 2.2 CPE (Classic Protocol Extension)

Negotiation (see `src/Protocol.c`, `CPE_Tick`/`CPE_ExtInfo`/`CPE_ExtEntry` around
lines 838–860 and `Net_Set(OPCODE_EXT_INFO/…)`):

1. Client login padding byte = `0x42` ⇒ "I support CPE".
2. Server → `ExtInfo` (opcode `0x10`: 64‑byte AppName + `short` count) then one
   `ExtEntry` (opcode `0x11`: 64‑byte ExtName + `int` version) per extension.
3. Client replies with its own `ExtInfo` + `ExtEntry` list
   (`cpe_clientExtensions[]` in `src/Protocol.c:103`).
4. Both sides keep the **intersection**. `IsSupported(ext)` ⇔ `ext.serverVersion > 0`.

The client already declares ~40 extensions (`cpe_clientExtensions[]`), including
the two that matter most here:

- **`PluginMessages` v1** — our transport (see §2.3).
- Plus ready‑made helpers we can lean on instead of reinventing: `HackControl`
  (fly/noclip/speed/jump gating — use it to lock survival players out of creative
  flight), `EnvWeatherType`, `MessageTypes` (status‑bar lines), `EntityProperty`
  (per‑entity rot/scale), `SetHotbar`, `VelocityControl` (server‑applied knockback!),
  `ChangeModel` (mob models), `CustomModels`, `CustomParticles`, `ExtEntityPositions`.

MCGalaxy side (`MCGalaxy/Network/ClassicProtocol.cs`): `HandlePacket` is the opcode
switch; `SendCpeExtensions` / `HandleExtInfo` / `HandleExtEntry` / `AddExtension`
run the negotiation and set `has*` capability flags.

### 2.3 PluginMessages — our custom channel (already on both sides)

This is the backbone. It is a generic, forward‑compatible side channel.

- **Packet:** `[0x35][channel:1 byte][payload:64 bytes]` = 66 bytes fixed.
- **Client send:** `CC_API void CPE_SendPluginMessage(cc_uint8 channel, cc_uint8* data)`
  (`src/Protocol.h:98`, impl `src/Protocol.c:891`). Guards on `IsSupported(pluginMessages_Ext)`.
- **Client receive:** `CPE_PluginMessage` (`src/Protocol.c:1551`) raises
  `Event_RaisePluginMessage(&NetEvents.PluginMessageReceived, channel, data+1)`.
  Subscribe to `NetEvents.PluginMessageReceived` to get `(channel, 64‑byte payload)`.
- **Server:** MCGalaxy already handles `0x35` and raises `OnPluginMessageReceivedEvent`
  (channel + payload). **Verify the server payload width** — the CPE spec is
  **64 bytes**; confirm MCGalaxy reads 64 (one fetch summary said 256 — treat as
  suspect and check `ClassicProtocol.cs`).

**Design consequence:** the whole survival sub‑protocol is a set of message types
multiplexed over PluginMessages. 64 bytes/frame is plenty for a header + a small
struct; larger payloads (full inventory, container contents) are split across
frames with a message‑id + sequence, or sent as several logical messages.

---

## 3. The master gate: a `SurvivalTest` CPE extension

Add **one** new CPE extension, negotiated the normal way, that means "both sides
speak the survival sub‑protocol". Everything else keys off it.

- **Client:** add `static struct CpeExt survival_Ext = { "SurvivalTest", 1 };` and
  put `&survival_Ext` in `cpe_clientExtensions[]` (`src/Protocol.c:103`). Add a
  `cc_bool SupportsSurvival;` to `Server` (`src/Server.h`) set from
  `IsSupported(survival_Ext)` (mirror how `SupportsNotifyAction` etc. are done).
- **Server (MCGalaxy):** advertise `SurvivalTest` in `SendCpeExtensions`, set a
  `hasSurvival` flag in `AddExtension`.
- **Meaning:** if `!Server.SupportsSurvival` (vanilla server), the client never
  enters networked survival — it stays stock Classic. This is the gate that
  satisfies "inactive on classic, active on Indev/c0.30‑s".

**Mode selection after negotiation:** the server tells the client *which* survival
mode this map is (Indev vs c0.30‑s vs off) via an initial `SURV_HELLO` plugin
message (see §6). The client then flips the existing runtime switches
(`SurvivalTest_Enabled`, `IndevTest_Enabled`) — today driven by
`SurvivalTest_Gamemode()` from `options.txt` (`src/SurvivalTest.c:7474`,
`src/IndevTest.c:2401`). Introduce a network override so those flags can be set by
the server in MP instead of by local options. **Keep SP reading options as now.**

---

## 4. Client architecture (this repo)

### 4.1 Where the SP/MP split already lives

`src/Server.c` cleanly separates the two via function pointers on the global
`Server` struct:

- `SPConnection_*` (`src/Server.c:122+`): local, `IsSinglePlayer = true`,
  `SendBlock`/`SendChat`/`SendData` are local/no‑ops.
- `MPConnection_*` (`src/Server.c:253+`): real socket; `SendData` writes to the
  socket; `Tick` reads packets and dispatches via the `Net_Handlers` table.

The survival net layer hangs off the **MP** path only.

### 4.2 Recommended new client module: `src/SurvivalNet.c` (+ `.h`)

Keep all networked‑survival glue in one new file so the simulation files stay
readable and the SP path is untouched. Responsibilities:

- **Subscribe** to `NetEvents.PluginMessageReceived`; if `channel` is a survival
  channel, decode the message and apply it to `SurvivalTest`/`IndevTest` state
  (health, mob table, inventory, container, time, drops…).
- **Send intents**: wrap `CPE_SendPluginMessage` in typed helpers
  (`SurvivalNet_SendAttack`, `_SendUseItem`, `_SendCraft`, `_SendInvClick`,
  `_SendContainerClick`, …). Call these from the existing input handlers instead
  of mutating local state directly, **when in MP survival**.
- **Author‑ity switch inside the sim:** in MP, the client must *not* run the
  authoritative bits locally. Add guards like
  `if (Server.IsSinglePlayer) { run local sim } else { rely on server state }`
  around: mob spawning/AI/damage (`Mob_*` in `SurvivalTest.c`), health changes
  (`SurvivalTest_Hurt`), container tick (`Furnace_Tick`), day/night
  (`Indev_TickDayNight`), block random ticks (grass/leaf/farmland/fire), and world
  generation. Client keeps rendering + prediction; server owns truth.

The wiring points to grep for in `src/SurvivalTest.c` / `src/IndevTest.c`:
`SurvivalTest_Tick` (the 20 Hz tick — gate the authoritative subsystems),
`SurvivalTest_Hurt` / `SurvivalTest_HurtFrom`, `Mob_SpawnerRun` /
`Mob_IndevSpawnerRun` / `SurvivalTest_IndevInitialSpawn`, `IndevTest_OpenContainer`
/ container slot mutation, `IndevTest_TickRandomBlocks`, `IndevGen_*`.

### 4.3 Worldgen in MP

In MP the **server** generates and streams the map over the normal Classic level
packets (`OPCODE_LEVEL_*`) — the client does **not** run `IndevGen`. But Indev
needs data the Classic level stream doesn't carry (water/ground level, theme,
floating vs inland, surroundings, per‑cell metadata for chests/furnaces/farmland/
fire). Send that as a `SURV_WORLDINFO` plugin message right after `LEVEL_END`,
before spawn (see §6). The client applies it to `Env`/`IndevTest_SetSurroundings`
etc. The existing `.mclevel` metadata fields (`src/Formats.c`) are the exact list
of what must cross the wire.

---

## 5. Server architecture (MCGalaxy fork)

### 5.1 Pointers

- `MCGalaxy/Network/ClassicProtocol.cs` — `HandlePacket` opcode switch, CPE
  negotiation, `SendLevel`, `SendBlockchange`, `MakeBulkBlockchange`,
  `SendSpawnEntity`, and the **existing `0x35` PluginMessage handler →
  `OnPluginMessageReceivedEvent`**. Hook survival receive here.
- `MCGalaxy/Generator/MapGen.cs` — generator registry.
  `Register(string theme, GenType type, MapGenFunc func, string desc)` where
  `public delegate bool MapGenFunc(Player p, Level lvl, MapGenArgs args);`.
  Add the Indev generator here (task §8.1).
- Player state / handlers: `MCGalaxy/Player/…` (`Player.cs`, `Player.Handlers.cs`).
- Events: MCGalaxy's plugin/event system (`OnPluginMessageReceivedEvent`, block‑
  change events, player‑move events) is where a **Survival plugin** subscribes.

### 5.2 Where the authoritative simulation goes

Two viable homes; recommend **a first‑party server module/plugin** ("SurvivalTest"
plugin) so it stays isolated and can be toggled per‑level:

- Per‑level survival state: player health/inventory/armor/score, mob list + AI,
  container tile‑entities, farmland/fire/grass random ticks, day/night `worldTime`.
- A 20 Hz level tick that mirrors the C client's `SurvivalTest_Tick`.
- Validation on every client intent (reach, cooldown, slot legality, LOS) —
  the "server‑side checks" task (§8.2).

The C client code in `src/SurvivalTest.c`/`src/IndevTest.c` is the **spec** for
this C# port: every constant, RNG stream, and formula there was made
source‑faithful to the decompiled Java. Reuse it as the reference.

---

## 6. Draft survival sub‑protocol (starting spec — refine in‑session)

All messages ride PluginMessages `[0x35][channel][64B]`. Reserve **channel `0xB0`**
("survival") and put a **1‑byte message id** as payload byte 0, leaving 63 bytes.
(If you outgrow one channel, allocate `0xB0..0xBF`.) Multi‑byte fields are
big‑endian to match Classic. This is a *starting point*; finalise the exact
layouts as you implement each phase.

**Server → client (state):**

| id | name | payload |
|---|---|---|
| 0x01 | `SURV_HELLO` | mode(1: 0 off/1 c0.30‑s/2 indev), enhanced flag, protocol ver |
| 0x02 | `SURV_WORLDINFO` | groundLevel, waterLevel, fluid id, theme, floating flag, edge/sides ids (the `.mclevel` metadata set) |
| 0x03 | `SURV_HEALTH` | health(1), + later air/score |
| 0x04 | `SURV_TIME` | worldTime(2) or eased sky‑light level |
| 0x10 | `SURV_MOB_SPAWN` | mobId(2), type(1), x/y/z(fixed), yaw/pitch(1), health(1) |
| 0x11 | `SURV_MOB_MOVE` | mobId(2), x/y/z, yaw/pitch (or rel deltas) |
| 0x12 | `SURV_MOB_STATE` | mobId(2), flags (hurt/fuse/fire/onfire), health |
| 0x13 | `SURV_MOB_DESPAWN` | mobId(2), reason |
| 0x20 | `SURV_INV_FULL` | slot run: base slot(1), count(1), then id/count/dmg triples (chunked) |
| 0x21 | `SURV_INV_SLOT` | slot(1), id(2), count(1), dmg(2) |
| 0x22 | `SURV_CONTAINER_OPEN` | kind(1 chest/furnace/large), rows(1), title ref |
| 0x23 | `SURV_CONTAINER_SLOT` | slot(1), id(2), count(1), dmg(2) |
| 0x24 | `SURV_FURNACE_PROG` | burn(1), cook(1) |
| 0x30 | `SURV_DROP_SPAWN` | dropId(2), block/item id(2), count(1), x/y/z |
| 0x31 | `SURV_DROP_PICKUP`/`_REMOVE` | dropId(2) |
| 0x40 | `SURV_BLOCKMETA` | x/y/z, meta (farmland moisture / fire age / chest facing …) |

**Client → server (intents):**

| id | name | payload |
|---|---|---|
| 0x80 | `SURV_ATTACK` | targetMobId(2) (or block for mining handled by 0x05) |
| 0x81 | `SURV_USE_ITEM` | held slot, target block x/y/z, face |
| 0x82 | `SURV_INV_CLICK` | slot, button(L/R/shift), (cursor state server‑tracked) |
| 0x83 | `SURV_CONTAINER_CLICK` | slot, button |
| 0x84 | `SURV_CRAFT` | recipe/grid state or "take result" |
| 0x85 | `SURV_HELD_SLOT` | selected hotbar index (also mirror to `HeldBlock` CPE) |
| 0x86 | `SURV_DROP_ITEM` | slot / whole‑stack flag |
| 0x87 | `SURV_RESPAWN` / menu action | — |

Reuse standard packets where they already fit — don't duplicate:
- **Player movement**: keep the normal Classic position packets (§2.1) +
  `ExtEntityPositions`. Mobs *could* also be sent as normal Classic entities via
  `OPCODE_ADD_ENTITY`/`OPCODE_EXT_ADD_ENTITY2` + `ChangeModel`, with only the
  survival‑specific bits (health/fuse/type) on our channel — **decide this early**;
  it may save a lot of code vs a bespoke mob channel. (Trade‑off: bespoke channel
  gives exact Indev interpolation/animation control; standard entities give free
  nametag/skin/model plumbing but coarser control.)
- **Knockback**: `VelocityControl` CPE instead of a custom vector.
- **Status text / hearts context**: `MessageTypes` for status‑bar lines if useful.
- **Flight lockout**: `HackControl` to force no‑fly/no‑noclip on survival players.
- **Held block**: `HeldBlock` CPE (`OPCODE_HOLD_THIS`).

---

## 7. Phased roadmap (do them in this order)

Each phase is independently shippable and stays gated behind `SurvivalTest`.

- **Phase 0 — Handshake & mode.** Add the `SurvivalTest` CPE ext (client + server).
  Server sends `SURV_HELLO`; client flips into Indev mode on a *server‑provided*
  world (existing local sim still running — desync is fine at this stage). Prove
  the gate: a vanilla server ⇒ no change; our server ⇒ client shows the Indev HUD.
- **Phase 1 — Authoritative world.** Port the Indev generator into MCGalaxy (§8.1);
  send `SURV_WORLDINFO`; client stops running `IndevGen` in MP and applies
  surroundings/env from the server. Block metadata sync (`SURV_BLOCKMETA`).
- **Phase 2 — Health & damage.** Server owns health; `SURV_HEALTH` → client HUD.
  Fall/lava/fire/mob damage computed server‑side; client stops calling
  `SurvivalTest_Hurt` locally in MP. Death/respawn flow.
- **Phase 3 — Mobs.** Server runs the spawner + AI (port of `Mob_*`); streams
  spawn/move/state/despawn. Client renders + interpolates + plays sounds; sends
  `SURV_ATTACK`. Decide standard‑entity vs bespoke‑channel here.
- **Phase 4 — Inventory / crafting / containers.** Server‑authoritative inventory,
  crafting matcher, chest/furnace tile‑entities + the large‑chest combining;
  window open/slot/click transactions. Client GUIs become views of server state.
- **Phase 5 — World simulation & polish.** Day/night (`worldTime`), item drops +
  pickup, grass/leaf/farmland/fire random ticks, TNT/explosions, paintings,
  physics edge cases. Anti‑cheat validation pass (§8.2). Then start c0.30‑s.

---

## 8. The two tasks explicitly handed to the next session (server‑side, do first)

### 8.1 Port the Indev generator into MCGalaxy

- The client generator (`src/IndevGen.c`, byte‑verified faithful — see
  `doc/indev-generation.md`) is the reference. Port the noise stack
  (Perlin/Octave/Combined + the Java `Random`), the terrain pipeline, carve/ore/
  tree/flower passes, the assembling floor/border fill, `growGrassOnDirt`, and
  `findSpawn` to C# under `MCGalaxy/Generator/` and `Register("indev", GenType.Advanced,
  IndevGen.Generate, "…")` (§5.1). **Match the RNG streams exactly** (gen seed,
  `world.random` = seed+1 with the burned `nextInt`, spawn = seed+2) so worlds are
  identical to singleplayer for a given seed. Persist the Indev metadata
  (ground/water level, theme, surroundings) with the level so `SURV_WORLDINFO` can
  send it. `doc/indev-generation.md` + `src/IndevGen.c` are your spec.
- Deliverable: `/os map add indev` (or MCGalaxy's gen command) produces an Indev
  world server‑side, streamable to the client.

### 8.2 Server‑side checks & handling

Stand up the authoritative **SurvivalTest server module/plugin** (§5.2) and make it
*validate*, not trust, the client:

- Hook `OnPluginMessageReceivedEvent` on channel `0xB0`; parse intents (§6).
- Validate every intent: reach distance on block/attack, per‑action cooldowns
  (attack delay, eat time, bow draw), slot/recipe legality (can't craft what you
  can't afford), line‑of‑sight for attacks, container access rules
  (`BlockChest.blockActivated` "solid block above blocks opening", the double‑chest
  pairing/`canPlaceBlockAt` limits). Reject → correct the client with an
  authoritative state message.
- Own the tick: health regen/damage, mob spawner + AI + combat, furnace smelting,
  farmland/crop/grass/leaf/fire random ticks, day/night, item‑drop despawn.
- Force survival constraints with CPE: `HackControl` (no fly/noclip),
  block‑permission for creative‑only blocks, etc.
- Keep it **per‑level and mode‑aware**: only levels flagged Indev/c0.30‑s run the
  sim; Classic levels are unaffected (mirror the client's gate on the server).

---

## 9. Testing strategy

- **Regression first:** connect this client to a *stock* MCGalaxy/ClassiCube server
  and confirm nothing changed (creative, blocks, chat, existing CPE). Do this after
  *every* networking change.
- **Loopback:** run the modified MCGalaxy locally; connect the modified client via
  `MPConnection` to `127.0.0.1`. The headless rig (`/tmp/cc_run`, Xvfb `:99`, gdb)
  used for the SP work can drive the client; inspect `Server.SupportsSurvival`,
  the mob table, health, etc. via gdb exactly as in prior sessions.
- **Two clients** for genuine MP checks (mob/entity sync, block races).
- **Wire logging:** add a debug dump of every `SURV_*` frame (id + hex) behind a
  flag on both ends; diff client‑predicted vs server‑authoritative state.
- **Parity harness:** for a fixed seed, assert the MCGalaxy Indev world equals the
  SP `IndevGen` world (reuse the Java‑oracle mindset from `doc/indev-generation.md`).

---

## 10. Risks / gotchas

- **PluginMessages payload width:** client sends/receives exactly 64 bytes; confirm
  MCGalaxy matches (one source claimed 256). Pad short messages; never over‑read.
- **Extension name collisions:** pick a survival ext name unlikely to clash; bump
  its version when the wire format changes and branch on `serverVersion`.
- **Fixed‑point positions:** mobs moving fast/high need `ExtEntityPositions`;
  otherwise clamp to Classic's range.
- **Block ID space:** Indev uses ids beyond the Classic 0–49 set (torch 50, fire 51,
  chest 54, furnaces, crops, …). Use `CustomBlocks`/`BlockDefinitions` CPE so the
  server can define them and the client renders them; keep the SP block table and
  the server definitions in sync.
- **Authority migration is the hard part:** moving mob AI/health/containers from
  client‑authoritative (today) to server‑authoritative without visual jank needs
  client‑side interpolation + reconciliation. Phase it; don't try to flip
  everything at once.
- **Don't regress SP:** every guard you add must have an `if (Server.IsSinglePlayer)`
  fast path that behaves exactly as today.

---

## 11. First concrete steps for the next session

1. Read `src/Protocol.c` (CPE negotiation + `CPE_SendPluginMessage`/`CPE_PluginMessage`),
   `src/Server.c` (SP/MP split), and MCGalaxy `ClassicProtocol.cs` + `MapGen.cs`.
2. Verify MCGalaxy's `0x35` payload width and its `OnPluginMessageReceivedEvent`.
3. **Server task 8.1** — port the Indev generator into MCGalaxy and register it.
4. **Server task 8.2** — scaffold the SurvivalTest server module: negotiate the
   `SurvivalTest` CPE ext, send `SURV_HELLO`/`SURV_WORLDINFO`, receive channel `0xB0`.
5. **Client Phase 0** — add `survival_Ext` to `cpe_clientExtensions[]`,
   `Server.SupportsSurvival`, and `src/SurvivalNet.c` that flips Indev mode on
   `SURV_HELLO`. Prove the gate (vanilla server unaffected).
6. Then walk the phases (§7). Update this file as the wire format solidifies.

## 12. Client code map — what to read / reuse / gate, per subsystem

These are the exact symbols the networked layer touches. In MP survival, the
authoritative logic moves to the server; the client keeps the *render/predict/
intent* half. Every "authoritative" call below needs an
`if (Server.IsSinglePlayer) { …as now… } else { …driven by server…}` split.

**Health** (`src/SurvivalTest.c`)
- State: `int SurvivalTest_Health` (`:53`, extern in `.h`), `SURVIVAL_MAX_HEALTH`
  (20), `st_score` (`:85`), `st_lastHealth`/invuln window inside `SurvivalTest_Damage`.
- Authoritative: `SurvivalTest_Damage(damage, attackerPos)` (`:~1240`) — invuln
  frames (equal-hit half-block), armor absorption call, knockback dir; public
  wrappers `SurvivalTest_Hurt` (`:1289`), `SurvivalTest_HurtFrom` (`:1290`),
  `SurvivalTest_Heal` (`:1338`). Every call site (fall/lava/fire/explosion/mob at
  `:3210,:3367,:3685,:4056`) must become server‑side in MP.
- Render/HUD: hearts drawn in `src/Screens.c` (HUDScreen). In MP the HUD just
  reflects the server's `SURV_HEALTH`.

**Mobs** (`src/SurvivalTest.c`)
- State: `struct Mob` (fields: `type,active,hasTarget,targetSlot,health,invincTicks,
  hurtTicks,attackTime,fire,fuseTicks/fuseState,graze,walkDist,hasHelmet/hasArmor,
  Base.prev/next` for interpolation), pool `st_mobs[]`, `mobTypeInfo[]` table,
  `enum MobType`.
- Authoritative: spawners `Mob_SpawnerRun`/`Mob_IndevSpawnPass`/`Mob_IndevSpawnerRun`/
  `SurvivalTest_IndevInitialSpawn`, AI `Mob_IndevCreatureAI`/`Mob_IndevCreatureUpdate`/
  `Mob_BasicAIUpdate` + pathfinder `Mob_FindPath`, combat `Mob_IndevAttackEntity`/
  `Mob_Hurt`/`Mob_Die`, factory `SurvivalTest_SpawnMobAt`, size `Mob_ApplySize`,
  ground/normal‑cube `Mob_BlockIsSolid`/`Mob_BlockIsNormalCube`.
- Render: `SurvivalTest_RenderMobs`, `Mob_UpdateBodyYaw`, model set via
  `Entity_SetModel`. This half stays client‑side (fed by `SURV_MOB_*`).
- Debug: `SurvivalTest_DebugSpawnMob` (`/client spawn`) is handy for MP testing.

**Armor** (`src/SurvivalTest.c`, `src/IndevArmor.c`, `src/IndevTest.c`)
- State: `st_armor[SURVIVAL_ARMOR_SLOTS]` (`:150`) — `[0]boots [1]legs [2]chest
  [3]helmet`, saved as `.mclevel` Slot 100+index.
- Authoritative: absorption in `SurvivalTest_Damage` (`:1210–1253`) — `remain`/
  `reduce` weighting + per‑hit +1 wear to each worn piece, piece breaks when
  `damage > IndevTest_ArmorMaxDamage`. Helpers `IndevTest_ArmorPiece`,
  `IndevTest_ArmorReduce`, `IndevTest_ArmorMaxDamage`.
- Render: `IndevArmor_Render` (`:5010`) worn plates; HUD armor bar in `Screens.c`.

**Inventory / crafting / containers** (`src/SurvivalTest.c`, `src/IndevTest.c`)
- State: `st_inv[SURVIVAL_INV_SLOTS]` (36), `st_craft[SURVIVAL_CRAFT_SLOTS]`,
  `st_cursor`, `struct SurvivalSlot {id,count,damage}` (in `.h`), unified addressing
  `SurvivalTest_SlotPtr` (`:6264`, with `SURVIVAL_{INV,CRAFT,CONTAINER,ARMOR}_BASE`
  extended‑slot ranges). Change ticker `SurvivalTest_InvVersion`/`_InvChanged`.
- Ops (make server‑authoritative): `SurvivalTest_AddItem`, `SurvivalTest_DamageHeldTool`,
  `SurvivalTest_ConsumeSelected`, crafting `IndevTest_MatchRecipe`/`SurvivalTest_CraftResult`/
  `_CraftTake`, containers `IndevTest_OpenContainer`/`_ContainerSlot`/`_ContainerSlotCount`,
  furnace `Furnace_Tick`, chest scatter `IndevTE_Scatter`.
- GUI (stays client‑side, becomes a view of server state): `SurvivalInvScreen_*`
  in `src/Screens.c`.

**World / time / metadata / tile‑entities** (`src/IndevTest.c`, `src/IndevGen.c`, `src/IndevFire.c`)
- Day/night: `indev_worldTime`, `indev_lastSkyLight`, `Indev_TickDayNight`,
  `Indev_EasedSkyLight`.
- Surroundings/env: `IndevTest_SetSurroundings`/`_ApplySurroundings`,
  `indev_surGround/surWater/surFluid`; env planes via `Env_*`.
- Tile entities: `struct IndevTE` + `indev_tes[]` (chest/furnace contents),
  `IndevTE_Find/_Create/_Scatter`.
- Block metadata: `IndevTest_BlockDataMeta(At)` / `IndevTest_ApplyDataMeta(At)`
  (chest facing, furnace lit, farmland moisture, crop stage), fire age in
  `src/IndevFire.c` (`IndevFire_Age/_SetAge`).
- Random ticks: `IndevTest_TickRandomBlocks` → grass/leaf/farmland/crop/fire.
- Worldgen: `src/IndevGen.c` (`IndevGen_*`) — **client does NOT run this in MP**.

**Serialization surface = the sync manifest** (`src/Formats.c`, MCLevel_*)
The `.mclevel` reader/writer already enumerates *every* piece of survival state
that must cross the wire: player `Inventory` (main + Slot 100+ armor), `Entities`
(mobs w/ type+health+pos), `TileEntities` (chest/furnace `Items`), `Environment`
(`Surrounding{Ground,Water}{Type,Height}`, colours), map dims/spawn. Treat
`MCLevel_ParseEnvironment`/`_CommitEntity`/`_CommitTE`/the inventory parser as the
canonical list when designing `SURV_WORLDINFO` / `SURV_INV_FULL` / `SURV_MOB_SPAWN`.

---

## 13. Server‑side handling — questions & recommended answers

Design answers for the server module (§8.2). Default stance everywhere: **the
server simulates and validates; the client only sends intents and renders.**

**Health**
- *Q: Who owns it?* Server. In MP the client's `SurvivalTest_Health` is a display
  mirror; local `_Hurt/_HurtFrom/_Heal` are suppressed and replaced by `SURV_HEALTH`.
- *Q: How is damage derived so it can't be spoofed?* Server computes it from
  server‑tracked state: fall damage from the server's view of the player's
  fall distance (never trust a client "I took fall damage" message), lava/fire
  from the block the server sees under/around the player, mob melee from the
  server's mob combat, explosion from server‑side blast falloff. Port the exact
  math + the 20‑tick invulnerability window (equal‑damage hits half‑blocked) from
  `SurvivalTest_Damage`.
- *Q: Regen?* Indev has **no** natural regeneration (no hunger system; that's
  Alpha+). Health only drops; it resets on respawn/new world. c0.30‑s is the same.
  So there is no regen packet — don't add one.
- *Q: Death?* Server detects `health <= 0`, sends a death `SURV_HEALTH`(0) +
  a death/score message; client shows the existing `GameOverScreen`. Respawn =
  server loads/regenerates and re‑streams (there is no in‑place respawn in Indev).
- *Q: Knockback?* Use the `VelocityControl` CPE packet from the server rather than
  a custom vector — the client already implements it.

**Mob + armor state**
- *Q: Bespoke mob channel vs standard Classic entities?* Recommend standard
  entities for transport (`OPCODE_EXT_ADD_ENTITY2` + `ChangeModel` for the model,
  normal pos/orient updates + `ExtEntityPositions`) and put *only* the
  survival‑specific bits (mob type for AI‑independent client anim, health for the
  hurt‑flash, creeper `fuseState`, `onFire`) on `SURV_MOB_STATE`. This reuses all
  the existing nametag/interp/model plumbing; you lose nothing because the client
  no longer runs AI. Keep `SURV_MOB_SPAWN` only if you need type/health atomically
  with the spawn.
- *Q: Does the client need mob health/AI?* No AI (server‑only). It needs enough
  for rendering: type→model, `hurtTicks` flash, creeper swell, fire overlay,
  death animation. Feed those from `SURV_MOB_STATE`.
- *Q: Where does armor absorption run?* Server, inside its damage routine (port
  of `SurvivalTest_Damage` armor block). The server owns the 4 armor slots +
  their durability and wears/breaks them. The client only needs the slot contents
  + damage to draw the HUD armor bar and (optionally) worn plates — send via
  `SURV_INV_SLOT` for slots 100–103.
- *Q: Show other players' worn armor?* Defer. First pass: only the local player's
  HUD + server‑side absorption. Later, worn plates on remote players via
  `CustomModels`/a plates model.

**Inventory tracking**
- *Q: Authority + anti‑dupe?* Server owns `main[36] + armor[4] + craft grid +
  cursor + open container`. The client sends **intents** (`SURV_INV_CLICK`,
  `SURV_CONTAINER_CLICK`, `SURV_CRAFT`, `SURV_DROP_ITEM`, `SURV_USE_ITEM`,
  `SURV_HELD_SLOT`); the server validates (slot exists, recipe affordable, stack
  limits, container open + in reach) and echoes authoritative slot states. The
  cursor‑held stack is **server‑tracked** — the client renders what the server
  says, so click races can't dupe.
- *Q: Full state vs deltas over 64‑byte frames?* On open/join send a chunked
  `SURV_INV_FULL` (base slot + run of id/count/dmg triples across several frames);
  thereafter send `SURV_INV_SLOT` deltas. Same for containers
  (`SURV_CONTAINER_OPEN` then `SURV_CONTAINER_SLOT`).
- *Q: Block/tool consumption?* Server‑side only. Placing consumes 1 from the held
  slot; breaking wears the tool (`IndevTest_ToolUseWear`) and drops per
  `SurvivalTest_GetBlockDrop`/`SpawnIndevDrops`. Never let the client decrement.
- *Q: Crafting grid?* Server owns the grid; client sends place/take‑result
  intents; server runs `IndevTest_MatchRecipe` (incl. mirrored layouts) and echoes
  grid + result slot. Large‑chest 54‑slot combining (`indev_openTE2`) is a
  server concern too.

**World tracking & status**
- *Q: Who runs the world simulation?* Server. Day/night (`worldTime` +
  `updateDaylightCycle` skylight easing), all random ticks (grass spread/decay,
  leaf decay, farmland/crop growth, fire spread, sapling growth), furnace
  smelting, item‑drop despawn (6000‑tick), TNT/explosions. Results reach clients
  as normal `OPCODE_SET_BLOCK` (block id) + `SURV_BLOCKMETA` (nibble) +
  `SURV_DROP_*` (items) + `SURV_TIME`.
- *Q: How does the client learn Indev's non‑Classic world params?* `SURV_WORLDINFO`
  right after `LEVEL_END`, carrying the `.mclevel` `Environment` set (ground/water
  level, surrounding fluid/heights, theme, floating flag, colours). Client applies
  via `IndevTest_SetSurroundings` + `Env_*`.
- *Q: Block id space beyond Classic 0–49?* Torch 50, fire 51, chest 54, furnaces,
  crops, diamond, etc. must be defined to the client with `BlockDefinitions`/
  `CustomBlocks` CPE so it renders them; keep the server's definitions in lockstep
  with the client's SP block table.
- *Q: Per‑level gating?* The server must run the survival sim **only** on
  Indev/c0.30‑s levels and leave Classic levels as plain creative — the exact
  mirror of the client's `SurvivalTest_Enabled`/`IndevTest_Enabled` gate. A level
  property (e.g. `level.SurvivalMode = off|c030s|indev|indevCreative`) drives both
  the sim and the `SURV_HELLO` sent to joiners.

---

## 14. Pseudo‑creative Indev mode (research + future task)

**This is genuine Indev behaviour, not an invention.** Indev shipped two
controllers (`net/minecraft/client/controller/`):

- `PlayerControllerSP` = **survival** (`world.survivalWorld = true`): timed mining
  (`blockStrength`/`sendBlockRemoving`), block drops on harvest, tool wear
  (`onBlockDestroyed`), health HUD.
- `PlayerControllerCreative` = **creative** (`world.survivalWorld = false`):
  `shouldDrawHUD() = false` (no hearts / no health), **instant break** (base
  `clickBlock` destroys immediately, no mining time), an **infinite palette
  hotbar** (`onRespawn` fills the 9 hotbar slots from the registered block list,
  stack sizes forced — placing never depletes), no tool wear/consumption — **but
  `onUpdate` still runs `mobSpawner.performSpawning()`, so mobs still spawn** (you
  just have no health to lose). No flight in this era.

So "pseudo‑creative Indev" = the Indev world + block set + mob spawning + day/night,
with the *survival* layer (health/damage, mining time, item consumption, tool
wear, inventory scarcity, crafting need) switched off. It is the genuine Indev
Creative game mode.

**Why it matters for networking:** it is by far the **simplest networked Indev
target** — no health sync, no inventory transactions, no crafting/containers,
instant‑break via plain `OPCODE_SET_BLOCK`. It's the ideal Phase‑0/1 proving
ground: get the Indev *world* (server‑generated), *blocks*, *mobs*, and *day/night*
flowing over the wire before taking on the hard survival authority migration.

**Tasks it implies (do the SP research/impl first, then network it):**
1. Add a 4th mode to the gamemode enum — **Indev Creative** — distinct from the
   current `OFF` (plain ClassiCube creative) and `INDEV` (Indev survival). It sets
   `IndevTest_Enabled = true` (Indev world/blocks/gen/day‑night/mob‑spawning) but
   leaves the survival simulation off: no `SurvivalTest_Health`/damage, instant
   break, infinite blocks, no crafting/inventory scarcity, HUD hides hearts.
   Gate off a `SurvivalTest_CreativeIndev()` predicate; make sure `c0.30-s` and
   `INDEV survival` are unaffected. Mirror genuine `PlayerControllerCreative`
   (mobs still spawn; `survivalWorld=false`).
2. Verify vs the decompiled `PlayerControllerCreative` (instant break, palette
   hotbar contents/order, HUD‑off, mob spawner still ticking).
3. Network it: `SURV_HELLO` mode = indev‑creative; server generates the Indev
   world, spawns mobs, ticks day/night + random blocks, and relays block changes —
   but sends no health/inventory/container messages and lets the client place/break
   freely (still validated for reach). This is the first end‑to‑end Indev MP demo.

---

## References

- CPE spec: https://c4k3.github.io/wiki.vg/Classic_Protocol_Extension.html
- Classic protocol (reference impls): https://github.com/PrismarineJS/minecraft-classic-protocol
- MCGalaxy (server base, this fork): https://github.com/UmbreoClaw/mcgalaxy
- Client transport: `src/Protocol.c` (`CPE_SendPluginMessage` `:891`, `CPE_PluginMessage` `:1551`), `src/Protocol.h:98`, `src/Server.c`, `src/Server.h`.
- Simulation reference (the C "spec" to mirror server‑side): `src/SurvivalTest.c`, `src/IndevTest.c`, `src/IndevGen.c`, `doc/indev-generation.md`, `SURVIVAL_TEST_NOTES.md`.
- Per‑subsystem client symbols: §12. Server‑side design Q&A: §13. Indev creative mode: §14.
- Indev creative/survival game modes (decompiled): `net/minecraft/client/controller/PlayerControllerCreative.java` (creative: `survivalWorld=false`, no HUD, instant break, palette hotbar, mobs still spawn) and `PlayerControllerSP.java` (survival). Serialization manifest: `src/Formats.c` `MCLevel_*`.
