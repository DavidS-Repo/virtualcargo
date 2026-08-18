# Clippy Virtual Cargo

Clippy Virtual Cargo moves DayZ container contents into PostgreSQL while the container is closed. Items return to DayZ when a player opens virtual cargo, then save back when the page closes.

The repository includes the built Windows storage host, the signed Workshop mod, and the source code. Server owners do not need to compile anything.

## Install

1. Download the repository ZIP or release ZIP.
2. Extract it into the folder that contains `DayZServer_x64.exe`.
3. Edit `ClippyServerManager.example.json` if you need different server launch settings or container exclusions.
4. Run `START-CLIPPY-SERVER.bat`.
5. Run `OPEN-CLIPPY-ADMIN.bat` when you want the local read-only admin panel.

The manager installs a private local PostgreSQL instance when needed. PostgreSQL program files stay with the server, while writable database data is kept under `%ProgramData%\ClippyVirtualCargo\PostgreSQL\data` on Windows. If it finds an older Clippy SQLite database, it keeps a safety copy and imports the data into PostgreSQL before the server starts.

## Container support

Fresh installs use `AutoDiscoverCargoContainers: true`. Clippy accepts top-level DayZ entities with cargo instead of requiring a short class allowlist. This includes normal vanilla storage and modded storage classes that use DayZ cargo. Vehicle cargo is also enabled by default.

`ContainerClassNames` is empty by default because auto-discovery does not need it. Set `AutoDiscoverCargoContainers` to `false` if you want to go back to an explicit class list.

`ExcludedContainerClassNames` always wins over auto-discovery and matches inherited classes. The default exclusions are `ClippyVirtualCargoQuarantine` and `FireplaceBase`. Add a class here if a modded container has custom lock, ownership, machine, or inventory rules that Clippy should not touch.

Clippy still rejects item trees it cannot safely capture. Explosives, traps, contaminated items, and active or plugged energy items remain blocked by the existing safety rules.

## Files

`START-CLIPPY-SERVER.bat` starts the manager.

`ClippyServerManager.ps1` installs or updates Clippy, sets up PostgreSQL, handles legacy SQLite import, starts the storage host, starts the local admin panel, and starts DayZ.

`OPEN-CLIPPY-ADMIN.bat` starts `ClippyAdminHost.exe` on `127.0.0.1:27817` and opens a short-lived authenticated local browser session. The Alpha panel is read-only.

`ClippyVirtualCargoPayload\ClippyAdmin\ClippyAdminHost.exe` contains the embedded offline admin UI and read-only query API.

PostgreSQL schema version 8 adds a derived `cargo_item_index` for fast nested item search. `cargo_roots.tree_json` remains authoritative. The manager backfills the index in small batches only while DayZ is stopped. Until the backfill is complete, the panel keeps using bounded root-class prefix search and exact nested item-ID lookup.

`ClippyVirtualCargoPayload` contains the built storage host, signed Workshop files, and default DayZ profile settings.

`Source` contains the C++ storage host source, admin host and web source, and DayZ mod source. It is there for review and modification. It is not required to run the release.

## Security

PostgreSQL, the storage host, and the admin host bind to localhost. Database passwords and the storage API token are generated on the server and are not stored in this repository.

The browser never receives PostgreSQL passwords or the storage API token. The admin host uses a separate PostgreSQL login that is forced to read-only mode. The Alpha build has no write API.

The DayZ signing private key is not included. The public `.bikey` and signed PBO are included.

## License

Clippy Virtual Cargo is released under the MIT License. See `LICENSE.txt`.
