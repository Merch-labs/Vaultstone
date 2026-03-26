#pragma once

#include "backup_manager.h"

#include <endstone/endstone.hpp>

#include <memory>

class BackupperPlugin : public endstone::Plugin {
public:
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override;

private:
    std::unique_ptr<backupper::BackupManager> manager_;
};
