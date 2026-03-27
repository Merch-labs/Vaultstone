#pragma once

#include "archive_writer.h"
#include "vaultstone_config.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <endstone/command/command_sender.h>
#include <endstone/scheduler/task.h>

class VaultstonePlugin;

namespace backupper {

struct BackupSummary {
    std::string name;
    std::string trigger;
    std::string requested_by;
    std::string label;
    std::string format;
    std::filesystem::path path;
    std::uintmax_t size_bytes = 0;
    std::uintmax_t source_bytes = 0;
    std::size_t file_count = 0;
    std::chrono::milliseconds duration{0};
    std::chrono::system_clock::time_point completed_at{};
};

struct StoredBackup {
    std::string name;
    std::filesystem::path path;
    std::uintmax_t size_bytes = 0;
    std::filesystem::file_time_type modified_at{};
};

class BackupManager {
public:
    explicit BackupManager(VaultstonePlugin &plugin);

    void onEnable();
    void onDisable();

    bool reload(std::string &error);
    bool startManualBackup(const std::string &requested_by, const std::string &label, std::string &error);
    bool startRestore(const std::string &requested_by, const std::string &backup_selector, std::string &error);
    bool startSchedule(std::string &error);
    bool stopSchedule(std::string &error);
    void sendStatus(endstone::CommandSender &sender) const;

    [[nodiscard]] std::vector<StoredBackup> listBackups(std::size_t limit) const;
    bool prune(std::string &error, std::size_t *removed_count = nullptr);
    bool deleteBackup(const std::string &name, std::string &error);

private:
    struct TemplateValues {
        std::string backup_name;
        std::string countdown_seconds;
        std::string date;
        std::string duration;
        std::string error;
        std::string label;
        std::string level_name;
        std::string requested_by;
        std::string shutdown_delay_seconds;
        std::string time;
        std::string timestamp;
        std::string trigger;
    };

    struct SnapshotResult {
        bool ok = false;
        std::string error;
        std::filesystem::path payload_root;
        std::vector<ArchiveEntry> archive_entries;
        std::size_t file_count = 0;
        std::uintmax_t source_bytes = 0;
    };

    struct FinalizeResult {
        bool ok = false;
        std::string error;
        BackupSummary summary;
        std::size_t removed_count = 0;
    };

    struct RestoreContext {
        BackupConfig config;
        StoredBackup backup;
        std::string requested_by;
        std::filesystem::path server_root;
        std::filesystem::path temp_root;
        TemplateValues values;
        int save_query_attempts = 0;
        bool save_hold_active = false;
    };

    struct PendingRestoreRequest {
        std::string requested_by;
        StoredBackup backup;
    };

    struct RestoreResult {
        bool ok = false;
        std::string error;
        std::string backup_name;
        std::size_t restored_files = 0;
        std::size_t removed_paths = 0;
    };

    struct BackupContext {
        BackupConfig config;
        std::string trigger;
        std::string requested_by;
        std::string label;
        TemplateValues values;
        std::filesystem::path server_root;
        std::filesystem::path backup_root;
        std::filesystem::path staging_root;
        std::filesystem::path output_path;
        std::filesystem::path config_path;
        std::vector<BackupTargetConfig> resolved_targets;
        std::chrono::system_clock::time_point started_at;
        int save_query_attempts = 0;
        bool save_hold_active = false;
        std::future<SnapshotResult> snapshot_future;
        std::future<FinalizeResult> finalize_future;
    };

