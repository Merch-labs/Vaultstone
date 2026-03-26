#include "archive_writer.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <miniz.h>

namespace backupper {
namespace {

bool pathEscapesRoot(const std::filesystem::path &relative_path)
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

void writeZipArchive(const std::filesystem::path &archive_path, const std::vector<ArchiveEntry> &entries,
                     const std::string &manifest_json, const int compression_level, const bool verify_after_write)
{
    mz_zip_archive archive{};
    std::memset(&archive, 0, sizeof(archive));

    if (!mz_zip_writer_init_file(&archive, archive_path.string().c_str(), 0)) {
        throw std::runtime_error("Failed to initialize zip archive writer.");
    }

    auto cleanup = [&archive]() {
        mz_zip_writer_finalize_archive(&archive);
        mz_zip_writer_end(&archive);
    };

    try {
        for (const auto &entry : entries) {
            if (!mz_zip_writer_add_file(&archive, entry.archive_path.c_str(), entry.source_path.string().c_str(), nullptr, 0,
                                        compression_level)) {
                throw std::runtime_error("Failed to add file to archive: " + entry.archive_path);
            }
        }

        if (!manifest_json.empty()) {
            if (!mz_zip_writer_add_mem(&archive, "backupper-manifest.json", manifest_json.data(), manifest_json.size(),
                                       compression_level)) {
                throw std::runtime_error("Failed to add backup manifest to archive.");
            }
        }

        if (!mz_zip_writer_finalize_archive(&archive)) {
            throw std::runtime_error("Failed to finalize zip archive.");
        }
        mz_zip_writer_end(&archive);
    }
    catch (...) {
        mz_zip_writer_end(&archive);
        throw;
    }

    if (verify_after_write) {
        mz_zip_archive reader{};
        std::memset(&reader, 0, sizeof(reader));
        if (!mz_zip_reader_init_file(&reader, archive_path.string().c_str(), 0)) {
            throw std::runtime_error("Archive verification failed: unable to reopen zip.");
        }
        if (mz_zip_reader_get_num_files(&reader) == 0) {
            mz_zip_reader_end(&reader);
            throw std::runtime_error("Archive verification failed: archive is empty.");
        }
        mz_zip_reader_end(&reader);
    }
}

void extractZipArchive(const std::filesystem::path &archive_path, const std::filesystem::path &destination_root)
{
    mz_zip_archive archive{};
    std::memset(&archive, 0, sizeof(archive));

    if (!mz_zip_reader_init_file(&archive, archive_path.string().c_str(), 0)) {
        throw std::runtime_error("Failed to open zip archive for reading.");
    }

    try {
        std::filesystem::create_directories(destination_root);
        const auto file_count = mz_zip_reader_get_num_files(&archive);
        for (mz_uint index = 0; index < file_count; ++index) {
            mz_zip_archive_file_stat file_stat{};
            if (!mz_zip_reader_file_stat(&archive, index, &file_stat)) {
                throw std::runtime_error("Failed to read archive file metadata.");
            }

            const auto relative = std::filesystem::path(file_stat.m_filename);
            if (pathEscapesRoot(relative.lexically_normal())) {
                throw std::runtime_error("Archive contains an unsafe entry path: " + relative.string());
            }
            const auto destination = destination_root / relative;
            if (file_stat.m_is_directory) {
                std::filesystem::create_directories(destination);
                continue;
            }

            std::filesystem::create_directories(destination.parent_path());
            if (!mz_zip_reader_extract_to_file(&archive, index, destination.string().c_str(), 0)) {
                throw std::runtime_error("Failed to extract archive entry: " + relative.string());
            }
        }
        mz_zip_reader_end(&archive);
    }
    catch (...) {
        mz_zip_reader_end(&archive);
        throw;
    }
}

}  // namespace backupper
