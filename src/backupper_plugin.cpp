#include "backupper_plugin.h"

#include <filesystem>

namespace fs = std::filesystem;

ENDSTONE_PLUGIN("backupper", "0.1.0", BackupperPlugin)
{
    prefix = "Backupper";
    description = "Configurable hot-backup plugin for Endstone Bedrock servers.";
    authors = {"Backupper contributors"};
    website = "https://github.com/EndstoneMC/endstone";

    command("backupper")
        .description("Inspect the Backupper plugin.")
        .usages("/backupper status")
        .aliases("backup")
        .permissions("backupper.command.status");

    permission("backupper.command")
        .description("Allows access to all Backupper commands.")
        .children("backupper.command.status", true);

    permission("backupper.command.status")
        .description("Allows viewing Backupper status.")
        .default_(endstone::PermissionDefault::Operator);
}

void BackupperPlugin::onLoad()
{
    getLogger().info("Loading Backupper.");
}

void BackupperPlugin::onEnable()
{
    fs::create_directories(getDataFolder());
    getLogger().info("Backupper enabled.");
}

void BackupperPlugin::onDisable()
{
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
        sender.sendMessage("Backupper is loaded. Data folder: {}", getDataFolder().string());
        return true;
    }

    sender.sendErrorMessage("Usage: /backupper status");
    return true;
}
