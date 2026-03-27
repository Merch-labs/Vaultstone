# Endstone Backupper

`Endstone Backupper` is a C++ hot-backup plugin for Endstone Bedrock servers on Linux. It coordinates `save hold`, `save query`, and `save resume`, snapshots the configured server data, writes a self-contained archive, and gives admins in-game controls for manual runs and schedule management.

## Features

- Native C++ Endstone plugin with no external runtime services
- JSON configuration generated automatically on first start
- Hot backups using Bedrock's save coordination commands
- ZIP, TAR, and TAR.GZ archives written directly in-process, plus directory-format backups when preferred
- Configurable include targets and exclude patterns
- Retention controls for count, age, and total backup size
- Free-space guard before backup creation
- Manual admin commands for backup, restore, reload, listing, pruning, deleting, and schedule control
- Interval, cron, or fixed local-time scheduling
- Optional interval reset behavior for manual backups, restores, and server restarts
- In-plugin restore flow with safe extraction, manifest validation, and optional post-restore shutdown
- Optional safety backup before restore and sender-only notifications
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
- `archive_format`: `zip`, `tar`, `tar.gz`, or `directory`
- `minimum_free_space_mb`: refuse to start when disk space falls below the configured floor
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
- `schedule.only_when_no_players`: run scheduled backups only while the server is empty

Useful retention options:

- `retention.min_backups_to_keep`: hard floor that pruning will not cross
- `retention.when_at_max_backups`: choose `prune_oldest`, `refuse_new_backup`, or `delete_newest_existing`
  `prune_oldest` keeps the current behavior, `refuse_new_backup` aborts a new backup when the cap is already reached, and `delete_newest_existing` evicts the newest existing backup before writing the new one
- `retention.auto_prune_after_manual_backup`: prune automatically after manual backups
- `retention.auto_prune_after_scheduled_backup`: prune automatically after scheduled backups
- `retention.auto_prune_after_pre_restore_backup`: prune automatically after safety backups taken before restore

Useful restore options:

- `restore.create_backup_before_restore`: create a safety backup immediately before applying a restore
- `restore.allow_latest_selector`: allow or block `/backupper restore latest`
- `restore.allow_named_restore`: allow or block named restores

Useful notification options:

- `notifications.notify_command_sender_only`: send success and failure messages only to the command sender for manual actions

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
- creating and validating ZIP and directory-format backups
- round-tripping `tar` and `tar.gz` archives through the in-process archive layer with a local smoke test
- restoring a real backup with `/backupper restore latest` and verifying `permissions.json` was rolled back successfully
- verifying a configured free-space floor blocks manual backups
- verifying sender-only notifications still report completion to the command sender
- verifying `restore.create_backup_before_restore` writes a `pre_restore` backup before the restore is applied
