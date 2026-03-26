#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace backupper {

struct ArchiveEntry {
    std::filesystem::path source_path;
    std::string archive_path;
};

void writeZipArchive(const std::filesystem::path &archive_path, const std::vector<ArchiveEntry> &entries,
                     const std::string &manifest_json, int compression_level, bool verify_after_write);

}  // namespace backupper
