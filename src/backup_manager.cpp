#include "backup_manager.h"

#include "archive_writer.h"
#include "backupper_plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>

#include <endstone/command/command_sender_wrapper.h>
#include <croncpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace backupper {
namespace {

std::string trim(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string timePointToFileTime(const std::chrono::system_clock::time_point value)
{
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm broken_down{};
    localtime_r(&time, &broken_down);

    std::ostringstream output;
    output << std::put_time(&broken_down, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

std::string timePointToCompactTimestamp(const std::chrono::system_clock::time_point value)
{
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm broken_down{};
    localtime_r(&time, &broken_down);

    std::ostringstream output;
    output << std::put_time(&broken_down, "%Y%m%d-%H%M%S");
    return output.str();
}

std::int64_t timePointToUnixSeconds(const std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point unixSecondsToTimePoint(const std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

int parseClockTimeString(const std::string &value)
{
    std::istringstream stream(value);
    std::string hour_text;
    std::string minute_text;
    std::string second_text;

    if (!std::getline(stream, hour_text, ':') || !std::getline(stream, minute_text, ':')) {
        throw std::runtime_error("Clock times must use HH:MM or HH:MM:SS format.");
    }

    if (!std::getline(stream, second_text)) {
        second_text = "0";
    }

    const int hour = std::stoi(hour_text);
    const int minute = std::stoi(minute_text);
    const int second = std::stoi(second_text);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        throw std::runtime_error("Clock times must be within 00:00:00 and 23:59:59.");
    }

    return (hour * 3600) + (minute * 60) + second;
}

std::regex globToRegex(const std::string &pattern)
{
    std::string output = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            const bool double_star = (i + 1 < pattern.size() && pattern[i + 1] == '*');
            if (double_star) {
                output += ".*";
                ++i;
            }
            else {
                output += "[^/]*";
            }
            continue;
        }
        if (c == '?') {
            output += '.';
            continue;
        }
        if (std::string_view(".^$|()[]{}+\\").find(c) != std::string_view::npos) {
            output += '\\';
        }
        output += c;
    }
    output += "$";
    return std::regex(output, std::regex::ECMAScript | std::regex::icase);
}

bool matchesAnyPattern(const std::string &path, const std::vector<std::string> &patterns)
{
    return std::any_of(patterns.begin(), patterns.end(), [&path](const auto &pattern) {
        return std::regex_match(path, globToRegex(pattern));
    });
}

std::uintmax_t computePathSize(const fs::path &path)
{
    if (!fs::exists(path)) {
        return 0;
    }
    if (fs::is_regular_file(path)) {
        return fs::file_size(path);
    }

    std::uintmax_t total = 0;
    for (const auto &entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_regular_file()) {
            total += entry.file_size();
        }
    }
    return total;
}

std::vector<ArchiveEntry> collectArchiveEntries(const fs::path &root)
{
    std::vector<ArchiveEntry> entries;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto relative = fs::relative(entry.path(), root).generic_string();
        entries.push_back({entry.path(), relative});
    }
    return entries;
}

std::size_t pruneBackupDirectory(const BackupConfig &config, const fs::path &backup_root, const fs::path &keep_path)
{
    struct BackupEntry {
        fs::path path;
        std::uintmax_t size_bytes = 0;
        fs::file_time_type modified_at{};
    };

    std::vector<BackupEntry> backups;
    if (fs::exists(backup_root)) {
        for (const auto &entry : fs::directory_iterator(backup_root)) {
            if (!entry.is_regular_file() && !entry.is_directory()) {
                continue;
            }
            backups.push_back({entry.path(), computePathSize(entry.path()), entry.last_write_time()});
        }
    }

    std::sort(backups.begin(), backups.end(), [](const auto &left, const auto &right) {
        return left.modified_at > right.modified_at;
    });

    std::uintmax_t total_size = 0;
    for (const auto &entry : backups) {
        total_size += entry.size_bytes;
    }

    const auto now = fs::file_time_type::clock::now();
    std::size_t removed = 0;
    for (std::size_t index = 0; index < backups.size(); ++index) {
        const auto &backup = backups[index];
        if (!keep_path.empty() && fs::exists(keep_path) && fs::equivalent(backup.path, keep_path)) {
            continue;
        }

        bool should_remove = false;
        if (config.retention.max_age_days > 0) {
            const auto age = now - backup.modified_at;
            const auto max_age = std::chrono::hours(24 * config.retention.max_age_days);
            if (age > max_age) {
                should_remove = true;
            }
        }
        if (!should_remove && config.retention.max_backups > 0 &&
            index >= static_cast<std::size_t>(config.retention.max_backups)) {
            should_remove = true;
        }
        if (!should_remove && config.retention.max_total_size_mb > 0) {
            const auto limit = static_cast<std::uintmax_t>(config.retention.max_total_size_mb) * 1024U * 1024U;
            if (total_size > limit) {
                total_size -= backup.size_bytes;
                should_remove = true;
            }
        }

        if (!should_remove) {
            continue;
        }

        fs::remove_all(backup.path);
        ++removed;
    }

    return removed;
}

void copyFilesParallel(const std::vector<std::pair<fs::path, fs::path>> &copies, const int thread_count)
{
    std::atomic_size_t next_index{0};
    std::exception_ptr exception;
    std::mutex exception_mutex;

    const auto worker = [&]() {
        while (true) {
            const auto index = next_index.fetch_add(1);
            if (index >= copies.size()) {
                return;
            }

            if (exception) {
                return;
            }

            try {
                const auto &[source, destination] = copies[index];
                fs::create_directories(destination.parent_path());
                fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
            }
            catch (...) {
                std::lock_guard lock(exception_mutex);
                if (!exception) {
                    exception = std::current_exception();
                }
                return;
            }
        }
    };

    const auto worker_count = std::max(1, std::min(thread_count, static_cast<int>(copies.size())));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker);
    }

    for (auto &worker_thread : workers) {
        worker_thread.join();
    }

    if (exception) {
        std::rethrow_exception(exception);
    }
}

