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
    int max_age_days = 14;
    int max_total_size_mb = 10240;
    bool prune_on_startup = false;
};

struct ScheduleConfig {
    bool enabled = false;
    std::string mode = "interval";
    int interval_minutes = 180;
    std::string cron = "0 */3 * * *";
    bool run_on_startup = false;
    bool skip_when_no_players = false;
};

struct NotificationConfig {
    bool broadcast = true;
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
    bool verify_archive_after_creation = true;
    bool keep_staging_on_failure = false;
    bool prune_after_backup = true;
    bool write_manifest = true;
    std::string name_template = "backup_${timestamp}_${trigger}";
    std::vector<BackupTargetConfig> targets;
    std::vector<std::string> exclude_patterns;
    RetentionConfig retention;
    ScheduleConfig schedule;
    NotificationConfig notifications;
};

BackupConfig makeDefaultConfig();
void writeDefaultConfig(const std::filesystem::path &path);
BackupConfig loadConfig(const std::filesystem::path &path);

}  // namespace backupper
