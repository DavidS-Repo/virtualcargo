# Clippy Virtual Cargo

Clippy Virtual Cargo stores DayZ container contents in a private local PostgreSQL database while virtual cargo is closed. Items return to DayZ when a player opens the container and are committed back through the existing revision, operation, session, migration, cleanup, and locking workflow.

Version 1.0.0 adds the complete local Admin Panel release, richer DayZ container metadata, optional player telemetry, and optional live player inventory controls.

## Install

1. Download the Server Ready or GitHub Ready release.
2. Extract it into the folder that contains `DayZServer_x64.exe`.
3. Review `ClippyServerManager.example.json` if you need custom launch, cargo, Admin Panel, telemetry, or retention settings.
4. Run `START-CLIPPY-SERVER.bat`.
5. Run `OPEN-CLIPPY-ADMIN.bat` when you want the local Admin Panel.

Existing server-specific `ClippyServerManager.json` files are preserved. New 1.0.0 settings are merged with safe defaults rather than replacing your server configuration.

PostgreSQL remains private on `127.0.0.1:27816`. ClippyStorageHost remains private on `127.0.0.1:27815`. ClippyAdminHost remains private on `127.0.0.1:27817`.

## Admin Panel

The 1.0.0 Admin Panel includes Overview, Containers, nested inventory trees, global item search, Players, Reports, Activity, Sessions, Recovery, Maintenance, Backups, Quarantine, Audit, Database inspection, Settings, and Ctrl+K navigation/search.

The web UI is embedded in `ClippyAdminHost.exe`. It does not use a CDN or require Internet access.

Admin writes use domain-specific endpoints, prepared PostgreSQL queries, a separate least-privilege edit role, short maintenance locks, revision checks, active workflow checks, PostgreSQL transactions, before/after records, audit events, quarantine, snapshots, and revision-checked undo. There is no browser SQL console and the browser never receives PostgreSQL passwords or the ClippyStorageHost API token.

Supported stored-cargo actions include quantity and health edits, remove, quarantine/restore, move/copy, nested detach-to-root, root duplication, snapshots, change comparison/export, selected-root export, bounded bulk operations, maintenance locks, and undo. Adapter-specific state JSON is not exposed as a generic editor because those fields need item-type validation.

## Container telemetry

The DayZ 1.0.0 mod reports container class, map, world position, first-seen state, and last-seen activity to the private storage service. This lets the Admin Panel show real container metadata and stale-container reports without guessing from PostgreSQL storage rows.

## Player telemetry

Player telemetry is optional and disabled by default with `AdminPanel.EnablePlayerTelemetry`.

When enabled, the DayZ server mod can report the durable hashed DayZ player ID, display/profile names, session player ID, online activity, inventory snapshots, equipment summary, map, position, ping estimates, bandwidth estimates, and output-throttle measurements. Historical snapshots are retained according to `PlayerTelemetryRetentionDays` and `PlayerSnapshotHistoryLimit`. The searchable player-item index contains only each player's newest snapshot.

`EnablePlayerNetworkTelemetry` and `EnablePlayerPositionTelemetry` can be controlled separately.

The supported DayZ script interface does not provide the mod with a player IP-address getter. Clippy does not scrape logs, inspect player machines, use the plaintext account ID, or invent an IP value. The Admin Panel reports IP collection as unavailable through the supported DayZ API.

## Live player inventory control

Live player control is optional and disabled by default with `AdminPanel.EnableLivePlayerControl`. It also requires player telemetry.

Commands are queued by AdminHost, claimed by the DayZ server-side Clippy script, executed against the live `PlayerBase` entity, and returned with success or failure. PostgreSQL is not used to pretend a live DayZ inventory changed.

Supported live actions include requesting a fresh inventory snapshot, giving an item, repairing an item, moving an item between online players, removing an item, quarantining an item, and restoring a quarantined item.

Commands have unique IDs, short expiry, claim state, result state, auditing, input validation, and replay protection. Live controls refuse items that belong to an active Clippy virtual-cargo materialization.

## PostgreSQL schema

Schema version 8 added the derived `cargo_item_index`. `cargo_roots.tree_json` remains the source of truth.

Schema version 9 added safe Admin Panel maintenance locks, change history, quarantine, snapshots, and admin audit history.

Schema version 10 added the player registry, aliases, inventory snapshots, derived player-item index, player events, live command queue, player quarantine, and richer container metadata.

Schema version 11 adds the network/profile/position telemetry fields and retention support used by 1.0.0.

The manager backfills derived indexes in bounded work. A failed derived-index rebuild does not stop DayZ storage from starting.

## Recovery, backups, and restore

Recovery actions call ClippyStorageHost domain endpoints instead of rewriting operation/session state in the browser.

The Backups page can create and verify backups. Database restore stays in `ClippyServerManager.ps1`. Restore refuses a running DayZ server, stops the private hosts, verifies the selected backup, creates and verifies a fresh safety backup, requires explicit confirmation, restores PostgreSQL, reapplies restricted roles, runs migrations and an integrity check, and leaves DayZ/StorageHost stopped.

## Reports

Reports are on-demand and bounded. The 1.0.0 panel includes stored item-class totals, container-class counts, largest containers, stale-container counts, player-carried class totals, and duplicate virtual item IDs. Run the included PostgreSQL benchmark tools before using heavier reports during peak gameplay on a large server.

## Security

The Admin Panel only accepts loopback requests and uses a one-use bootstrap token, HttpOnly session cookie, CSRF token, Host/Origin checks, Fetch Metadata checks, security headers, request limits, and idle shutdown.

PostgreSQL is never made public. The browser does not receive PostgreSQL credentials, service secrets, or DayZ signing material.

The private `.biprivatekey` is never included in GitHub Ready, Workshop Upload, or Server Ready packages. Version 1.0.0 is signed with the existing `ClippyVirtualCargo_0_1` identity.

## Source and packages

`START-CLIPPY-SERVER.bat` starts the server manager.

`OPEN-CLIPPY-ADMIN.bat` opens the local authenticated Admin Panel.

`ClippyVirtualCargoPayload` contains the signed Workshop payload, ClippyStorageHost, ClippyAdminHost, payload manifest, and default profile settings.

`Source` in the GitHub Ready package contains the C++ hosts, AdminWeb source, DayZ mod source, and server-manager source for review and modification. Source files are not included in Server Ready.

## License

Clippy Virtual Cargo is released under the MIT License. See `LICENSE.txt`.