void copyTree(const fs::path &source_root, const fs::path &destination_root)
{
    if (!fs::exists(source_root)) {
        return;
    }

    for (const auto &entry : fs::recursive_directory_iterator(source_root)) {
        const auto relative = fs::relative(entry.path(), source_root);
        const auto destination = destination_root / relative;
        if (entry.is_directory()) {
            fs::create_directories(destination);
            continue;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        fs::create_directories(destination.parent_path());
        fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing);
    }
}

bool pathEscapesRoot(const fs::path &relative_path)
{
    if (relative_path.empty() || relative_path.is_absolute()) {
        return true;
    }

    for (const auto &component : relative_path) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

}  // namespace

BackupManager::BackupManager(BackupperPlugin &plugin) : plugin_(plugin) {}

void BackupManager::onEnable()
{
    loadConfigOrThrow();
    runtime_schedule_enabled_ = config_.schedule.enabled;
    configureSchedule();
    queueStartupBackupIfNeeded();
    if (config_.retention.prune_on_startup) {
        std::string error;
        prune(error, nullptr);
        if (!error.empty()) {
            plugin_.getLogger().warning("Startup prune failed: {}", error);
        }
    }
}

void BackupManager::onDisable()
{
    if (save_query_task_) {
        save_query_task_->cancel();
        save_query_task_.reset();
    }
    if (phase_monitor_task_) {
        phase_monitor_task_->cancel();
        phase_monitor_task_.reset();
    }
    clearSchedule();

    resumeSaveIfNeeded();
    resumeRestoreSaveIfNeeded();

    if (current_) {
        if (current_->snapshot_future.valid()) {
            current_->snapshot_future.wait();
        }
        if (current_->finalize_future.valid()) {
            current_->finalize_future.wait();
        }
    }
    if (restore_current_ && restore_future_.valid()) {
        restore_future_.wait();
    }
}

