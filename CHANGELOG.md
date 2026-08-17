# Changelog

## 0.5.0

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