    void loadConfigOrThrow();
    bool startBackupRequest(const std::string &trigger, const std::string &requested_by, const std::string &label,
                            std::string &error);
    bool startRestoreRequest(const std::string &requested_by, const StoredBackup &backup, std::string &error);
    bool startRestoreRequestNow(const std::string &requested_by, const StoredBackup &backup, std::string &error);
    void beginBackup(std::shared_ptr<BackupContext> context);
    void onSaveQueryTick();
    void onPhaseMonitorTick();
    void onScheduledIntervalTick();
    void onScheduledCronTick();
    void ensurePhaseMonitorTask(std::uint64_t period_ticks);
    void configureSchedule();
    void clearSchedule();
    void queueStartupBackupIfNeeded();
    bool startScheduledBackup(const std::string &trigger, std::string &error);
    bool shouldSkipScheduledBackup(std::string &reason) const;
    void updateScheduleAfterSuccessfulBackup(const BackupSummary &summary);
    void updateScheduleAfterSuccessfulRestore();
    bool maybeStartQueuedRestore(std::string &error);
    void restartIntervalScheduleFrom(const std::chrono::system_clock::time_point &time_point);
    void armIntervalSchedule();
    void persistRuntimeState() const;
    void clearRuntimeState();
    void maybeRunMissedIntervalBackupOnStartup(const std::chrono::system_clock::time_point &persisted_next_run);
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> loadPersistedIntervalRun() const;
    [[nodiscard]] std::chrono::system_clock::time_point computeNextClockRun(
        const std::chrono::system_clock::time_point &after) const;
    [[nodiscard]] std::vector<int> parseClockTimesLocal() const;
    [[nodiscard]] std::optional<StoredBackup> findBackup(const std::string &selector) const;
    void finishFailure(const std::string &error);
    void finishRestoreFailure(const std::string &error);
    void finishSuccess(const FinalizeResult &result);
    void finishRestoreSuccess(const RestoreResult &result);
    void resumeSaveIfNeeded();
    void resumeRestoreSaveIfNeeded();
    void broadcastMessage(const std::string &message) const;
    void broadcastFailure(const TemplateValues &values, const std::string &error) const;
    void broadcastSuccess(const BackupSummary &summary) const;
    void broadcastRestoreFailure(const TemplateValues &values, const std::string &error) const;
    void broadcastRestoreSuccess(const TemplateValues &values) const;

    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool canRestoreSelector(const std::string &selector, std::string &error) const;
    [[nodiscard]] bool hasEnoughFreeSpace(const std::filesystem::path &path, std::string &error) const;
    [[nodiscard]] bool shouldAutoPruneAfterBackup(const std::string &trigger) const;
    bool enforceMaxBackupPolicyBeforeBackup(const BackupConfig &config, const std::filesystem::path &backup_root,
                                            const std::string &trigger, std::string &error) const;
    [[nodiscard]] std::filesystem::path getServerRoot() const;
    [[nodiscard]] std::filesystem::path getConfigPath() const;
    [[nodiscard]] std::filesystem::path getStatePath() const;
    [[nodiscard]] std::string readLevelName() const;
    void sendNotificationToRequester(const std::string &requested_by, const std::string &message, bool error) const;
    [[nodiscard]] TemplateValues buildTemplateValues(const std::string &trigger, const std::string &requested_by,
                                                     const std::string &label) const;
    [[nodiscard]] std::string renderTemplate(const std::string &input, const TemplateValues &values) const;
    [[nodiscard]] std::string sanitizeName(std::string value) const;
    [[nodiscard]] std::string formatDuration(std::chrono::milliseconds duration) const;
    [[nodiscard]] std::string formatBytes(std::uintmax_t size_bytes) const;
    [[nodiscard]] std::string messageToString(const endstone::Message &message) const;
    [[nodiscard]] bool saveQueryLooksReady(const std::vector<std::string> &messages, bool dispatch_result) const;
    [[nodiscard]] std::vector<BackupTargetConfig> resolveTargets(const BackupConfig &config,
                                                                 const TemplateValues &values) const;
    [[nodiscard]] std::vector<StoredBackup> collectBackups(const BackupConfig &config) const;
    [[nodiscard]] std::size_t pruneBackupsInternal(const BackupConfig &config, const std::filesystem::path &keep_path,
                                                   std::vector<std::string> *removed_names) const;

    static SnapshotResult createSnapshot(const BackupContext &context);
    static FinalizeResult finalizeBackup(const BackupContext &context, const SnapshotResult &snapshot);
    static RestoreResult performRestore(const RestoreContext &context);

    VaultstonePlugin &plugin_;
    BackupConfig config_;
    std::shared_ptr<BackupContext> current_;
    std::shared_ptr<RestoreContext> restore_current_;
    std::shared_ptr<endstone::Task> save_query_task_;
    std::shared_ptr<endstone::Task> phase_monitor_task_;
    std::shared_ptr<endstone::Task> schedule_task_;
    std::optional<PendingRestoreRequest> pending_restore_;
    std::optional<BackupSummary> last_backup_;
    std::optional<std::chrono::system_clock::time_point> next_scheduled_run_;
    std::string last_error_;
    bool runtime_schedule_enabled_ = false;
    std::future<RestoreResult> restore_future_;
};

}  // namespace backupper
