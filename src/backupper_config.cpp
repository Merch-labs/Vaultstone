#include "backupper_config.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace backupper {
namespace {

BackupTargetConfig readTarget(const json &value)
{
    if (!value.is_object()) {
        throw std::runtime_error("Each entry in targets must be an object.");
    }

    BackupTargetConfig target;
    target.path = value.value("path", "");
    target.required = value.value("required", true);
    if (target.path.empty()) {
        throw std::runtime_error("Backup target path cannot be empty.");
    }
    return target;
}

RetentionConfig readRetention(const json &value, const RetentionConfig &defaults)
{
    RetentionConfig retention = defaults;
    if (!value.is_object()) {
        return retention;
    }
    retention.max_backups = value.value("max_backups", retention.max_backups);
    retention.max_age_days = value.value("max_age_days", retention.max_age_days);
    retention.max_total_size_mb = value.value("max_total_size_mb", retention.max_total_size_mb);
    retention.prune_on_startup = value.value("prune_on_startup", retention.prune_on_startup);
    return retention;
}

ScheduleConfig readSchedule(const json &value, const ScheduleConfig &defaults)
{
    ScheduleConfig schedule = defaults;
    if (!value.is_object()) {
        return schedule;
    }
    schedule.enabled = value.value("enabled", schedule.enabled);
    schedule.mode = value.value("mode", schedule.mode);
    schedule.interval_minutes = value.value("interval_minutes", schedule.interval_minutes);
    schedule.cron = value.value("cron", schedule.cron);
    schedule.run_on_startup = value.value("run_on_startup", schedule.run_on_startup);
    schedule.skip_when_no_players = value.value("skip_when_no_players", schedule.skip_when_no_players);
    return schedule;
}

NotificationConfig readNotifications(const json &value, const NotificationConfig &defaults)
{
    NotificationConfig notifications = defaults;
    if (!value.is_object()) {
        return notifications;
    }
    notifications.broadcast = value.value("broadcast", notifications.broadcast);
    notifications.countdown_seconds = value.value("countdown_seconds", notifications.countdown_seconds);
    notifications.start_message = value.value("start_message", notifications.start_message);
    notifications.copy_message = value.value("copy_message", notifications.copy_message);
    notifications.success_message = value.value("success_message", notifications.success_message);
    notifications.failure_message = value.value("failure_message", notifications.failure_message);
    return notifications;
}

json toJson(const BackupConfig &config)
{
    json targets = json::array();
    for (const auto &target : config.targets) {
        targets.push_back({{"path", target.path}, {"required", target.required}});
    }

    return {
        {"backup_directory", config.backup_directory},
        {"temporary_directory", config.temporary_directory},
        {"archive_format", config.archive_format},
        {"compression_level", config.compression_level},
        {"copy_threads", config.copy_threads},
        {"save_query_interval_ticks", config.save_query_interval_ticks},
        {"save_query_timeout_ticks", config.save_query_timeout_ticks},
        {"verify_archive_after_creation", config.verify_archive_after_creation},
        {"keep_staging_on_failure", config.keep_staging_on_failure},
        {"prune_after_backup", config.prune_after_backup},
        {"write_manifest", config.write_manifest},
        {"name_template", config.name_template},
        {"targets", targets},
        {"exclude_patterns", config.exclude_patterns},
        {"retention",
         {{"max_backups", config.retention.max_backups},
          {"max_age_days", config.retention.max_age_days},
          {"max_total_size_mb", config.retention.max_total_size_mb},
          {"prune_on_startup", config.retention.prune_on_startup}}},
        {"schedule",
         {{"enabled", config.schedule.enabled},
          {"mode", config.schedule.mode},
          {"interval_minutes", config.schedule.interval_minutes},
          {"cron", config.schedule.cron},
          {"run_on_startup", config.schedule.run_on_startup},
          {"skip_when_no_players", config.schedule.skip_when_no_players}}},
        {"notifications",
         {{"broadcast", config.notifications.broadcast},
          {"countdown_seconds", config.notifications.countdown_seconds},
          {"start_message", config.notifications.start_message},
          {"copy_message", config.notifications.copy_message},
          {"success_message", config.notifications.success_message},
          {"failure_message", config.notifications.failure_message}}},
    };
}

void validateConfig(const BackupConfig &config)
{
    if (config.archive_format != "zip" && config.archive_format != "directory") {
        throw std::runtime_error("archive_format must be either 'zip' or 'directory'.");
    }
    if (config.compression_level < 0 || config.compression_level > 9) {
        throw std::runtime_error("compression_level must be between 0 and 9.");
    }
    if (config.copy_threads < 1 || config.copy_threads > 64) {
        throw std::runtime_error("copy_threads must be between 1 and 64.");
    }
    if (config.save_query_interval_ticks < 1) {
        throw std::runtime_error("save_query_interval_ticks must be at least 1.");
    }
    if (config.save_query_timeout_ticks < config.save_query_interval_ticks) {
        throw std::runtime_error("save_query_timeout_ticks must be greater than save_query_interval_ticks.");
    }
    if (config.targets.empty()) {
        throw std::runtime_error("At least one backup target is required.");
    }
}

}  // namespace

BackupConfig makeDefaultConfig()
{
    BackupConfig config;
    config.targets = {
        {"worlds/${level_name}", true},
        {"server.properties", true},
        {"allowlist.json", false},
        {"permissions.json", false},
        {"endstone.toml", false},
    };
    config.exclude_patterns = {
        "logs/**",
        "crash_reports/**",
        "backups/**",
        "plugins/.local/**",
    };
    return config;
}

void writeDefaultConfig(const std::filesystem::path &path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << toJson(makeDefaultConfig()).dump(4) << '\n';
}

BackupConfig loadConfig(const std::filesystem::path &path)
{
    if (!std::filesystem::exists(path)) {
        writeDefaultConfig(path);
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to open config file: " + path.string());
    }

    json document = json::parse(input);
    BackupConfig config = makeDefaultConfig();

    config.backup_directory = document.value("backup_directory", config.backup_directory);
    config.temporary_directory = document.value("temporary_directory", config.temporary_directory);
    config.archive_format = document.value("archive_format", config.archive_format);
    config.compression_level = document.value("compression_level", config.compression_level);
    config.copy_threads = document.value("copy_threads", config.copy_threads);
    config.save_query_interval_ticks = document.value("save_query_interval_ticks", config.save_query_interval_ticks);
    config.save_query_timeout_ticks = document.value("save_query_timeout_ticks", config.save_query_timeout_ticks);
    config.verify_archive_after_creation =
        document.value("verify_archive_after_creation", config.verify_archive_after_creation);
    config.keep_staging_on_failure = document.value("keep_staging_on_failure", config.keep_staging_on_failure);
    config.prune_after_backup = document.value("prune_after_backup", config.prune_after_backup);
    config.write_manifest = document.value("write_manifest", config.write_manifest);
    config.name_template = document.value("name_template", config.name_template);

    if (document.contains("targets")) {
        config.targets.clear();
        for (const auto &value : document.at("targets")) {
            config.targets.push_back(readTarget(value));
        }
    }

    if (document.contains("exclude_patterns")) {
        config.exclude_patterns = document.at("exclude_patterns").get<std::vector<std::string>>();
    }

    config.retention = readRetention(document.value("retention", json::object()), config.retention);
    config.schedule = readSchedule(document.value("schedule", json::object()), config.schedule);
    config.notifications = readNotifications(document.value("notifications", json::object()), config.notifications);

    validateConfig(config);
    return config;
}

}  // namespace backupper
