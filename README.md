# Vaultstone

`Vaultstone` is a native C++ hot-backup plugin for Endstone Bedrock servers on Linux. It coordinates `save hold`, `save query`, and `save resume`, snapshots the configured server data, writes a self-contained backup, and exposes in-game admin commands for manual backups, scheduling, pruning, deletion, and restore.

## Features

- Native C++ Endstone plugin with no external backup services
- JSON configuration generated automatically on first start
- Hot backups using Bedrock save coordination commands
- In-process `zip`, `tar`, `tar.gz`, and `directory` backup formats
- Configurable targets and glob-style exclude patterns
- Retention controls for count, age, and total backup size
- Manual admin commands for backup, restore, reload, list, prune, delete, and scheduler control
- Interval, cron, and fixed local-time scheduling
- Optional interval reset behavior for manual backups, restores, and server restarts
- Free-space guard before backup creation
- Optional safety backup before restore
- In-plugin restore flow with manifest validation and optional shutdown
- Sender-only or broadcast notifications
- Portable Debian 12 release build flow for broad Linux compatibility

## Quick Start

### Build

For a local development build on a Linux host with `apt` available:

```bash
./scripts/setup-local-libcxx.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

For a portable release build:

```bash
./scripts/build-plugin-debian12.sh
```

Release artifact:

```text
build-debian12/vaultstone.so
```

### Install

1. Copy the built `.so` into the server's `plugins/` directory.
2. Start the server once so the plugin can generate `plugins/vaultstone/config.json`.
3. Review the config and adjust backup targets, schedule, and retention.
4. Reload or restart the server.

### First Backup

Run one manual backup in game or from console:

```text
/vaultstone create first-run
```

Useful follow-up commands:

```text
/vaultstone status
/vaultstone list
/vaultstone schedule start
```

## Commands

| Command | Description |
| --- | --- |
| `/vaultstone status` | Show plugin status, running operations, next scheduled run, and last error |
| `/vaultstone create [label]` | Start a manual backup |
| `/vaultstone restore <name|latest>` | Restore a stored backup |
| `/vaultstone reload` | Reload `config.json` |
| `/vaultstone list [limit]` | List stored backups |
| `/vaultstone prune` | Apply retention rules immediately |
| `/vaultstone delete <name>` | Delete a specific backup |
| `/vaultstone schedule status|start|stop` | Inspect or control the scheduler |

Alias:

- `/backup`

## Permissions

| Permission | Default | Description |
| --- | --- | --- |
| `vaultstone.command` | inherits children | Access to all Vaultstone commands |
| `vaultstone.command.status` | operator | View status |
| `vaultstone.command.create` | operator | Start manual backups |
| `vaultstone.command.reload` | operator | Reload config |
| `vaultstone.command.restore` | operator | Restore backups |
| `vaultstone.command.list` | operator | List backups |
| `vaultstone.command.prune` | operator | Run pruning |
| `vaultstone.command.delete` | operator | Delete specific backups |
| `vaultstone.command.schedule` | operator | Start, stop, or inspect scheduling |

## Restore Safety

Restore is destructive. It removes current target paths and replaces them with the contents from the selected backup.

Recommended restore practice:

- make sure all players are offline
- leave `restore.require_no_players` enabled
- leave `restore.create_backup_before_restore` enabled
- verify the target backup with `/vaultstone list`
- expect the server to shut down after restore if `restore.shutdown_after_restore` is enabled

## Archive Formats

| Format | Best For | Notes |
| --- | --- | --- |
| `zip` | General-purpose backups | Good compatibility and easy to inspect |
| `tar` | Fast local backups | Low overhead, larger files than compressed formats |
| `tar.gz` | Better compression with broad Linux support | Slower than `tar`, smaller than `zip` in many cases |
| `directory` | Debugging or external post-processing | Leaves the snapshot unpacked on disk |

Practical recommendation:

- use `zip` if you want the safest all-round default
- use `tar` if backup speed matters more than size
- use `tar.gz` if storage matters and a slower backup is acceptable
- use `directory` only when you explicitly want unpacked backup folders

## Full Example Config

The plugin writes `plugins/vaultstone/config.json` automatically on first load. A full example:

```json
{
  "backup_directory": "backups",
  "temporary_directory": "temp",
  "archive_format": "zip",
  "compression_level": 6,
  "copy_threads": 4,
  "save_query_interval_ticks": 20,
  "save_query_timeout_ticks": 600,
  "minimum_free_space_mb": 0,
  "verify_archive_after_creation": true,
  "keep_staging_on_failure": false,
  "prune_after_backup": true,
  "write_manifest": true,
  "name_template": "backup_${timestamp}_${trigger}",
  "targets": [
    {
      "path": "worlds/${level_name}",
      "required": true
    },
    {
      "path": "server.properties",
      "required": true
    },
    {
      "path": "allowlist.json",
      "required": false
    },
    {
      "path": "permissions.json",
      "required": false
    },
    {
      "path": "endstone.toml",
      "required": false
    }
  ],
  "exclude_patterns": [
    "logs/**",
    "crash_reports/**",
    "backups/**",
    "plugins/.local/**"
  ],
  "retention": {
    "max_backups": 20,
    "min_backups_to_keep": 1,
    "when_at_max_backups": "prune_oldest",
    "max_age_days": 14,
    "max_total_size_mb": 10240,
    "prune_on_startup": false,
    "auto_prune_after_manual_backup": true,
    "auto_prune_after_scheduled_backup": true,
    "auto_prune_after_pre_restore_backup": false
  },
  "schedule": {
    "enabled": false,
    "mode": "interval",
    "interval_minutes": 180,
    "cron": "0 0 */3 * * *",
    "clock_times_local": [
      "03:00:00"
    ],
    "run_on_startup": false,
    "skip_when_no_players": false,
    "only_when_no_players": false,
    "reset_interval_on_manual_backup": true,
    "reset_interval_on_restore": true,
    "reset_interval_on_server_start": true,
    "persist_interval_state": true,
    "catch_up_missed_run_on_startup": false
  },
  "restore": {
    "require_no_players": true,
    "create_backup_before_restore": true,
    "allow_latest_selector": true,
    "allow_named_restore": true,
    "shutdown_after_restore": true,
    "shutdown_delay_seconds": 3,
    "prune_backups_after_restore": false,
    "success_message": "Restore from ${backup_name} finished. Server shutdown in ${shutdown_delay_seconds} seconds.",
    "failure_message": "Restore from ${backup_name} failed: ${error}"
  },
  "notifications": {
    "broadcast": true,
    "notify_command_sender_only": false,
    "countdown_seconds": 5,
    "start_message": "Backup starting in ${countdown_seconds} seconds.",
    "copy_message": "Backup in progress. World writes are paused for a moment.",
    "success_message": "Backup ${backup_name} completed in ${duration}.",
    "failure_message": "Backup ${backup_name} failed: ${error}"
  }
}
```

## Config Reference

### Top Level

| Key | Default | Description |
| --- | --- | --- |
| `backup_directory` | `backups` | Folder, relative to the server root, where backups are stored |
| `temporary_directory` | `temp` | Plugin-managed staging folder |
| `archive_format` | `zip` | `zip`, `tar`, `tar.gz`, or `directory` |
| `compression_level` | `6` | Compression level for compressed archive formats |
| `copy_threads` | `4` | Worker threads used while copying snapshot files |
| `save_query_interval_ticks` | `20` | Delay between `save query` checks |
| `save_query_timeout_ticks` | `600` | Maximum wait for Bedrock save readiness |
| `minimum_free_space_mb` | `0` | Refuse backup start if available disk space is below this value |
| `verify_archive_after_creation` | `true` | Reopen and validate compressed archives after writing |
| `keep_staging_on_failure` | `false` | Preserve the staging directory after failed backups |
| `prune_after_backup` | `true` | Allow automatic prune logic after backups |
| `write_manifest` | `true` | Embed `vaultstone-manifest.json` in each backup |
| `name_template` | `backup_${timestamp}_${trigger}` | Backup name template |
| `targets` | see example | Files and folders to include |
| `exclude_patterns` | see example | Glob-style exclusions relative to server root |

Template variables available in messages and names:

- `${backup_name}`
- `${countdown_seconds}`
- `${date}`
- `${duration}`
- `${error}`
- `${label}`
- `${level_name}`
- `${requested_by}`
- `${shutdown_delay_seconds}`
- `${time}`
- `${timestamp}`
- `${trigger}`

### `retention`

| Key | Default | Description |
| --- | --- | --- |
| `max_backups` | `20` | Preferred maximum number of retained backups |
| `min_backups_to_keep` | `1` | Hard safety floor pruning will not cross |
| `when_at_max_backups` | `prune_oldest` | `prune_oldest`, `refuse_new_backup`, or `delete_newest_existing` |
| `max_age_days` | `14` | Remove backups older than this age when pruning |
| `max_total_size_mb` | `10240` | Remove older backups when total size exceeds this limit |
| `prune_on_startup` | `false` | Run pruning on plugin startup |
| `auto_prune_after_manual_backup` | `true` | Prune automatically after manual backups |
| `auto_prune_after_scheduled_backup` | `true` | Prune automatically after scheduled backups |
| `auto_prune_after_pre_restore_backup` | `false` | Prune automatically after safety backups created before restore |

### `schedule`

| Key | Default | Description |
| --- | --- | --- |
| `enabled` | `false` | Enable automatic scheduling |
| `mode` | `interval` | `interval`, `cron`, or `clock` |
| `interval_minutes` | `180` | Backup interval in minutes for `interval` mode |
| `cron` | `0 0 */3 * * *` | Cron expression for `cron` mode |
| `clock_times_local` | `["03:00:00"]` | One or more local times for `clock` mode |
| `run_on_startup` | `false` | Queue a backup shortly after plugin startup |
| `skip_when_no_players` | `false` | Skip scheduled backups while the server is empty |
| `only_when_no_players` | `false` | Run scheduled backups only while the server is empty |
| `reset_interval_on_manual_backup` | `true` | Restart the interval timer after manual backups |
| `reset_interval_on_restore` | `true` | Restart the interval timer after restore |
| `reset_interval_on_server_start` | `true` | Restart the interval timer after server start |
| `persist_interval_state` | `true` | Save the next interval run to disk |
| `catch_up_missed_run_on_startup` | `false` | Run one missed interval backup after startup |

### `restore`

| Key | Default | Description |
| --- | --- | --- |
| `require_no_players` | `true` | Block restore while players are online |
| `create_backup_before_restore` | `true` | Create a safety backup before restore |
| `allow_latest_selector` | `true` | Allow `/vaultstone restore latest` |
| `allow_named_restore` | `true` | Allow restore by backup name |
| `shutdown_after_restore` | `true` | Shut the server down after a successful restore |
| `shutdown_delay_seconds` | `3` | Delay before shutdown after a successful restore |
| `prune_backups_after_restore` | `false` | Run pruning after restore |
| `success_message` | built-in message | Message shown after successful restore |
| `failure_message` | built-in message | Message shown after failed restore |

### `notifications`

| Key | Default | Description |
| --- | --- | --- |
| `broadcast` | `true` | Broadcast backup lifecycle messages |
| `notify_command_sender_only` | `false` | Send manual command completion only to the requester |
| `countdown_seconds` | `5` | Delay used by start-message templates |
| `start_message` | built-in message | Message sent before a backup starts |
| `copy_message` | built-in message | Message sent while world writes are paused |
| `success_message` | built-in message | Message sent after a successful backup |
| `failure_message` | built-in message | Message sent after a failed backup |

## Recommended Presets

### Small Server

Good if you want simple scheduled protection without a huge config change:

```json
{
  "archive_format": "zip",
  "retention": {
    "max_backups": 12,
    "max_age_days": 7
  },
  "schedule": {
    "enabled": true,
    "mode": "interval",
    "interval_minutes": 180
  }
}
```

### Fast Local Backups

Good when backup speed matters more than archive size:

```json
{
  "archive_format": "tar",
  "compression_level": 0,
  "schedule": {
    "enabled": true,
    "mode": "interval",
    "interval_minutes": 60
  }
}
```

### Daily Archival

Good when you care more about storage efficiency than backup speed:

```json
{
  "archive_format": "tar.gz",
  "compression_level": 9,
  "retention": {
    "max_backups": 30,
    "max_age_days": 30
  },
  "schedule": {
    "enabled": true,
    "mode": "clock",
    "clock_times_local": [
      "03:00:00"
    ]
  }
}
```

### Safer Restore-Focused Setup

Good if restore safety matters more than convenience:

```json
{
  "restore": {
    "require_no_players": true,
    "create_backup_before_restore": true,
    "allow_latest_selector": false,
    "allow_named_restore": true,
    "shutdown_after_restore": true,
    "shutdown_delay_seconds": 5
  },
  "notifications": {
    "notify_command_sender_only": true
  }
}
```

## Default Backup Targets

By default the plugin includes:

- `worlds/${level_name}`
- `server.properties`
- `allowlist.json`
- `permissions.json`
- `endstone.toml`

By default the plugin excludes:

- `logs/**`
- `crash_reports/**`
- `backups/**`
- `plugins/.local/**`

## Compatibility and Testing

Built against the Endstone `v0.11.2` SDK in CMake.

Runtime validation performed so far:

- loaded successfully on an Endstone `0.11.1` test server running Debian 12
- manual backup flow with real `save hold`, `save query`, and `save resume`
- automatic interval scheduling
- cron scheduling with `*/15 * * * * *`
- fixed local-time scheduling with `clock_times_local`
- real backup creation, listing, deletion, pruning, and reload commands
- restore of a real backup with `permissions.json` rollback verification
- `zip` and `directory` backups validated on the server
- `tar` and `tar.gz` archive write/extract round-trip validated with a local archive-layer smoke test
- free-space refusal path
- sender-only notifications
- safety backup before restore
- interval reset behavior for manual backups and server restarts

Not yet broadly validated:

- every Endstone runtime version
- every Linux distribution and libc combination
- very large worlds under long-running production load

## Install Notes

- The plugin creates `plugins/vaultstone/config.json` automatically if it does not exist.
- Backup paths in `targets` are relative to the server root.
- Backups are stored relative to the server root, not the plugin data folder.
- The plugin keeps all backup logic inside the `.so`; it does not require another service or container.
