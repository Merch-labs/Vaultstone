#include "backupper_plugin.h"

ENDSTONE_PLUGIN("backupper", "0.1.0", BackupperPlugin)
{
    prefix = "Backupper";
    description = "Configurable hot-backup plugin for Endstone Bedrock servers.";
    authors = {"Backupper contributors"};

    command("backupper")
        .description("Manage world backups.")
        .usages("/backupper status", "/backupper backup [label: string]", "/backupper reload", "/backupper list [limit: int]",
                "/backupper prune", "/backupper delete <name: string>", "/backupper schedule <status|start|stop>")
        .aliases("backup")
        .permissions("backupper.command.status", "backupper.command.backup", "backupper.command.reload",
                     "backupper.command.list", "backupper.command.prune", "backupper.command.delete",
                     "backupper.command.schedule");

    permission("backupper.command")
        .description("Allows access to all Backupper commands.")
        .children("backupper.command.status", true)
        .children("backupper.command.backup", true)
        .children("backupper.command.reload", true)
        .children("backupper.command.list", true)
        .children("backupper.command.prune", true)
        .children("backupper.command.delete", true)
        .children("backupper.command.schedule", true);

    permission("backupper.command.status")
        .description("Allows viewing Backupper status.")
        .default_(endstone::PermissionDefault::Operator);

    permission("backupper.command.backup")
        .description("Allows starting a manual backup.")
        .default_(endstone::PermissionDefault::Operator);

    permission("backupper.command.reload")
        .description("Allows reloading the Backupper config.")
        .default_(endstone::PermissionDefault::Operator);

    permission("backupper.command.list")
        .description("Allows listing available backups.")
        .default_(endstone::PermissionDefault::Operator);

    permission("backupper.command.prune")
        .description("Allows pruning old backups.")
        .default_(endstone::PermissionDefault::Operator);

    permission("backupper.command.delete")
        .description("Allows deleting specific backups.")
        .default_(endstone::PermissionDefault::Operator);

    permission("backupper.command.schedule")
        .description("Allows managing the automatic backup scheduler.")
        .default_(endstone::PermissionDefault::Operator);
}

void BackupperPlugin::onLoad()
{
    getLogger().info("Loading Backupper.");
}

void BackupperPlugin::onEnable()
{
    std::filesystem::create_directories(getDataFolder());
    manager_ = std::make_unique<backupper::BackupManager>(*this);
    manager_->onEnable();
    getLogger().info("Backupper enabled with data folder '{}'.", getDataFolder().string());
}

void BackupperPlugin::onDisable()
{
    if (manager_) {
        manager_->onDisable();
    }
    getLogger().info("Backupper disabled.");
}

bool BackupperPlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                                const std::vector<std::string> &args)
{
    if (command.getName() != "backupper") {
        return false;
    }

    const auto subcommand = args.empty() ? std::string("status") : args[0];
    if (subcommand == "status") {
        manager_->sendStatus(sender);
        return true;
    }

    if (subcommand == "backup") {
        std::string error;
        std::string label;
        if (args.size() > 1) {
            label = args[1];
            for (std::size_t index = 2; index < args.size(); ++index) {
                label.append(" ").append(args[index]);
            }
        }
        if (!manager_->startManualBackup(sender.getName(), label, error)) {
            sender.sendErrorMessage("Unable to start backup: {}", error);
            return true;
        }
        sender.sendMessage("Backup started.");
        return true;
    }

    if (subcommand == "reload") {
        std::string error;
        if (!manager_->reload(error)) {
            sender.sendErrorMessage("Reload failed: {}", error);
            return true;
        }
        sender.sendMessage("Backupper config reloaded.");
        return true;
    }

    if (subcommand == "list") {
        const auto limit = args.size() > 1 ? static_cast<std::size_t>(std::max(1, std::stoi(args[1]))) : 10U;
        const auto backups = manager_->listBackups(limit);
        if (backups.empty()) {
            sender.sendMessage("No backups found.");
            return true;
        }
        sender.sendMessage("Stored backups:");
        for (const auto &backup : backups) {
            sender.sendMessage("- {} ({})", backup.name, backup.path.string());
        }
        return true;
    }

    if (subcommand == "prune") {
        std::string error;
        std::size_t removed_count = 0;
        if (!manager_->prune(error, &removed_count)) {
            sender.sendErrorMessage("Prune failed: {}", error);
            return true;
        }
        sender.sendMessage("Pruned {} backup(s).", removed_count);
        return true;
    }

    if (subcommand == "delete") {
        if (args.size() < 2) {
            sender.sendErrorMessage("Usage: /backupper delete <name>");
            return true;
        }
        std::string error;
        if (!manager_->deleteBackup(args[1], error)) {
            sender.sendErrorMessage("Delete failed: {}", error);
            return true;
        }
        sender.sendMessage("Deleted backup '{}'.", args[1]);
        return true;
    }

    if (subcommand == "schedule") {
        const auto action = args.size() > 1 ? args[1] : std::string("status");
        std::string error;
        if (action == "status") {
            manager_->sendStatus(sender);
            return true;
        }
        if (action == "start") {
            if (!manager_->startSchedule(error)) {
                sender.sendErrorMessage("Unable to start scheduler: {}", error);
                return true;
            }
            sender.sendMessage("Scheduler started.");
            return true;
        }
        if (action == "stop") {
            if (!manager_->stopSchedule(error)) {
                sender.sendErrorMessage("Unable to stop scheduler: {}", error);
                return true;
            }
            sender.sendMessage("Scheduler stopped.");
            return true;
        }
        sender.sendErrorMessage("Usage: /backupper schedule <status|start|stop>");
        return true;
    }

    sender.sendErrorMessage("Usage: /backupper <status|backup|reload|list|prune|delete|schedule>");
    return true;
}
