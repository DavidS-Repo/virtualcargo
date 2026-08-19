# Changelog

## 1.0.0

- Completed the Admin Panel plan through the DayZ-side container metadata, player telemetry, and live command-bridge phases.
- Added PostgreSQL schema version 10/11 support for richer container metadata, players, aliases, player inventory snapshots, the latest-snapshot player item index, player events, live admin commands, player quarantine, network/profile/position telemetry, and retention controls.
- Added real DayZ container class, map, world position, first-seen, and last-seen reporting instead of inferring those values from storage rows.
- Added optional server-side player telemetry with hashed player ID, names/aliases, session player ID, online activity, inventory/equipment snapshots, map/position, ping, bandwidth, and output-throttle data.
- Kept player telemetry disabled by default and added separate network and position telemetry switches plus bounded retention/history settings.
- Added a full Players page with player search, online state, latest inventory, nested snapshot trees, carried-item search, snapshot comparison/export, equipment, aliases, recent Clippy container activity, events, command history, and quarantine recovery.
- Added optional live player inventory control executed inside the DayZ server process. Supported actions include fresh snapshot, give, repair, move, remove, quarantine, and restore.
- Added expiring/claimed live commands with unique IDs, validation, result reporting, audit records, and protection against changing items that belong to active virtual-cargo materializations.
- Added on-demand economy/forensic reports for stored classes, container types, largest containers, stale containers, carried classes, and duplicate virtual item IDs.
- Added telemetry integrity checks and bounded cleanup so historical telemetry and completed command data do not grow without limit.
- Updated the public example configuration for the 1.0.0 telemetry/live-control settings and corrected the manager/schema safety checks to require schema version 11.
- Rebuilt and signed the changed DayZ Workshop PBO with the existing `ClippyVirtualCargo_0_1` signing identity. No replacement signing key was generated.
- Preserved the local-only security model, PostgreSQL privacy, exact Server Ready allowlist, server-specific manager configuration, and private-key exclusion rules.

## 0.6.1

- Completed a second plan-to-code audit of the Admin Panel and corrected features that were still partial or only documented.
- Added an active Maintenance page with unexpired admin container locks, lock ownership handling, renew/release controls, and direct container navigation.
- Added bounded bulk move and copy for up to 25 selected roots with dry-run conflict reporting, stable lock ordering, revision checks, regenerated IDs for copies, one transaction, audit records, and undo-compatible change history.
- Added selected-root JSON export from global item search and kept all bulk selections bounded to 25 roots.
- Added PostgreSQL backup restore as a manager-controlled maintenance command. Restore refuses a running DayZ server, closes AdminHost and StorageHost, verifies the target archive, creates and verifies a fresh safety backup, requires exact typed confirmation, restores with pg_restore, reapplies restricted roles, runs StorageHost migrations and an integrity check, then leaves the server stopped.
- Added Restore instructions to the Backups page without giving the browser PostgreSQL administrator credentials.
- Added backup reasons and latest verification history to the Backups page.
- Improved keyboard/focus behavior for dialogs and live status/error announcements.
- Fixed Ctrl+K `storage:<id>` and `container:<query>` behavior so exact storage IDs open directly and container searches route to the filtered container list.
- Added Overview and container-detail shortcuts for recovery and integrity checks.
- Split release, StorageHost, AdminHost, and Workshop component versions in the payload manifest so an AdminHost-only update does not falsely relabel unchanged gameplay or Workshop binaries. Release 0.6.1 carries StorageHost 0.6.0 and the unchanged signed Workshop 0.5.0 payload.

## 0.6.0

