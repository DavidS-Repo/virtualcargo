# Clippy Virtual Cargo

Clippy Virtual Cargo moves DayZ container contents into PostgreSQL while the container is closed. Items return to DayZ when a player opens virtual cargo, then save back when the page closes.

The repository includes the built Windows storage host, the signed Workshop mod, and the source code. Server owners do not need to compile anything.

## Install

1. Download the repository ZIP or release ZIP.
2. Extract it into the folder that contains `DayZServer_x64.exe`.
3. Edit `ClippyServerManager.example.json` if you need different server launch settings or container exclusions.
4. Run `START-CLIPPY-SERVER.bat`.

The manager installs a private local PostgreSQL instance when needed. PostgreSQL program files stay with the server, while writable database data is kept under `%ProgramData%\ClippyVirtualCargo\PostgreSQL\data` on Windows. If it finds an older Clippy SQLite database, it keeps a safety copy and imports the data into PostgreSQL before the server starts.

## Container support

Fresh installs use `AutoDiscoverCargoContainers: true`. Clippy accepts top-level DayZ entities with cargo instead of requiring a short class allowlist. This includes normal vanilla storage and modded storage classes that use DayZ cargo. Vehicle cargo is also enabled by default.

`ContainerClassNames` is empty by default because auto-discovery does not need it. Set `AutoDiscoverCargoContainers` to `false` if you want to go back to an explicit class list.

`ExcludedContainerClassNames` always wins over auto-discovery and matches inherited classes. The default exclusions are `ClippyVirtualCargoQuarantine` and `FireplaceBase`. Add a class here if a modded container has custom lock, ownership, machine, or inventory rules that Clippy should not touch.

Clippy still rejects item trees it cannot safely capture. Explosives, traps, contaminated items, and active or plugged energy items remain blocked by the existing safety rules.

## Files

`START-CLIPPY-SERVER.bat` starts the manager.

`ClippyServerManager.ps1` installs or updates Clippy, sets up PostgreSQL, handles legacy SQLite import, starts the storage host, and starts DayZ.

`ClippyVirtualCargoPayload` contains the built storage host, signed Workshop files, and default DayZ profile settings.

`Source` contains the C++ storage host source and DayZ mod source. It is there for review and modification. It is not required to run the release.

## Security

PostgreSQL and the Clippy HTTP host bind to localhost. Database passwords and the API token are generated on the server and are not stored in this repository.

The DayZ signing private key is not included. The public `.bikey` and signed PBO are included.

## License

Clippy Virtual Cargo is released under the MIT License. See `LICENSE.txt`.
