#pragma once

#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

namespace backupper {

struct ArchiveEntry {
    std::filesystem::path source_path;
    std::string archive_path;
};

bool isSupportedArchiveFormat(std::string_view archive_format);
std::string archiveFormatFileExtension(std::string_view archive_format);
std::string detectArchiveFormat(const std::filesystem::path &archive_path);
void writeArchive(const std::filesystem::path &archive_path, std::string_view archive_format,
                  const std::vector<ArchiveEntry> &entries, const std::string &manifest_json, int compression_level,
                  bool verify_after_write);
void extractArchive(const std::filesystem::path &archive_path, std::string_view archive_format,
                    const std::filesystem::path &destination_root);

}  // namespace backupper
