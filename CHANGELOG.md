# Changelog

## 0.5.1

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
