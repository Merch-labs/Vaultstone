#include "backupper_plugin.h"

namespace {

bool parsePositiveInt(const std::string &value, int &parsed)
{
    try {
        std::size_t consumed = 0;
        const int result = std::stoi(value, &consumed);
        if (consumed != value.size() || result < 1) {
            return false;
        }
        parsed = result;
        return true;
    }
    catch (...) {
        return false;
    }
}

std::string joinLabel(const std::vector<std::string> &args, const std::size_t start_index)
{
    std::string label;
    for (std::size_t index = start_index; index < args.size(); ++index) {
        if (!label.empty()) {
            label.push_back(' ');
        }
        label.append(args[index]);
    }
    return label;
}

}  // namespace

ENDSTONE_PLUGIN("vaultstone", "0.1.0", BackupperPlugin)
{
    prefix = "Vaultstone";
    description = "Configurable hot-backup plugin for Endstone Bedrock servers.";
    authors = {"Vaultstone contributors"};

    command("vaultstone")
        .description("Manage world backups.")
        .usages("/vaultstone status", "/vaultstone create [label: string]", "/vaultstone restore <name|latest>",
                "/vaultstone reload", "/vaultstone list [limit: int]", "/vaultstone prune",
                "/vaultstone delete <name: string>", "/vaultstone schedule <status|start|stop>")
        .aliases("backup")
        .permissions("vaultstone.command.status", "vaultstone.command.create", "vaultstone.command.reload",
                     "vaultstone.command.restore", "vaultstone.command.list", "vaultstone.command.prune",
                     "vaultstone.command.delete", "vaultstone.command.schedule");

    permission("vaultstone.command")
        .description("Allows access to all Vaultstone commands.")
        .children("vaultstone.command.status", true)
        .children("vaultstone.command.create", true)
        .children("vaultstone.command.reload", true)
        .children("vaultstone.command.restore", true)
        .children("vaultstone.command.list", true)
        .children("vaultstone.command.prune", true)
        .children("vaultstone.command.delete", true)
        .children("vaultstone.command.schedule", true);

    permission("vaultstone.command.status")
        .description("Allows viewing Vaultstone status.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.create")
        .description("Allows starting a manual backup.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.reload")
        .description("Allows reloading the Vaultstone config.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.restore")
        .description("Allows restoring a stored backup.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.list")
        .description("Allows listing available backups.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.prune")
        .description("Allows pruning old backups.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.delete")
        .description("Allows deleting specific backups.")
        .default_(endstone::PermissionDefault::Operator);

    permission("vaultstone.command.schedule")
        .description("Allows managing the automatic backup scheduler.")
        .default_(endstone::PermissionDefault::Operator);
}

void BackupperPlugin::onLoad()
{
    getLogger().info("Loading Vaultstone.");
}

void BackupperPlugin::onEnable()
{
    std::filesystem::create_directories(getDataFolder());
    manager_ = std::make_unique<backupper::BackupManager>(*this);
    manager_->onEnable();
    getLogger().info("Vaultstone enabled with data folder '{}'.", getDataFolder().string());
}

void BackupperPlugin::onDisable()
{
    if (manager_) {
        manager_->onDisable();
    }
    getLogger().info("Vaultstone disabled.");
}

bool BackupperPlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                                const std::vector<std::string> &args)
{
    if (command.getName() != "vaultstone" && command.getName() != "backup") {
        return false;
    }

    const auto subcommand = args.empty() ? std::string("status") : args[0];
    if (subcommand == "status") {
        manager_->sendStatus(sender);
        return true;
    }

    if (subcommand == "create" || subcommand == "backup") {
        std::string error;
        const auto label = joinLabel(args, 1);
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
        sender.sendMessage("Vaultstone config reloaded.");
        return true;
    }

    if (subcommand == "restore") {
        if (args.size() < 2) {
            sender.sendErrorMessage("Usage: /vaultstone restore <name|latest>");
            return true;
        }
        std::string error;
        if (!manager_->startRestore(sender.getName(), args[1], error)) {
            sender.sendErrorMessage("Unable to start restore: {}", error);
            return true;
        }
        sender.sendMessage("Restore started for '{}'.", args[1]);
        return true;
    }

    if (subcommand == "list") {
        std::size_t limit = 10;
        if (args.size() > 1) {
            int parsed_limit = 0;
            if (!parsePositiveInt(args[1], parsed_limit)) {
                sender.sendErrorMessage("Usage: /vaultstone list [limit: positive integer]");
                return true;
            }
            limit = static_cast<std::size_t>(parsed_limit);
        }
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
            sender.sendErrorMessage("Usage: /vaultstone delete <name>");
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
        sender.sendErrorMessage("Usage: /vaultstone schedule <status|start|stop>");
        return true;
    }

    sender.sendErrorMessage("Usage: /vaultstone <status|create|restore|reload|list|prune|delete|schedule>");
    return true;
}
