#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace backupper {

struct BackupTargetConfig {
    std::string path;
    bool required = true;
};

struct RetentionConfig {
    int max_backups = 20;
    int min_backups_to_keep = 1;
    std::string when_at_max_backups = "prune_oldest";
    int max_age_days = 14;
    int max_total_size_mb = 10240;
    bool prune_on_startup = false;
    bool auto_prune_after_manual_backup = true;
    bool auto_prune_after_scheduled_backup = true;
    bool auto_prune_after_pre_restore_backup = false;
};

struct ScheduleConfig {
    bool enabled = false;
    std::string mode = "interval";
    int interval_minutes = 180;
    std::string cron = "0 0 */3 * * *";
    std::vector<std::string> clock_times_local = {"03:00:00"};
    bool run_on_startup = false;
    bool skip_when_no_players = false;
    bool only_when_no_players = false;
    bool reset_interval_on_manual_backup = true;
    bool reset_interval_on_restore = true;
    bool reset_interval_on_server_start = true;
    bool persist_interval_state = true;
    bool catch_up_missed_run_on_startup = false;
};

struct RestoreConfig {
    bool require_no_players = true;
    bool create_backup_before_restore = true;
    bool allow_latest_selector = true;
    bool allow_named_restore = true;
    bool shutdown_after_restore = true;
    int shutdown_delay_seconds = 3;
    bool prune_backups_after_restore = false;
    std::string success_message = "Restore from ${backup_name} finished. Server shutdown in ${shutdown_delay_seconds} seconds.";
    std::string failure_message = "Restore from ${backup_name} failed: ${error}";
};

struct NotificationConfig {
    bool broadcast = true;
    bool notify_command_sender_only = false;
    int countdown_seconds = 5;
    std::string start_message = "Backup starting in ${countdown_seconds} seconds.";
    std::string copy_message = "Backup in progress. World writes are paused for a moment.";
    std::string success_message = "Backup ${backup_name} completed in ${duration}.";
    std::string failure_message = "Backup ${backup_name} failed: ${error}";
};

struct BackupConfig {
    std::string backup_directory = "backups";
    std::string temporary_directory = "temp";
    std::string archive_format = "zip";
    int compression_level = 6;
    int copy_threads = 4;
    int save_query_interval_ticks = 20;
    int save_query_timeout_ticks = 600;
    int minimum_free_space_mb = 0;
    bool verify_archive_after_creation = true;
    bool keep_staging_on_failure = false;
    bool prune_after_backup = true;
    bool write_manifest = true;
    std::string name_template = "backup_${timestamp}_${trigger}";
    std::vector<BackupTargetConfig> targets;
    std::vector<std::string> exclude_patterns;
    RetentionConfig retention;
    ScheduleConfig schedule;
    RestoreConfig restore;
    NotificationConfig notifications;
};

BackupConfig makeDefaultConfig();
void writeDefaultConfig(const std::filesystem::path &path);
BackupConfig loadConfig(const std::filesystem::path &path);

}  // namespace backupper
