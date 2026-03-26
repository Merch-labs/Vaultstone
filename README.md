# Endstone Backupper

`Endstone Backupper` is a C++ hot-backup plugin for Endstone Bedrock servers on Linux. It coordinates `save hold`, `save query`, and `save resume`, snapshots the configured server data, writes a self-contained archive, and gives admins in-game controls for manual runs and schedule management.

## Features

- Native C++ Endstone plugin with no external runtime services
- JSON configuration generated automatically on first start
- Hot backups using Bedrock's save coordination commands
- ZIP archives written directly in-process, plus directory-format backups when preferred
- Configurable include targets and exclude patterns
- Retention controls for count, age, and total backup size
- Manual admin commands for backup, restore, reload, listing, pruning, deleting, and schedule control
- Interval, cron, or fixed local-time scheduling
- Optional interval reset behavior for manual backups, restores, and server restarts
- In-plugin restore flow with safe extraction, manifest validation, and optional post-restore shutdown
- Debian 12 release build script for broad Linux compatibility

## Commands

- `/backupper status`
- `/backupper backup [label]`
- `/backupper restore <name|latest>`
- `/backupper reload`
- `/backupper list [limit]`
- `/backupper prune`
- `/backupper delete <name>`
- `/backupper schedule <status|start|stop>`

The command alias `/backup` is also registered.

## Config

The plugin writes its config to `plugins/backupper/config.json`.

Important sections:

- `targets`: files and directories to include in each backup
- `exclude_patterns`: glob-style filters applied to relative paths
- `archive_format`: `zip` or `directory`
- `retention`: max backups, max age, and max total size
- `schedule`: interval, cron, or fixed-time scheduling, plus interval persistence/reset behavior
- `restore`: restore safety checks, shutdown behavior, and restore messages
- `notifications`: broadcast text for backup lifecycle messages

Useful schedule options:

- `schedule.mode`: `interval`, `cron`, or `clock`
- `schedule.interval_minutes`: repeat interval for `interval` mode
- `schedule.cron`: cron expression for `cron` mode
- `schedule.clock_times_local`: one or more local `HH:MM[:SS]` times for `clock` mode
- `schedule.reset_interval_on_manual_backup`: whether a manual backup restarts the interval timer
- `schedule.reset_interval_on_restore`: whether a restore restarts the interval timer
- `schedule.reset_interval_on_server_start`: whether restarting the server restarts the interval timer
- `schedule.persist_interval_state`: whether the next interval run is written to disk
- `schedule.catch_up_missed_run_on_startup`: optionally run one missed interval backup after startup

The default config backs up:

- `worlds/${level_name}`
- `server.properties`
- `allowlist.json`
- `permissions.json`
- `endstone.toml`

## Build

For a local development build on a Linux host with apt available:

```bash
./scripts/setup-local-libcxx.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

For a portable Debian 12 release build:

```bash
./scripts/build-plugin-debian12.sh
```

The compatible release artifact is written to:

```text
build-debian12/endstone_endstone_backupper.so
```

## Install

Copy the built `.so` into your Endstone `plugins/` directory and restart the server. The plugin will create `plugins/backupper/config.json` automatically on first load.

## Validation

The plugin was validated against an Endstone 0.11.1 Debian 12 server by:

- loading the compiled plugin in the server container
- running `/backupper status`
- running `/backupper backup smoke-test`
- verifying the created ZIP archive and embedded `backupper-manifest.json`
- running `/backupper list`, `/backupper reload`, and `/backupper delete`
- confirming `reset_interval_on_manual_backup` on and off with live interval schedule checks
- confirming `reset_interval_on_server_start` on and off across real server restarts
- running cron scheduling live with `*/15 * * * * *`
- running fixed local-time scheduling live with `clock_times_local`
- creating and validating both ZIP and directory-format backups
- restoring a real backup with `/backupper restore latest` and verifying `permissions.json` was rolled back successfully
