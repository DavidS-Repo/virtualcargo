# Clippy Virtual Cargo

Clippy Virtual Cargo is a DayZ server mod that moves eligible container contents out of the live game world while the container is closed. The physical container stays in DayZ. Its stored items are kept in a private PostgreSQL database until a player opens the container again.

The project is built for persistent servers with large amounts of stored loot. Closed virtual cargo no longer exists as normal DayZ item entities that must remain loaded, persisted, and networked.

This is container storage, not a player bank or shared account system.

## How it works

Clippy uses the container's normal DayZ cargo grid as the only player-facing inventory. There is no separate virtual-cargo inventory or alternate storage prompt.

When nobody is using an eligible container, Clippy stores its cargo tree in PostgreSQL and removes those item entities from the live world. When a player opens DayZ inventory near an accessible container, Clippy restores the stored cargo into that same native cargo grid before the inventory opens. Closing inventory, leaving range, disconnecting, or moving the container out of its world position saves the live cargo back to storage.

Normal DayZ access rules still apply to player interactions. During a hierarchy transition, Clippy materializes stored roots into native cargo so nested vehicle and storage cargo can use those rules.

When a virtualized portable container enters hands, an attachment, or nested vehicle or storage cargo, Clippy restores its stored roots to the native DayZ cargo grid. Returning the container to top-level ground stores those roots in PostgreSQL again.
For stored records with cargo coordinates, Clippy restores the exact cargo index, row, column, and rotation. It does not silently repack an exact record into another free slot.

## Container scope

Fresh installations can automatically discover compatible top-level entities with cargo storage. Inherited container classes and vehicle cargo are supported by the default configuration.

Players are never treated as virtual cargo containers.

Servers can disable automatic discovery and use an explicit class list. Classes with custom locks, ownership rules, machines, remote storage, or unusual inventory behavior can be excluded with `ExcludedContainerClassNames`.

The default exclusions include:

- `ClippyVirtualCargoQuarantine`
- `FireplaceBase`

## Item state

The built-in DayZ item adapter stores common item state, including:

- Class and quantity
- Global and damage-zone health
- Wetness and temperature
- Liquids, energy, and food stage
- Magazine cartridges
- Weapon chambers and internal magazines
- Attachments
- Nested cargo and cargo locations

Items with private class-specific state need dedicated adapter support if that state must survive virtualization. Mods can register a `CVCWorldItemAdapter` for their own item classes.

Unsafe item trees fail closed instead of being partly removed from DayZ. Default safety rules reject cases such as contaminated items, active or plugged energy items, explosives, and traps when their state cannot be restored safely.

## Player use

Use containers normally. Open or unlock them with their normal DayZ or mod-provided action, then open inventory with the standard DayZ controls.

Clippy does not add an `Open virtual cargo` prompt. Items are shown in the container's normal cargo grid and use normal dragging, attachments, nested cargo, placement, pickup, and vehicle inventory rules.

## Server companion

The Workshop mod requires the Clippy server companion on the DayZ server machine. The companion includes:

- `ClippyServerManager.ps1`
- `ClippyStorageHost.exe`
- `ClippyAdminHost.exe`
- PostgreSQL setup and migration support
- Backup, restore, recovery, and integrity tools
- Default server-side configuration

`START-CLIPPY-SERVER.bat` starts the managed server stack.

`OPEN-CLIPPY-ADMIN.bat` opens the local Admin Panel.

`STOP-CLIPPY-SERVER.bat` closes DayZ, ClippyAdminHost, ClippyStorageHost, and the configured PostgreSQL service after requesting a StorageHost backup.

## Installation

1. Install or subscribe to the Clippy Virtual Cargo Workshop mod.
2. Download the matching Server Ready package from GitHub Releases.
3. Stop the DayZ server.
4. Extract the Server Ready package into the folder containing `DayZServer_x64.exe`.
5. Run `START-CLIPPY-SERVER.bat` as Administrator.
6. Keep the generated `ClippyServerManager.json` for future updates.

The manager creates missing configuration with defaults and preserves existing settings during updates.

Players only need the Workshop mod. PostgreSQL and the Clippy server hosts run on the server machine.

## Configuration

`ClippyServerManager.json` controls DayZ launch settings, container discovery, exclusions, Admin Panel settings, telemetry, retention, and optional live player controls.

See `ClippyServerManager.example.json` for the supported structure.

Default local service ports are:

- ClippyStorageHost: `127.0.0.1:27815`
- PostgreSQL: `127.0.0.1:27816`
- ClippyAdminHost: `127.0.0.1:27817`

These services are intended to remain private to the server machine.

## Admin Panel

The local Admin Panel provides views and tools for:

- Virtual cargo containers and nested inventories
- Stored item search
- Active sessions and recovery state
- Maintenance locks
- Quarantine
- Snapshots and change history
- Audit and activity history
- Database status and integrity checks
- Backups
- Reports
- Optional player telemetry
- Optional live player inventory controls

Stored-cargo edits use revision checks, maintenance locks, PostgreSQL transactions, audit records, and recoverable before-state data.

The browser does not receive unrestricted SQL access, PostgreSQL credentials, Clippy service secrets, or DayZ signing material.

## Player telemetry

Player telemetry is optional and disabled by default.

When enabled, Clippy can record supported DayZ server data such as hashed player identity, names, online state, inventory snapshots, equipment, map position, ping estimates, bandwidth estimates, and recent Clippy activity.

The mod does not inspect player computers, hardware, files, or unrelated client data. The supported DayZ script interface does not expose a player IP-address getter, so Clippy does not collect player IP addresses through the mod.

## Live player controls

Live player inventory controls are optional and require player telemetry.

Commands execute against the live DayZ `PlayerBase` entity. Supported operations include requesting a fresh snapshot, giving an item, repairing an item, moving an item between online players, removing an item, and quarantine or restore operations.

Commands use IDs, expiry, claim state, result reporting, validation, replay protection, and audit records.

## Backups and recovery

The server manager includes PostgreSQL backup, verification, restore, migration, and integrity checks.

Database restore requires the DayZ server to be stopped. The restore workflow verifies the selected backup and creates a fresh safety backup before replacing the active database.

The Admin Panel also exposes session recovery, snapshots, quarantine, change history, and maintenance tools.

## Compatibility

Clippy does not require a framework.

Mods that replace the same inventory actions, container classes, or mission hooks can conflict depending on load order. Custom locks, ownership systems, storage systems, vehicle inventory rules, and private item state may need an exclusion or adapter.

Test custom container and item mods before enabling them for virtual cargo.

A container that already owns separate remote storage should not be treated as ordinary nested cargo inside another virtual-storage provider unless that integration has explicit identity handling.

## Source layout

The GitHub Ready package contains the public source for the DayZ mod, native hosts, Admin Panel, and server manager.

Important entry points include:

- `Source/Mod` for the DayZ mod source
- `Source/Daemon` for ClippyStorageHost
- `Source/AdminHost` for ClippyAdminHost
- `Source/AdminWeb` for the Admin Panel frontend
- `ClippyServerManager.ps1` for server setup and lifecycle management

Release history belongs in `CHANGELOG.md`, Git commits, and GitHub release notes.

## Security

The Admin Panel accepts loopback access only by default and uses session, CSRF, Host, Origin, request-size, and idle-shutdown controls.

The private DayZ signing key is not included in public GitHub, Workshop, or Server Ready packages.

## License

Clippy Virtual Cargo is released under the MIT License. See `LICENSE.txt`.