- Added safe Admin Panel editing for virtual cargo. Every write uses domain-specific operations, short maintenance locks, active-workflow checks, expected revisions, a PostgreSQL transaction, audit records, and recoverable before-state data.
- Added PostgreSQL schema version 9 with admin maintenance locks, change sets, change entries, quarantine, container snapshots, snapshot roots, and durable admin audit events.
- Added a separate least-privilege PostgreSQL edit role. Browser code still never receives PostgreSQL passwords or the ClippyStorageHost service token.
- Added quantity and health edits, nested item removal, quarantine and restore, move and copy, nested detach-to-root, root duplication, change comparison/export, item history, and revision-checked undo. Adapter-specific state editing remains blocked until each adapter has its own validator.
- Added bounded bulk dry-run for up to 25 selected virtual roots, with conflict reporting and transaction-safe quarantine or removal.
- Added Recovery diagnostics for stale sessions, failed migrations, revision conflicts, orphaned roots, blocked containers, and the latest integrity result. Recovery abort actions continue to call ClippyStorageHost domain endpoints instead of rewriting workflow status columns.
- Added Activity and Audit filters including bounded from/to time ranges, backup create/verify/open-folder actions, snapshot comparison, quarantine management, and safe database table previews from a fixed server-side allowlist.
- Added bounded server-side container inventory export in JSONL format, plus container filters for active sessions, recovery state, admin locks, minimum node count, and indexed item-class containment.
- Expanded Overview with DayZ process state, estimated indexed item nodes, last backup, recent activity, recent admin changes, maintenance locks, quarantine state, and recovery actions.
- Added exact adapter and location filters to indexed item search, filter-only quantity/health searches without a class prefix, exact item history, search-result export, and real cancellation of obsolete browser searches.
- Added the planned performance-data seeder, repeat-latency benchmark harness, and schema safety test for disposable PostgreSQL databases.
- Expanded the HTTP security harness to reject unauthenticated writes, mutation attempts through GET, and authenticated writes without CSRF.
- Fixed release-source drift found during the final plan audit so the public AdminHost, AdminWeb, manager, example config, changelog, and maintainer source stay synchronized.
- Kept `cargo_roots.tree_json` authoritative. `cargo_item_index` remains derived data maintained in the same transaction by PostgreSQL triggers.
- Kept the signed DayZ Workshop payload at 0.5.0 because this release does not change DayZ mod source. The original `ClippyVirtualCargo_0_1` signing identity remains pinned.
- Container class/map/position telemetry, player inventory telemetry, and the live DayZ command bridge remain later signed DayZ-side phases. The Admin Panel does not invent that data from the current storage schema.

## 0.5.1

- Fixed `OPEN-CLIPPY-ADMIN.bat` failing to launch `ClippyAdminHost.exe` on Windows PowerShell because the bootstrap arguments were bound incorrectly by `Start-Process`.
- Removed the redundant Clippy Admin branding block from the main admin-panel sidebar.
- Added the Clippy Admin Panel Alpha with a localhost-only authenticated browser session, embedded offline UI, Overview, Containers, inventory trees, bounded item search, Sessions, Recovery, and Database pages.
- Added a dedicated PostgreSQL read-only login and separate admin connection pool. Admin editing remains disabled.
- Added `OPEN-CLIPPY-ADMIN.bat`, the server-manager `admin` command, admin executable payload hashing, and exact Server Ready packaging rules.
- Added schema version 8 with the derived `cargo_item_index`. `cargo_roots.tree_json` remains authoritative.
- Added same-transaction item-index maintenance through a PostgreSQL trigger and bounded backfill batches.
- Added indexed nested item search with keyset paging and quantity and health filters. Safe root-only and exact-ID fallbacks remain available until backfill is complete.
- Added an `EXPLAIN (ANALYZE, BUFFERS)` benchmark harness for the main item-search query families.
- Split the 0.5.1 server/runtime release version from the unchanged signed 0.5.0 Workshop payload version so the existing DayZ signing identity and PBO can be preserved.

## 0.5.0

- Fixed Windows setup so a partial PostgreSQL install with a missing service is repaired automatically.
- Changed the default container policy to auto-discover top-level cargo containers, including compatible modded storage.
- Enabled vehicle cargo by default for fresh installs.
- Changed class exclusions to match inherited classes, so excluding a base class also blocks its subclasses.
- Reduced the default exclusion list to the Clippy quarantine container and `FireplaceBase`.
- Replaced the live SQLite backend with PostgreSQL.
- Added concurrent processing for different storage containers.
- Added row locking for operations on the same container.
- Stored each virtual root and its nested item tree as one PostgreSQL record.
- Reduced container page reads and batched commit queries.
- Added a 256-node page budget and a global DayZ materialization queue.
- Added automatic fresh PostgreSQL setup and automatic legacy SQLite import.
- Removed the old ReportOnly migration workflow.
- Kept runtime SQL values in prepared or parameterized queries.
