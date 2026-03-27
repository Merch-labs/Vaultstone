#include "backupper_config.h"

#include "archive_writer.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <croncpp.h>
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
    retention.min_backups_to_keep = value.value("min_backups_to_keep", retention.min_backups_to_keep);
    retention.when_at_max_backups = value.value("when_at_max_backups", retention.when_at_max_backups);
    retention.max_age_days = value.value("max_age_days", retention.max_age_days);
    retention.max_total_size_mb = value.value("max_total_size_mb", retention.max_total_size_mb);
    retention.prune_on_startup = value.value("prune_on_startup", retention.prune_on_startup);
    retention.auto_prune_after_manual_backup =
        value.value("auto_prune_after_manual_backup", retention.auto_prune_after_manual_backup);
    retention.auto_prune_after_scheduled_backup =
        value.value("auto_prune_after_scheduled_backup", retention.auto_prune_after_scheduled_backup);
    retention.auto_prune_after_pre_restore_backup =
        value.value("auto_prune_after_pre_restore_backup", retention.auto_prune_after_pre_restore_backup);
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
    if (value.contains("clock_times_local")) {
        schedule.clock_times_local = value.at("clock_times_local").get<std::vector<std::string>>();
    }
    schedule.run_on_startup = value.value("run_on_startup", schedule.run_on_startup);
    schedule.skip_when_no_players = value.value("skip_when_no_players", schedule.skip_when_no_players);
    schedule.only_when_no_players = value.value("only_when_no_players", schedule.only_when_no_players);
    schedule.reset_interval_on_manual_backup =
        value.value("reset_interval_on_manual_backup", schedule.reset_interval_on_manual_backup);
    schedule.reset_interval_on_restore = value.value("reset_interval_on_restore", schedule.reset_interval_on_restore);
    schedule.reset_interval_on_server_start =
        value.value("reset_interval_on_server_start", schedule.reset_interval_on_server_start);
    schedule.persist_interval_state = value.value("persist_interval_state", schedule.persist_interval_state);
    schedule.catch_up_missed_run_on_startup =
        value.value("catch_up_missed_run_on_startup", schedule.catch_up_missed_run_on_startup);
    return schedule;
}

RestoreConfig readRestore(const json &value, const RestoreConfig &defaults)
{
    RestoreConfig restore = defaults;
    if (!value.is_object()) {
        return restore;
    }
    restore.require_no_players = value.value("require_no_players", restore.require_no_players);
    restore.create_backup_before_restore = value.value("create_backup_before_restore", restore.create_backup_before_restore);
    restore.allow_latest_selector = value.value("allow_latest_selector", restore.allow_latest_selector);
    restore.allow_named_restore = value.value("allow_named_restore", restore.allow_named_restore);
    restore.shutdown_after_restore = value.value("shutdown_after_restore", restore.shutdown_after_restore);
    restore.shutdown_delay_seconds = value.value("shutdown_delay_seconds", restore.shutdown_delay_seconds);
    restore.prune_backups_after_restore = value.value("prune_backups_after_restore", restore.prune_backups_after_restore);
    restore.success_message = value.value("success_message", restore.success_message);
    restore.failure_message = value.value("failure_message", restore.failure_message);
    return restore;
}

NotificationConfig readNotifications(const json &value, const NotificationConfig &defaults)
{
    NotificationConfig notifications = defaults;
    if (!value.is_object()) {
        return notifications;
    }
    notifications.broadcast = value.value("broadcast", notifications.broadcast);
    notifications.notify_command_sender_only =
        value.value("notify_command_sender_only", notifications.notify_command_sender_only);
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
        {"minimum_free_space_mb", config.minimum_free_space_mb},
        {"verify_archive_after_creation", config.verify_archive_after_creation},
        {"keep_staging_on_failure", config.keep_staging_on_failure},
        {"prune_after_backup", config.prune_after_backup},
        {"write_manifest", config.write_manifest},
        {"name_template", config.name_template},
        {"targets", targets},
        {"exclude_patterns", config.exclude_patterns},
        {"retention",
         {{"max_backups", config.retention.max_backups},
          {"min_backups_to_keep", config.retention.min_backups_to_keep},
          {"when_at_max_backups", config.retention.when_at_max_backups},
          {"max_age_days", config.retention.max_age_days},
          {"max_total_size_mb", config.retention.max_total_size_mb},
          {"prune_on_startup", config.retention.prune_on_startup},
          {"auto_prune_after_manual_backup", config.retention.auto_prune_after_manual_backup},
          {"auto_prune_after_scheduled_backup", config.retention.auto_prune_after_scheduled_backup},
          {"auto_prune_after_pre_restore_backup", config.retention.auto_prune_after_pre_restore_backup}}},
        {"schedule",
         {{"enabled", config.schedule.enabled},
          {"mode", config.schedule.mode},
          {"interval_minutes", config.schedule.interval_minutes},
          {"cron", config.schedule.cron},
          {"clock_times_local", config.schedule.clock_times_local},
          {"run_on_startup", config.schedule.run_on_startup},
          {"skip_when_no_players", config.schedule.skip_when_no_players},
          {"only_when_no_players", config.schedule.only_when_no_players},
          {"reset_interval_on_manual_backup", config.schedule.reset_interval_on_manual_backup},
          {"reset_interval_on_restore", config.schedule.reset_interval_on_restore},
          {"reset_interval_on_server_start", config.schedule.reset_interval_on_server_start},
          {"persist_interval_state", config.schedule.persist_interval_state},
          {"catch_up_missed_run_on_startup", config.schedule.catch_up_missed_run_on_startup}}},
        {"restore",
         {{"require_no_players", config.restore.require_no_players},
          {"create_backup_before_restore", config.restore.create_backup_before_restore},
          {"allow_latest_selector", config.restore.allow_latest_selector},
          {"allow_named_restore", config.restore.allow_named_restore},
          {"shutdown_after_restore", config.restore.shutdown_after_restore},
          {"shutdown_delay_seconds", config.restore.shutdown_delay_seconds},
          {"prune_backups_after_restore", config.restore.prune_backups_after_restore},
          {"success_message", config.restore.success_message},
          {"failure_message", config.restore.failure_message}}},
        {"notifications",
         {{"broadcast", config.notifications.broadcast},
          {"notify_command_sender_only", config.notifications.notify_command_sender_only},
          {"countdown_seconds", config.notifications.countdown_seconds},
          {"start_message", config.notifications.start_message},
          {"copy_message", config.notifications.copy_message},
          {"success_message", config.notifications.success_message},
          {"failure_message", config.notifications.failure_message}}},
    };
}