bool BackupManager::reload(std::string &error)
{
    if (isBusy()) {
        error = "A backup or restore is already running.";
        return false;
    }

    try {
        loadConfigOrThrow();
        runtime_schedule_enabled_ = config_.schedule.enabled;
        configureSchedule();
        error.clear();
        return true;
    }
    catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool BackupManager::startManualBackup(const std::string &requested_by, const std::string &label, std::string &error)
{
    return startBackupRequest("manual", requested_by, label, error);
}

bool BackupManager::startRestore(const std::string &requested_by, const std::string &backup_selector, std::string &error)
{
    const auto backup = findBackup(backup_selector);
    if (!backup) {
        error = "No backup found with that name.";
        return false;
    }
    return startRestoreRequest(requested_by, *backup, error);
}

bool BackupManager::startSchedule(std::string &error)
{
    runtime_schedule_enabled_ = true;
    next_scheduled_run_.reset();
    try {
        configureSchedule();
        error.clear();
        return true;
    }
    catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool BackupManager::stopSchedule(std::string &error)
{
    clearSchedule();
    runtime_schedule_enabled_ = false;
    next_scheduled_run_.reset();
    clearRuntimeState();
    error.clear();
    return true;
}

bool BackupManager::startBackupRequest(const std::string &trigger, const std::string &requested_by,
                                       const std::string &label, std::string &error)
{
    if (isBusy()) {
        error = "A backup is already running.";
        return false;
    }

    auto context = std::make_shared<BackupContext>();
    context->config = config_;
    context->trigger = trigger;
    context->requested_by = requested_by;
    context->label = label;
    context->server_root = getServerRoot();
    context->backup_root = context->server_root / config_.backup_directory;
    context->config_path = getConfigPath();
    context->started_at = std::chrono::system_clock::now();
    context->values = buildTemplateValues(trigger, requested_by, label);
    context->resolved_targets = resolveTargets(config_, context->values);
    context->values.backup_name = sanitizeName(renderTemplate(config_.name_template, context->values));
    context->staging_root = plugin_.getDataFolder() / config_.temporary_directory / context->values.backup_name;
    context->output_path = context->backup_root / context->values.backup_name;
    if (config_.archive_format == "zip") {
        context->output_path += ".zip";
    }
    context->values.backup_name = context->output_path.filename().string();

    try {
        beginBackup(context);
        error.clear();
        return true;
    }
    catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool BackupManager::startRestoreRequest(const std::string &requested_by, const StoredBackup &backup, std::string &error)
{
    if (isBusy()) {
        error = "Another backup or restore operation is already running.";
        return false;
    }
    if (config_.restore.require_no_players && !plugin_.getServer().getOnlinePlayers().empty()) {
        error = "All players must leave before running a restore.";
        return false;
    }

    auto context = std::make_shared<RestoreContext>();
    context->config = config_;
    context->backup = backup;
    context->requested_by = requested_by;
    context->server_root = getServerRoot();
    context->temp_root = plugin_.getDataFolder() / config_.temporary_directory / ("restore_" + sanitizeName(backup.name));
    context->values = buildTemplateValues("restore", requested_by, backup.name);
    context->values.backup_name = backup.name;
    context->values.label = backup.name;

    restore_current_ = std::move(context);
    auto &console = plugin_.getServer().getCommandSender();
    if (!plugin_.getServer().dispatchCommand(console, "save hold")) {
        restore_current_.reset();
        error = "Unable to execute 'save hold' for restore.";
        return false;
    }

    restore_current_->save_hold_active = true;
    save_query_task_ = plugin_.getServer().getScheduler().runTaskTimer(
        plugin_, [this]() { onSaveQueryTick(); }, 1, config_.save_query_interval_ticks);
    error.clear();
    return true;
}

void BackupManager::sendStatus(endstone::CommandSender &sender) const
{
    sender.sendMessage("Backupper status: {}", isBusy() ? "running" : "idle");
    sender.sendMessage("Config: {}", getConfigPath().string());
    sender.sendMessage("Backup directory: {}", config_.backup_directory);
    sender.sendMessage("Format: {}", config_.archive_format);
    sender.sendMessage("Schedule: {} ({})", runtime_schedule_enabled_ ? "running" : "stopped", config_.schedule.mode);
    if (config_.schedule.mode == "interval") {
        sender.sendMessage("Interval resets: manual={}, restart={}, restore={}",
                           config_.schedule.reset_interval_on_manual_backup ? "on" : "off",
                           config_.schedule.reset_interval_on_server_start ? "on" : "off",
                           config_.schedule.reset_interval_on_restore ? "on" : "off");
        sender.sendMessage("Interval startup state: persist={}, catch_up={}",
                           config_.schedule.persist_interval_state ? "on" : "off",
                           config_.schedule.catch_up_missed_run_on_startup ? "on" : "off");
    }
    if (next_scheduled_run_) {
        sender.sendMessage("Next scheduled run: {}", timePointToFileTime(*next_scheduled_run_));
    }

    if (current_) {
        sender.sendMessage("Current backup: {}", current_->values.backup_name);
        sender.sendMessage("Trigger: {}", current_->trigger);
        sender.sendMessage("Requested by: {}", current_->requested_by);
    }
    if (restore_current_) {
        sender.sendMessage("Current restore: {}", restore_current_->backup.name);
        sender.sendMessage("Restore requested by: {}", restore_current_->requested_by);
    }

    if (last_backup_) {
        sender.sendMessage("Last backup: {} ({}, {}, {})", last_backup_->name, last_backup_->format,
                           formatBytes(last_backup_->size_bytes), formatDuration(last_backup_->duration));
    }

    if (!last_error_.empty()) {
        sender.sendErrorMessage("Last error: {}", last_error_);
    }
}

std::vector<StoredBackup> BackupManager::listBackups(const std::size_t limit) const
{
    auto backups = collectBackups(config_);
    if (limit < backups.size()) {
        backups.resize(limit);
    }
    return backups;
}

bool BackupManager::prune(std::string &error, std::size_t *removed_count)
{
    try {
        std::vector<std::string> removed_names;
        const auto count = pruneBackupsInternal(config_, current_ ? current_->output_path : fs::path{}, &removed_names);
        if (removed_count != nullptr) {
            *removed_count = count;
        }
        error.clear();
        return true;
    }
    catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool BackupManager::deleteBackup(const std::string &name, std::string &error)
{
    try {
        const auto backups = collectBackups(config_);
        const auto match = std::find_if(backups.begin(), backups.end(), [&name](const auto &backup) {
            return backup.name == name || backup.path.filename() == name;
        });
        if (match == backups.end()) {
            error = "No backup found with that name.";
            return false;
        }
        if (current_ && fs::exists(current_->output_path) && fs::equivalent(match->path, current_->output_path)) {
            error = "That backup is currently being written.";
            return false;
        }
        fs::remove_all(match->path);
        error.clear();
        return true;
    }
    catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

void BackupManager::loadConfigOrThrow()
{
    config_ = loadConfig(getConfigPath());
}

void BackupManager::configureSchedule()
{
    clearSchedule();

    if (!runtime_schedule_enabled_) {
        runtime_schedule_enabled_ = config_.schedule.enabled;
    }

    if (!runtime_schedule_enabled_) {
        next_scheduled_run_.reset();
        clearRuntimeState();
        return;
    }

    if (config_.schedule.mode == "interval") {
        const auto period = std::max(1, config_.schedule.interval_minutes);
        std::chrono::system_clock::time_point next_run;
        const auto now = std::chrono::system_clock::now();
        if (!config_.schedule.reset_interval_on_server_start) {
            if (const auto persisted = loadPersistedIntervalRun()) {
                next_run = *persisted;
                while (next_run <= now) {
                    next_run += std::chrono::minutes(period);
                }
                if (config_.schedule.catch_up_missed_run_on_startup && *persisted <= now) {
                    maybeRunMissedIntervalBackupOnStartup(*persisted);
                    next_run = std::chrono::system_clock::now() + std::chrono::minutes(period);
                }
            }
            else {
                next_run = now + std::chrono::minutes(period);
            }
        }
        else {
            next_run = now + std::chrono::minutes(period);
        }

        next_scheduled_run_ = next_run;
        persistRuntimeState();
        armIntervalSchedule();
        return;
    }

    clearRuntimeState();

    if (config_.schedule.mode == "clock") {
        next_scheduled_run_ = computeNextClockRun(std::chrono::system_clock::now());
        schedule_task_ =
            plugin_.getServer().getScheduler().runTaskTimer(plugin_, [this]() { onScheduledCronTick(); }, 20, 20);
        return;
    }

    if (config_.schedule.mode == "cron") {
        const auto expression = cron::make_cron(config_.schedule.cron);
        next_scheduled_run_ = cron::cron_next(expression, std::chrono::system_clock::now());
        schedule_task_ =
            plugin_.getServer().getScheduler().runTaskTimer(plugin_, [this]() { onScheduledCronTick(); }, 20, 20);
        return;
    }

    throw std::runtime_error("schedule.mode must be one of 'interval', 'clock', or 'cron'.");
}

void BackupManager::clearSchedule()
{
    if (schedule_task_) {
        schedule_task_->cancel();
        schedule_task_.reset();
    }
}

void BackupManager::queueStartupBackupIfNeeded()
{
    if (!runtime_schedule_enabled_ || !config_.schedule.run_on_startup) {
        return;
    }

    plugin_.getServer().getScheduler().runTaskLater(plugin_, [this]() {
        std::string error;
        if (!startScheduledBackup("startup", error) && !error.empty()) {
            plugin_.getLogger().warning("Startup backup skipped: {}", error);
        }
    }, 40);
}

void BackupManager::restartIntervalScheduleFrom(const std::chrono::system_clock::time_point &time_point)
{
    if (!runtime_schedule_enabled_ || config_.schedule.mode != "interval") {
        return;
    }

    clearSchedule();

    const auto period = std::max(1, config_.schedule.interval_minutes);
    next_scheduled_run_ = time_point + std::chrono::minutes(period);
    persistRuntimeState();
    armIntervalSchedule();
}

void BackupManager::armIntervalSchedule()
{
    if (!runtime_schedule_enabled_ || config_.schedule.mode != "interval" || !next_scheduled_run_) {
        return;
    }

    auto delay = std::chrono::duration_cast<std::chrono::seconds>(*next_scheduled_run_ - std::chrono::system_clock::now())
                     .count();
    if (delay < 1) {
        delay = 1;
    }

    schedule_task_ = plugin_.getServer().getScheduler().runTaskLater(
        plugin_, [this]() { onScheduledIntervalTick(); }, static_cast<std::uint64_t>(delay) * 20ULL);
}

void BackupManager::persistRuntimeState() const
{
    if (config_.schedule.mode != "interval" || !config_.schedule.persist_interval_state || !next_scheduled_run_) {
        return;
    }

    fs::create_directories(getStatePath().parent_path());
    json state = {{"next_interval_run_unix", timePointToUnixSeconds(*next_scheduled_run_)}};
    std::ofstream output(getStatePath());
    output << state.dump(4) << '\n';
}

void BackupManager::clearRuntimeState()
{
    std::error_code error;
    fs::remove(getStatePath(), error);
}

void BackupManager::maybeRunMissedIntervalBackupOnStartup(const std::chrono::system_clock::time_point &persisted_next_run)
{
    plugin_.getLogger().info("Queueing a missed interval backup for the persisted run at '{}'.",
                             timePointToFileTime(persisted_next_run));
    plugin_.getServer().getScheduler().runTaskLater(plugin_, [this]() {
        std::string error;
        if (!startScheduledBackup("schedule", error) && !error.empty()) {
            plugin_.getLogger().warning("Missed interval backup skipped: {}", error);
        }
    }, 40);
}

std::optional<std::chrono::system_clock::time_point> BackupManager::loadPersistedIntervalRun() const
{
    if (!config_.schedule.persist_interval_state || !fs::exists(getStatePath())) {
        return std::nullopt;
    }

    std::ifstream input(getStatePath());
    if (!input) {
        return std::nullopt;
    }

    json state = json::parse(input, nullptr, false);
    if (state.is_discarded() || !state.contains("next_interval_run_unix")) {
        return std::nullopt;
    }

    return unixSecondsToTimePoint(state.at("next_interval_run_unix").get<std::int64_t>());
}

std::chrono::system_clock::time_point BackupManager::computeNextClockRun(
    const std::chrono::system_clock::time_point &after) const
{
    const auto times = parseClockTimesLocal();
    const auto time = std::chrono::system_clock::to_time_t(after);
    std::tm broken_down{};
    localtime_r(&time, &broken_down);

    for (int day_offset = 0; day_offset < 8; ++day_offset) {
        std::tm candidate = broken_down;
        candidate.tm_hour = 0;
        candidate.tm_min = 0;
        candidate.tm_sec = 0;
        candidate.tm_mday += day_offset;
        const auto midnight = std::chrono::system_clock::from_time_t(std::mktime(&candidate));
        for (const int seconds_since_midnight : times) {
            const auto next_run = midnight + std::chrono::seconds(seconds_since_midnight);
            if (next_run > after) {
                return next_run;
            }
        }
    }

    throw std::runtime_error("Unable to compute next clock-based scheduled backup.");
}

std::vector<int> BackupManager::parseClockTimesLocal() const
{
    std::vector<int> result;
    result.reserve(config_.schedule.clock_times_local.size());
    for (const auto &value : config_.schedule.clock_times_local) {
        result.push_back(parseClockTimeString(value));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void BackupManager::beginBackup(std::shared_ptr<BackupContext> context)
{
    fs::create_directories(context->backup_root);
    fs::remove_all(context->staging_root);
    fs::create_directories(context->staging_root.parent_path());

    current_ = std::move(context);
    last_error_.clear();

    auto &console = plugin_.getServer().getCommandSender();
    if (!plugin_.getServer().dispatchCommand(console, "save hold")) {
        current_.reset();
        throw std::runtime_error("Unable to execute 'save hold'.");
    }

    current_->save_hold_active = true;
    if (config_.notifications.broadcast) {
        broadcastMessage(renderTemplate(config_.notifications.copy_message, current_->values));
    }

    save_query_task_ = plugin_.getServer().getScheduler().runTaskTimer(
        plugin_, [this]() { onSaveQueryTick(); }, 1, current_->config.save_query_interval_ticks);
}

void BackupManager::onScheduledIntervalTick()
{
    if (!runtime_schedule_enabled_) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    if (next_scheduled_run_ && now + std::chrono::seconds(5) < *next_scheduled_run_) {
        plugin_.getLogger().warning("Ignoring an early interval schedule tick; next run is '{}'.",
                                    timePointToFileTime(*next_scheduled_run_));
        return;
    }

    std::string error;
    if (!startScheduledBackup("schedule", error) && !error.empty()) {
        plugin_.getLogger().warning("Scheduled backup skipped: {}", error);
    }

    next_scheduled_run_ = now + std::chrono::minutes(std::max(1, config_.schedule.interval_minutes));
    persistRuntimeState();
    armIntervalSchedule();
}

void BackupManager::onScheduledCronTick()
{
    if (!runtime_schedule_enabled_ || !next_scheduled_run_) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    if (now < *next_scheduled_run_) {
        return;
    }

    if (config_.schedule.mode == "clock") {
        next_scheduled_run_ = computeNextClockRun(now + std::chrono::seconds(1));
    }
    else {
        const auto expression = cron::make_cron(config_.schedule.cron);
        next_scheduled_run_ = cron::cron_next(expression, now + std::chrono::seconds(1));
    }

    std::string error;
    if (!startScheduledBackup("schedule", error) && !error.empty()) {
        plugin_.getLogger().warning("Scheduled backup skipped: {}", error);
    }
}

void BackupManager::onSaveQueryTick()
{
    if (!current_ && !restore_current_) {
        return;
    }

    std::vector<std::string> messages;
    endstone::CommandSenderWrapper wrapper(
        plugin_.getServer().getCommandSender(),
        [&messages, this](const endstone::Message &message) { messages.push_back(messageToString(message)); },
        [&messages, this](const endstone::Message &message) { messages.push_back(messageToString(message)); });

    const bool dispatch_result = plugin_.getServer().dispatchCommand(wrapper, "save query");

    if (current_) {
        ++current_->save_query_attempts;
    }
    if (restore_current_) {
        ++restore_current_->save_query_attempts;
    }

    if (saveQueryLooksReady(messages, dispatch_result)) {
        if (save_query_task_) {
            save_query_task_->cancel();
            save_query_task_.reset();
        }
        if (current_) {
            current_->snapshot_future =
                std::async(std::launch::async, [context = current_]() { return createSnapshot(*context); });
        }
        else if (restore_current_) {
            restore_future_ = std::async(std::launch::async, [context = restore_current_]() { return performRestore(*context); });
        }
        ensurePhaseMonitorTask(5);
        return;
    }

    const auto waited_ticks =
        current_ ? current_->save_query_attempts * current_->config.save_query_interval_ticks
                 : restore_current_->save_query_attempts * restore_current_->config.save_query_interval_ticks;
    const auto timeout_ticks =
        current_ ? current_->config.save_query_timeout_ticks : restore_current_->config.save_query_timeout_ticks;
    if (waited_ticks >= timeout_ticks) {
        if (current_) {
            finishFailure("Timed out waiting for 'save query' to report a ready snapshot.");
        }
        else {
            finishRestoreFailure("Timed out waiting for 'save query' to report a ready restore point.");
        }
    }
}

void BackupManager::onPhaseMonitorTick()
{
    if (!current_ && !restore_current_) {
        if (phase_monitor_task_) {
            phase_monitor_task_->cancel();
            phase_monitor_task_.reset();
        }
        return;
    }

    if (current_ && current_->snapshot_future.valid() &&
        current_->snapshot_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto snapshot = current_->snapshot_future.get();
        resumeSaveIfNeeded();

        if (!snapshot.ok) {
            finishFailure(snapshot.error);
            return;
        }

        current_->finalize_future = std::async(std::launch::async, [context = current_, snapshot]() {
            return BackupManager::finalizeBackup(*context, snapshot);
        });
    }

    if (current_ && current_->finalize_future.valid() &&
        current_->finalize_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = current_->finalize_future.get();
        if (!result.ok) {
            finishFailure(result.error);
            return;
        }
        finishSuccess(result);
    }

    if (restore_current_ && restore_future_.valid() &&
        restore_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = restore_future_.get();
        if (!result.ok) {
            finishRestoreFailure(result.error);
            return;
        }
        finishRestoreSuccess(result);
    }
}

void BackupManager::ensurePhaseMonitorTask(const std::uint64_t period_ticks)
{
    if (phase_monitor_task_ && !phase_monitor_task_->isCancelled()) {
        return;
    }
    phase_monitor_task_ =
        plugin_.getServer().getScheduler().runTaskTimer(plugin_, [this]() { onPhaseMonitorTick(); }, 1, period_ticks);
}

void BackupManager::finishFailure(const std::string &error)
{
    if (save_query_task_) {
        save_query_task_->cancel();
        save_query_task_.reset();
    }
    if (phase_monitor_task_) {
        phase_monitor_task_->cancel();
        phase_monitor_task_.reset();
    }

    if (current_) {
        resumeSaveIfNeeded();
        if (!current_->config.keep_staging_on_failure) {
            fs::remove_all(current_->staging_root);
        }
        broadcastFailure(current_->values, error);
    }

    last_error_ = error;
    plugin_.getLogger().error("Backup failed: {}", error);
    current_.reset();
}

void BackupManager::finishRestoreFailure(const std::string &error)
{
    if (save_query_task_) {
        save_query_task_->cancel();
        save_query_task_.reset();
    }
    if (phase_monitor_task_) {
        phase_monitor_task_->cancel();
        phase_monitor_task_.reset();
    }

    if (restore_current_) {
        resumeRestoreSaveIfNeeded();
        fs::remove_all(restore_current_->temp_root);
        broadcastRestoreFailure(restore_current_->values, error);
    }

    last_error_ = error;
    plugin_.getLogger().error("Restore failed: {}", error);
    restore_current_.reset();
}

void BackupManager::finishSuccess(const FinalizeResult &result)
{
    if (save_query_task_) {
        save_query_task_->cancel();
        save_query_task_.reset();
    }
    if (phase_monitor_task_) {
        phase_monitor_task_->cancel();
        phase_monitor_task_.reset();
    }

    last_backup_ = result.summary;
    last_error_.clear();
    updateScheduleAfterSuccessfulBackup(result.summary);
    broadcastSuccess(result.summary);
    plugin_.getLogger().info("Backup '{}' finished in {}.", result.summary.name, formatDuration(result.summary.duration));
    current_.reset();
}

void BackupManager::finishRestoreSuccess(const RestoreResult &result)
{
    if (save_query_task_) {
        save_query_task_->cancel();
        save_query_task_.reset();
    }
    if (phase_monitor_task_) {
        phase_monitor_task_->cancel();
        phase_monitor_task_.reset();
    }

    if (!restore_current_) {
        return;
    }

    last_error_.clear();
    updateScheduleAfterSuccessfulRestore();
    broadcastRestoreSuccess(restore_current_->values);
    plugin_.getLogger().info("Restore '{}' finished, restored {} file(s).", result.backup_name, result.restored_files);
    fs::remove_all(restore_current_->temp_root);

    const auto shutdown_delay_ticks = static_cast<std::uint64_t>(restore_current_->config.restore.shutdown_delay_seconds) * 20ULL;
    const bool should_shutdown = restore_current_->config.restore.shutdown_after_restore;

    if (!should_shutdown) {
        resumeRestoreSaveIfNeeded();
    }

    restore_current_.reset();

    if (should_shutdown) {
        plugin_.getServer().getScheduler().runTaskLater(plugin_, [this]() { plugin_.getServer().shutdown(); },
                                                        shutdown_delay_ticks);
    }
}

void BackupManager::resumeSaveIfNeeded()
{
    if (!current_ || !current_->save_hold_active) {
        return;
    }
    auto &console = plugin_.getServer().getCommandSender();
    if (!plugin_.getServer().dispatchCommand(console, "save resume")) {
        plugin_.getLogger().warning("Unable to execute 'save resume'.");
    }
    current_->save_hold_active = false;
}

void BackupManager::resumeRestoreSaveIfNeeded()
{
    if (!restore_current_ || !restore_current_->save_hold_active) {
        return;
    }
    auto &console = plugin_.getServer().getCommandSender();
    if (!plugin_.getServer().dispatchCommand(console, "save resume")) {
        plugin_.getLogger().warning("Unable to execute 'save resume' after restore.");
    }
    restore_current_->save_hold_active = false;
}

void BackupManager::broadcastMessage(const std::string &message) const
{
    plugin_.getServer().broadcastMessage(message);
}

void BackupManager::broadcastFailure(const TemplateValues &values, const std::string &error) const
{
    if (!config_.notifications.broadcast) {
        return;
    }
    auto copy = values;
    copy.error = error;
    broadcastMessage(renderTemplate(config_.notifications.failure_message, copy));
}

void BackupManager::broadcastSuccess(const BackupSummary &summary) const
{
    if (!config_.notifications.broadcast) {
        return;
    }
    TemplateValues values = buildTemplateValues(summary.trigger, summary.requested_by, summary.label);
    values.backup_name = summary.name;
    values.duration = formatDuration(summary.duration);
    broadcastMessage(renderTemplate(config_.notifications.success_message, values));
}

void BackupManager::broadcastRestoreFailure(const TemplateValues &values, const std::string &error) const
{
    if (!config_.notifications.broadcast) {
        return;
    }
    auto copy = values;
    copy.error = error;
    broadcastMessage(renderTemplate(config_.restore.failure_message, copy));
}

void BackupManager::broadcastRestoreSuccess(const TemplateValues &values) const
{
    if (!config_.notifications.broadcast) {
        return;
    }
    auto copy = values;
    copy.shutdown_delay_seconds = std::to_string(config_.restore.shutdown_delay_seconds);
    broadcastMessage(renderTemplate(config_.restore.success_message, copy));
}

bool BackupManager::startScheduledBackup(const std::string &trigger, std::string &error)
{
    std::string skip_reason;
    if (shouldSkipScheduledBackup(skip_reason)) {
        error = skip_reason;
        return false;
    }

    return startBackupRequest(trigger, "scheduler", "", error);
}

bool BackupManager::shouldSkipScheduledBackup(std::string &reason) const
{
    if (config_.schedule.skip_when_no_players && plugin_.getServer().getOnlinePlayers().empty()) {
        reason = "No players are online.";
        return true;
    }

    reason.clear();
    return false;
}

void BackupManager::updateScheduleAfterSuccessfulBackup(const BackupSummary &summary)
{
    if (!runtime_schedule_enabled_ || config_.schedule.mode != "interval") {
        return;
    }
    if (summary.trigger == "manual" && config_.schedule.reset_interval_on_manual_backup) {
        restartIntervalScheduleFrom(std::chrono::system_clock::now());
    }
}

void BackupManager::updateScheduleAfterSuccessfulRestore()
{
    if (!runtime_schedule_enabled_ || config_.schedule.mode != "interval") {
        return;
    }
    if (config_.schedule.reset_interval_on_restore) {
        restartIntervalScheduleFrom(std::chrono::system_clock::now());
    }
}

bool BackupManager::isBusy() const
{
    return static_cast<bool>(current_) || static_cast<bool>(restore_current_);
}

fs::path BackupManager::getServerRoot() const
{
    return plugin_.getDataFolder().parent_path().parent_path();
}

fs::path BackupManager::getConfigPath() const
{
    return plugin_.getDataFolder() / "config.json";
}

fs::path BackupManager::getStatePath() const
{
    return plugin_.getDataFolder() / "runtime_state.json";
}

std::string BackupManager::readLevelName() const
{
    std::ifstream input(getServerRoot() / "server.properties");
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("level-name=")) {
            continue;
        }
        return trim(line.substr(std::string("level-name=").size()));
    }
    return "Bedrock level";
}

BackupManager::TemplateValues BackupManager::buildTemplateValues(const std::string &trigger, const std::string &requested_by,
                                                                 const std::string &label) const
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm broken_down{};
    localtime_r(&time, &broken_down);

    std::ostringstream date_stream;
    date_stream << std::put_time(&broken_down, "%Y-%m-%d");

    std::ostringstream time_stream;
    time_stream << std::put_time(&broken_down, "%H-%M-%S");

    TemplateValues values;
    values.countdown_seconds = std::to_string(config_.notifications.countdown_seconds);
    values.date = date_stream.str();
    values.duration = "0s";
    values.error.clear();
    values.label = label;
    values.level_name = readLevelName();
    values.requested_by = requested_by;
    values.shutdown_delay_seconds = std::to_string(config_.restore.shutdown_delay_seconds);
    values.time = time_stream.str();
    values.timestamp = timePointToCompactTimestamp(now);
    values.trigger = trigger;
    return values;
}

std::string BackupManager::renderTemplate(const std::string &input, const TemplateValues &values) const
{
    std::string output = input;
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"${backup_name}", values.backup_name},
        {"${countdown_seconds}", values.countdown_seconds},
        {"${date}", values.date},
        {"${duration}", values.duration},
        {"${error}", values.error},
        {"${label}", values.label},
        {"${level_name}", values.level_name},
        {"${requested_by}", values.requested_by},
        {"${shutdown_delay_seconds}", values.shutdown_delay_seconds},
        {"${time}", values.time},
        {"${timestamp}", values.timestamp},
        {"${trigger}", values.trigger},
    };

    for (const auto &[needle, replacement] : replacements) {
        std::size_t position = 0;
        while ((position = output.find(needle, position)) != std::string::npos) {
            output.replace(position, needle.size(), replacement);
            position += replacement.size();
        }
    }
    return output;
}

std::string BackupManager::sanitizeName(std::string value) const
{
    for (char &character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_' || character == '.') {
            continue;
        }
        character = '_';
    }
    value = trim(value);
    while (value.find("__") != std::string::npos) {
        value.replace(value.find("__"), 2, "_");
    }
    if (value.empty()) {
        value = "backup";
    }
    return value;
}

std::string BackupManager::formatDuration(const std::chrono::milliseconds duration) const
{
    const auto total_seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    const auto minutes = total_seconds / 60;
    const auto seconds = total_seconds % 60;
    if (minutes == 0) {
        return fmt::format("{}s", seconds);
    }
    return fmt::format("{}m {}s", minutes, seconds);
}

std::string BackupManager::formatBytes(const std::uintmax_t size_bytes) const
{
    static const std::array<const char *, 5> units = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(size_bytes);
    std::size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    return fmt::format("{:.2f} {}", size, units[unit]);
}

std::string BackupManager::messageToString(const endstone::Message &message) const
{
    if (const auto *text = std::get_if<std::string>(&message)) {
        return *text;
    }

    const auto *translatable = std::get_if<endstone::Translatable>(&message);
    if (translatable == nullptr) {
        return {};
    }

    std::ostringstream output;
    output << translatable->getText();
    for (const auto &parameter : translatable->getParameters()) {
        output << ' ' << parameter;
    }
    return output.str();
}

bool BackupManager::saveQueryLooksReady(const std::vector<std::string> &messages, const bool dispatch_result) const
{
    if (dispatch_result) {
        return true;
    }

    return std::any_of(messages.begin(), messages.end(), [](const auto &message) {
        const auto lowered = toLower(message);
        return lowered.find("ready") != std::string::npos || lowered.find("copied") != std::string::npos;
    });
}

std::vector<BackupTargetConfig> BackupManager::resolveTargets(const BackupConfig &config, const TemplateValues &values) const
{
    std::vector<BackupTargetConfig> resolved;
    resolved.reserve(config.targets.size());
    for (const auto &target : config.targets) {
        resolved.push_back({renderTemplate(target.path, values), target.required});
    }
    return resolved;
}

std::vector<StoredBackup> BackupManager::collectBackups(const BackupConfig &config) const
{
    const auto backup_root = getServerRoot() / config.backup_directory;
    std::vector<StoredBackup> backups;
    if (!fs::exists(backup_root)) {
        return backups;
    }

    for (const auto &entry : fs::directory_iterator(backup_root)) {
        if (!entry.is_regular_file() && !entry.is_directory()) {
            continue;
        }
        backups.push_back({entry.path().filename().string(), entry.path(), computePathSize(entry.path()), entry.last_write_time()});
    }

    std::sort(backups.begin(), backups.end(), [](const auto &left, const auto &right) {
        return left.modified_at > right.modified_at;
    });
    return backups;
}

std::optional<StoredBackup> BackupManager::findBackup(const std::string &selector) const
{
    const auto backups = collectBackups(config_);
    if (backups.empty()) {
        return std::nullopt;
    }
    if (selector == "latest") {
        return backups.front();
    }

    const auto match = std::find_if(backups.begin(), backups.end(), [&selector](const auto &backup) {
        return backup.name == selector || backup.path.filename() == selector;
    });
    if (match == backups.end()) {
        return std::nullopt;
    }
    return *match;
}

std::size_t BackupManager::pruneBackupsInternal(const BackupConfig &config, const fs::path &keep_path,
                                                std::vector<std::string> *removed_names) const
{
    auto backups = collectBackups(config);
    std::size_t removed = 0;
    std::uintmax_t total_size = 0;
    for (const auto &backup : backups) {
        total_size += backup.size_bytes;
    }

    const auto now = fs::file_time_type::clock::now();

    auto should_remove = [&](std::size_t index, const StoredBackup &backup) {
        if (!keep_path.empty() && fs::exists(keep_path) && fs::equivalent(backup.path, keep_path)) {
            return false;
        }

        if (config.retention.max_age_days > 0) {
            const auto age = now - backup.modified_at;
            const auto max_age = std::chrono::hours(24 * config.retention.max_age_days);
            if (age > max_age) {
                return true;
            }
        }

        if (config.retention.max_backups > 0 && index >= static_cast<std::size_t>(config.retention.max_backups)) {
            return true;
        }

        if (config.retention.max_total_size_mb > 0) {
            const auto limit = static_cast<std::uintmax_t>(config.retention.max_total_size_mb) * 1024U * 1024U;
            if (total_size > limit) {
                total_size -= backup.size_bytes;
                return true;
            }
        }

        return false;
    };

    for (std::size_t index = 0; index < backups.size(); ++index) {
        const auto &backup = backups[index];
        if (!should_remove(index, backup)) {
            continue;
        }
        fs::remove_all(backup.path);
        ++removed;
        if (removed_names != nullptr) {
            removed_names->push_back(backup.name);
        }
    }

    return removed;
}

BackupManager::SnapshotResult BackupManager::createSnapshot(const BackupContext &context)
{
    SnapshotResult result;

    try {
        fs::remove_all(context.staging_root);
        const auto payload_root = context.staging_root / "payload";
        fs::create_directories(payload_root);

        auto effective_excludes = context.config.exclude_patterns;
        effective_excludes.push_back((fs::relative(context.backup_root, context.server_root)).generic_string() + "/**");
        effective_excludes.push_back((fs::relative(context.staging_root.parent_path(), context.server_root)).generic_string() + "/**");

        const auto targets = context.resolved_targets;
        std::vector<std::pair<fs::path, fs::path>> copy_jobs;
        std::uintmax_t source_bytes = 0;
        std::size_t file_count = 0;

        for (const auto &target : targets) {
            const auto source = context.server_root / target.path;
            if (!fs::exists(source)) {
                if (target.required) {
                    throw std::runtime_error("Required target does not exist: " + target.path);
                }
                continue;
            }

            if (fs::is_regular_file(source)) {
                const auto relative = fs::relative(source, context.server_root).generic_string();
                if (matchesAnyPattern(relative, effective_excludes)) {
                    continue;
                }
                const auto destination = payload_root / relative;
                copy_jobs.emplace_back(source, destination);
                source_bytes += fs::file_size(source);
                ++file_count;
                continue;
            }

            for (const auto &entry : fs::recursive_directory_iterator(source)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const auto relative = fs::relative(entry.path(), context.server_root).generic_string();
                if (matchesAnyPattern(relative, effective_excludes)) {
                    continue;
                }
                const auto destination = payload_root / relative;
                copy_jobs.emplace_back(entry.path(), destination);
                source_bytes += entry.file_size();
                ++file_count;
            }
        }

        copyFilesParallel(copy_jobs, context.config.copy_threads);

        result.ok = true;
        result.payload_root = payload_root;
        result.archive_entries = collectArchiveEntries(payload_root);
        result.file_count = file_count;
        result.source_bytes = source_bytes;
        return result;
    }
    catch (const std::exception &exception) {
        result.ok = false;
        result.error = exception.what();
        return result;
    }
}

BackupManager::FinalizeResult BackupManager::finalizeBackup(const BackupContext &context, const SnapshotResult &snapshot)
{
    FinalizeResult result;

    try {
        fs::create_directories(context.backup_root);

        const auto finished_at = std::chrono::system_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - context.started_at);

        json manifest = {
            {"name", context.output_path.filename().string()},
            {"created_at", timePointToFileTime(finished_at)},
            {"trigger", context.trigger},
            {"requested_by", context.requested_by},
            {"label", context.label},
            {"format", context.config.archive_format},
            {"file_count", snapshot.file_count},
            {"source_bytes", snapshot.source_bytes},
            {"config_path", context.config_path.string()},
            {"targets", json::array()},
        };

        for (const auto &target : context.resolved_targets) {
            manifest["targets"].push_back({{"path", target.path}, {"required", target.required}});
        }

        const auto manifest_text = context.config.write_manifest ? manifest.dump(4) : std::string{};

        if (context.config.archive_format == "zip") {
            writeZipArchive(context.output_path, snapshot.archive_entries, manifest_text, context.config.compression_level,
                            context.config.verify_archive_after_creation);
        }
        else {
            fs::remove_all(context.output_path);
            fs::create_directories(context.output_path.parent_path());
            fs::rename(snapshot.payload_root, context.output_path);
            if (!manifest_text.empty()) {
                std::ofstream output(context.output_path / "backupper-manifest.json");
                output << manifest_text << '\n';
            }
        }

        fs::remove_all(context.staging_root);

        std::size_t removed_count = 0;
        if (context.config.prune_after_backup) {
            removed_count = pruneBackupDirectory(context.config, context.backup_root, context.output_path);
        }

        result.summary.name = context.output_path.filename().string();
        result.summary.trigger = context.trigger;
        result.summary.requested_by = context.requested_by;
        result.summary.label = context.label;
        result.summary.format = context.config.archive_format;
        result.summary.path = context.output_path;
        result.summary.size_bytes = computePathSize(context.output_path);
        result.summary.source_bytes = snapshot.source_bytes;
        result.summary.file_count = snapshot.file_count;
        result.summary.duration = duration;
        result.summary.completed_at = finished_at;
        result.removed_count = removed_count;
        result.ok = true;
        return result;
    }
    catch (const std::exception &exception) {
        result.ok = false;
        result.error = exception.what();
        return result;
    }
}

