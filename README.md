# Clippy Virtual Cargo

Clippy Virtual Cargo stores selected DayZ container contents in a private local PostgreSQL database while virtual cargo is closed. Items return to DayZ when a player opens the container and are committed back through Clippy's revision, session, migration, cleanup, and locking workflow.

Version 1.0.3 is a DayZ-side portable-container pickup and action-targeting fix. Idle clothing, backpacks, bags, barrels, and other item-based cargo containers can use normal DayZ pickup-to-hands behavior again. Clippy container actions now use the interaction input group and are only attached to cargo-capable items, so medicine and other ordinary held items keep their normal target-player actions. Clippy still locks movement while virtual cargo is opening, saving, recovering, or migrating. ClippyAdminHost remains at 1.0.1, ClippyStorageHost remains at 1.0.0, and PostgreSQL schema 11 is unchanged.

## Version split

- Clippy Virtual Cargo release: 1.0.3
- ClippyAdminHost: 1.0.1
- ClippyStorageHost: 1.0.0
- Signed DayZ Workshop payload: 1.0.3
- PostgreSQL schema: 11
- ClippyServerManager revision: 21

This split is intentional. Version 1.0.3 changes the DayZ mod source and requires the new signed 1.0.3 PBO. The native hosts and database schema do not change.

## Install or upgrade

1. Stop the DayZ server before replacing server-side Clippy files.
2. Download the 1.0.3 Server Ready or GitHub Ready package.
3. Extract it into the folder that contains `DayZServer_x64.exe` and allow the Clippy server-side files to be replaced.
4. Keep your existing `ClippyServerManager.json`. The manager preserves it and adds missing settings with defaults.
5. Run `START-CLIPPY-SERVER.bat`.
6. Run `OPEN-CLIPPY-ADMIN.bat` when you want the local Admin Panel.

For a new installation, review `ClippyServerManager.example.json` if you need custom launch, virtual-cargo, Admin Panel, telemetry, retention, or live-control settings.

PostgreSQL stays on `127.0.0.1:27816` by default. ClippyStorageHost stays on `127.0.0.1:27815`. ClippyAdminHost stays on `127.0.0.1:27817`.

## Portable container pickup in 1.0.3

Clippy now separates a container being managed from a container being unsafe to move. Idle portable storage can be picked up normally when DayZ allows it. A separate movement lock is used only while Clippy has an active open, save, recovery, or migration workflow for that container.

## Container interaction compatibility in 1.0.2

Item-based storage can impose its own open, lock, ownership, door, or cargo rules. Clippy now requires an item-based container to be in its normal native open state before `Open virtual cargo` is available. Clippy does not call a container's `Open()` method itself, so another mod's unlock or access action is not bypassed.

After a virtual page is rebuilt, Clippy probes the container's real cargo receive/release rules while suspending only Clippy's own temporary physical-cargo guard. If the native rules still reject the materialized items, Clippy removes the temporary page and aborts the OPEN session instead of showing cargo that cannot be moved.

For lockable third-party storage, unlock and open the container normally first, then use `Open virtual cargo`. A container class whose native rules still reject cargo interaction while open should be excluded or integrated with a dedicated compatibility adapter.

## Admin Panel

The local Admin Panel includes Overview, Containers, Items, Activity, Sessions, Recovery, Maintenance, Backups, Quarantine, Audit Log, Database, Reports, Players, Settings, and Ctrl+K navigation/search.

Version 1.0.1 changes the day-to-day admin workflow:

- Table headers are clickable sort controls. Loaded rows can be sorted ascending or descending with numeric, date/time, text, or DayZ-position ordering as appropriate for the column.
- Container, item, activity, audit, and player filters now use the same submit model. Click Search or press Enter to run the query.
- Item search no longer runs after every typed character or steals input focus.
- Opening Items with a blank query browses the first bounded page of indexed virtual cargo.
- Filterable pages include Clear controls.
- Containers includes a `Has virtual cargo` shortcut for hiding discovered containers with zero stored nodes.
- The Ctrl+K box documents its global search prefixes instead of looking like a second copy of each page's local filter box.

Sorting applies to the rows currently loaded in the browser. Database queries remain bounded rather than loading every row only to sort it client-side.

## Settings page

Version 1.0.1 can write a fixed allowlist of server-owner settings back to the real `ClippyServerManager.json` from the local Settings page.

Editable groups include Admin Panel state and limits, PostgreSQL read/write pool settings and timeouts, maintenance-lock duration, player telemetry, network and position telemetry, snapshot interval, telemetry retention, audit retention, live player controls, command polling, and command expiry.

The page does not expose PostgreSQL passwords, Clippy service tokens, signing keys, executable paths, PostgreSQL installation paths, or arbitrary JSON fields.

Settings changes use these safeguards:

- Apply stays disabled until a setting changes.
- Reset discards unsaved form changes.
- Input values are validated before the file is changed.
- A safety copy named `ClippyServerManager.json.before-admin-settings.bak` is kept.
- The page sends the fingerprint of the config it loaded. A save is rejected if another process changed the file in the meantime.
- The replacement is written atomically.
- The browser reloads after a successful save.

