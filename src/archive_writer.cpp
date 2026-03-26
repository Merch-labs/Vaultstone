#include "archive_writer.h"

#include <cstring>
#include <stdexcept>

#include <miniz.h>

namespace backupper {

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

}  // namespace backupper