BackupManager::RestoreResult BackupManager::performRestore(const RestoreContext &context)
{
    RestoreResult result;
    result.backup_name = context.backup.name;

    try {
        fs::remove_all(context.temp_root);
        const auto extraction_root = context.temp_root / "payload";
        fs::create_directories(extraction_root);

        if (fs::is_directory(context.backup.path)) {
            copyTree(context.backup.path, extraction_root);
        }
        else {
            extractZipArchive(context.backup.path, extraction_root);
        }

        const auto manifest_path = extraction_root / "backupper-manifest.json";
        if (!fs::exists(manifest_path)) {
            throw std::runtime_error("Backup manifest is missing. This backup cannot be restored safely.");
        }

        std::ifstream input(manifest_path);
        if (!input) {
            throw std::runtime_error("Unable to open backup manifest.");
        }

        json manifest = json::parse(input);
        if (!manifest.contains("targets") || !manifest.at("targets").is_array()) {
            throw std::runtime_error("Backup manifest is missing target metadata.");
        }

        for (const auto &target_value : manifest.at("targets")) {
            if (!target_value.is_object()) {
                throw std::runtime_error("Backup manifest contains an invalid target entry.");
            }

            const auto target_text = target_value.value("path", "");
            const bool required = target_value.value("required", true);
            if (target_text.empty()) {
                throw std::runtime_error("Backup manifest contains a target with an empty path.");
            }

            const fs::path relative_target = fs::path(target_text).lexically_normal();
            if (pathEscapesRoot(relative_target)) {
                throw std::runtime_error("Backup manifest contains an unsafe target path: " + target_text);
            }

            const auto source_path = extraction_root / relative_target;
            const auto destination_path = context.server_root / relative_target;

            if (fs::exists(destination_path)) {
                fs::remove_all(destination_path);
                ++result.removed_paths;
            }

            if (!fs::exists(source_path)) {
                if (required) {
                    throw std::runtime_error("Required restore target is missing from the backup: " + target_text);
                }
                continue;
            }

            if (fs::is_regular_file(source_path)) {
                fs::create_directories(destination_path.parent_path());
                fs::copy_file(source_path, destination_path, fs::copy_options::overwrite_existing);
                ++result.restored_files;
                continue;
            }

            fs::create_directories(destination_path);
            copyTree(source_path, destination_path);
            result.restored_files += collectArchiveEntries(source_path).size();
        }

        if (context.config.restore.prune_backups_after_restore) {
            pruneBackupDirectory(context.config, context.server_root / context.config.backup_directory, context.backup.path);
        }

        result.ok = true;
        return result;
    }
    catch (const std::exception &exception) {
        result.ok = false;
        result.error = exception.what();
        return result;
    }
}

}  // namespace backupper