Changes to the AdminHost port, database pools, timeouts, or stored-cargo editing mode take effect after the Admin Panel is reopened. DayZ-side telemetry and live-control changes take effect after the managed DayZ server is restarted.

## Stored-cargo administration

Stored-cargo writes use domain-specific endpoints, prepared PostgreSQL queries, a separate least-privilege edit role, short maintenance locks, revision checks, active-workflow checks, PostgreSQL transactions, recoverable before-state records, audit events, quarantine, snapshots, and revision-checked undo.

Supported stored-cargo actions include:

- Quantity and health edits
- Item or subtree removal
- Quarantine and restore
- Move and copy
- Nested detach-to-root
- Root duplication
- Snapshots and snapshot comparison
- Change comparison and export
- Selected-root export
- Bounded bulk operations
- Maintenance-lock management
- Revision-checked undo

Adapter-specific state JSON is not exposed as a generic editor because those fields need item-type validation.

## Container telemetry

The DayZ 1.0.0 mod reports container class, map, world position, first-seen state, and last-seen activity to the private storage service. The Admin Panel uses those fields for container details, filtering, and stale-container reports.

Older database rows can show unknown class, map, or position until DayZ observes those containers again.

## Player telemetry

Player telemetry is optional and disabled by default with `AdminPanel.EnablePlayerTelemetry`.

When enabled, the DayZ server mod can report:

- Durable hashed DayZ player ID
- Display and profile names
- Alias history
- Session player ID
- Online activity
- Inventory snapshots
- Equipment summary
- Map and world position
- Ping estimates
- Bandwidth estimates
- Output-throttle measurements
- Recent Clippy container activity
- Player events

Historical snapshots are limited by `PlayerTelemetryRetentionDays` and `PlayerSnapshotHistoryLimit`. The searchable player-item index contains only each player's newest snapshot.

`EnablePlayerNetworkTelemetry` and `EnablePlayerPositionTelemetry` can be controlled separately.

The supported DayZ script interface does not provide a player IP-address getter to the mod. Clippy does not scrape logs, inspect player machines, use the plaintext account ID, or invent an IP value.

## Live player inventory control

Live player control is optional and disabled by default with `AdminPanel.EnableLivePlayerControl`. It requires player telemetry.

Commands are queued by AdminHost, claimed by the DayZ server-side Clippy script, executed against the live `PlayerBase` entity, and returned with success or failure. PostgreSQL is not used to fake a live DayZ inventory change.

Supported live actions include:

- Request a fresh inventory snapshot
- Give an item
- Repair an item
- Move an item between online players
- Remove an item
- Quarantine an item
- Restore a quarantined item

Commands have unique IDs, short expiry, claim state, result state, auditing, validation, and replay protection. Live controls refuse items that belong to an active Clippy virtual-cargo materialization.

## Reports

Reports are on-demand and bounded. The panel includes stored item-class totals, container-class counts, largest containers, stale-container counts, player-carried class totals when telemetry is enabled, and duplicate virtual item IDs.

The browser does not receive an unrestricted SQL endpoint.

## PostgreSQL schema

Schema version 8 added the derived `cargo_item_index`. `cargo_roots.tree_json` remains the source of truth.

Schema version 9 added Admin Panel maintenance locks, change history, quarantine, snapshots, and admin audit history.

Schema version 10 added the player registry, aliases, inventory snapshots, derived player-item index, player events, live command queue, player quarantine, and richer container metadata.

Schema version 11 added network, profile, position, and telemetry-retention fields used by the 1.0.0 DayZ telemetry features.

The manager backfills derived indexes in bounded work. A failed derived-index rebuild does not stop DayZ storage from starting.

## Recovery, backups, and restore

Recovery actions call ClippyStorageHost domain endpoints instead of rewriting operation or session status from the browser.

The Backups page can create and verify backups. Database restore remains in `ClippyServerManager.ps1`. Restore refuses a running DayZ server, stops the private hosts, verifies the selected backup, creates and verifies a fresh safety backup, requires explicit confirmation, restores PostgreSQL, reapplies restricted roles, runs migrations and an integrity check, and leaves DayZ and StorageHost stopped.

## Security

The Admin Panel accepts loopback requests only. It uses a one-use bootstrap token, HttpOnly session cookie, CSRF token, Host and Origin checks, Fetch Metadata checks, security headers, request limits, and idle shutdown.

PostgreSQL is not exposed to players or the public network by Clippy. The browser does not receive PostgreSQL credentials, ClippyStorageHost service secrets, or DayZ signing material.

The private `.biprivatekey` is never included in GitHub Ready, Workshop Upload, or Server Ready packages. The 1.0.3 Workshop payload is signed with the existing `ClippyVirtualCargo_0_1` identity.

## Source and packages

`START-CLIPPY-SERVER.bat` starts the server manager.

`OPEN-CLIPPY-ADMIN.bat` opens the local authenticated Admin Panel.

`ClippyVirtualCargoPayload` contains the signed Workshop payload, ClippyStorageHost, ClippyAdminHost, payload manifest, and default profile settings.

`Source` in GitHub Ready contains the C++ hosts, AdminWeb source, DayZ mod source, and server-manager source. Source files are not included in Server Ready.

## License

Clippy Virtual Cargo is released under the MIT License. See `LICENSE.txt`.