void validateConfig(const BackupConfig &config)
{
    if (!isSupportedArchiveFormat(config.archive_format)) {
        throw std::runtime_error("archive_format must be one of 'zip', 'tar', 'tar.gz', or 'directory'.");
    }
    if (config.compression_level < 0 || config.compression_level > 9) {
        throw std::runtime_error("compression_level must be between 0 and 9.");
    }
    if (config.minimum_free_space_mb < 0) {
        throw std::runtime_error("minimum_free_space_mb must be at least 0.");
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
    if (config.retention.min_backups_to_keep < 0) {
        throw std::runtime_error("retention.min_backups_to_keep must be at least 0.");
    }
    if (config.retention.when_at_max_backups != "prune_oldest" &&
        config.retention.when_at_max_backups != "refuse_new_backup" &&
        config.retention.when_at_max_backups != "delete_newest_existing") {
        throw std::runtime_error(
            "retention.when_at_max_backups must be 'prune_oldest', 'refuse_new_backup', or 'delete_newest_existing'.");
    }
    if (config.retention.max_backups > 0 && config.retention.min_backups_to_keep > config.retention.max_backups) {
        throw std::runtime_error("retention.min_backups_to_keep cannot be greater than retention.max_backups.");
    }
    if (config.schedule.mode != "interval" && config.schedule.mode != "clock" && config.schedule.mode != "cron") {
        throw std::runtime_error("schedule.mode must be one of 'interval', 'clock', or 'cron'.");
    }
    if (config.schedule.mode == "interval" && config.schedule.interval_minutes < 1) {
        throw std::runtime_error("schedule.interval_minutes must be at least 1 when mode is 'interval'.");
    }
    if (config.schedule.skip_when_no_players && config.schedule.only_when_no_players) {
        throw std::runtime_error("schedule.skip_when_no_players and schedule.only_when_no_players cannot both be true.");
    }
    if (config.schedule.mode == "clock" && config.schedule.clock_times_local.empty()) {
        throw std::runtime_error("schedule.clock_times_local must contain at least one time when mode is 'clock'.");
    }
    if (config.schedule.mode == "clock") {
        for (const auto &value : config.schedule.clock_times_local) {
            std::istringstream stream(value);
            std::string hour_text;
            std::string minute_text;
            std::string second_text;
            if (!std::getline(stream, hour_text, ':') || !std::getline(stream, minute_text, ':')) {
                throw std::runtime_error("schedule.clock_times_local entries must use HH:MM or HH:MM:SS.");
            }
            if (!std::getline(stream, second_text)) {
                second_text = "0";
            }
            const int hour = std::stoi(hour_text);
            const int minute = std::stoi(minute_text);
            const int second = std::stoi(second_text);
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
                throw std::runtime_error("schedule.clock_times_local entries must be valid local times.");
            }
        }
    }
    if (config.schedule.mode == "cron") {
        try {
            static_cast<void>(cron::make_cron(config.schedule.cron));
        }
        catch (const std::exception &exception) {
            throw std::runtime_error(std::string("schedule.cron is invalid: ") + exception.what());
        }
    }
    if (config.restore.shutdown_delay_seconds < 0 || config.restore.shutdown_delay_seconds > 3600) {
        throw std::runtime_error("restore.shutdown_delay_seconds must be between 0 and 3600.");
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
    config.minimum_free_space_mb = document.value("minimum_free_space_mb", config.minimum_free_space_mb);
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
    config.restore = readRestore(document.value("restore", json::object()), config.restore);
    config.notifications = readNotifications(document.value("notifications", json::object()), config.notifications);

    validateConfig(config);
    return config;
}

}  // namespace backupper
