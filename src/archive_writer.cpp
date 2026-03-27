#include "archive_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <miniz.h>

namespace backupper {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kTarBlockSize = 512;
constexpr std::size_t kStreamBufferSize = 64 * 1024;

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

std::string readStringField(const std::array<char, kTarBlockSize> &header, const std::size_t offset, const std::size_t length)
{
    const auto *begin = header.data() + offset;
    const auto *end = begin;
    while (end < begin + length && *end != '\0') {
        ++end;
    }
    return std::string(begin, end);
}

void setStringField(std::array<char, kTarBlockSize> &header, const std::size_t offset, const std::size_t length,
                    const std::string_view value)
{
    if (value.size() > length) {
        throw std::runtime_error("Tar header field is too long.");
    }
    std::fill(header.begin() + static_cast<std::ptrdiff_t>(offset),
              header.begin() + static_cast<std::ptrdiff_t>(offset + length), '\0');
    std::memcpy(header.data() + offset, value.data(), value.size());
}

std::uint64_t parseOctalField(const std::array<char, kTarBlockSize> &header, const std::size_t offset, const std::size_t length)
{
    std::uint64_t value = 0;
    bool has_digit = false;
    for (std::size_t i = offset; i < offset + length; ++i) {
        const char character = header[i];
        if (character == '\0' || character == ' ') {
            continue;
        }
        if (character < '0' || character > '7') {
            throw std::runtime_error("Tar header contains an invalid octal field.");
        }
        value = (value * 8U) + static_cast<std::uint64_t>(character - '0');
        has_digit = true;
    }
    return has_digit ? value : 0;
}

void setOctalField(std::array<char, kTarBlockSize> &header, const std::size_t offset, const std::size_t length,
                   const std::uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%0*llo", static_cast<int>(length - 1), static_cast<unsigned long long>(value));
    const auto digits = std::strlen(buffer);
    if (digits > length - 1) {
        throw std::runtime_error("Value is too large for the tar header.");
    }

    std::fill(header.begin() + static_cast<std::ptrdiff_t>(offset),
              header.begin() + static_cast<std::ptrdiff_t>(offset + length), '0');
    std::memcpy(header.data() + offset + (length - 1 - digits), buffer, digits);
    header[offset + length - 1] = '\0';
}

void setChecksumField(std::array<char, kTarBlockSize> &header, const std::uint32_t checksum)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%06o", checksum);
    std::memcpy(header.data() + 148, buffer, 6);
    header[154] = '\0';
    header[155] = ' ';
}

std::uint32_t computeTarHeaderChecksum(const std::array<char, kTarBlockSize> &header)
{
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < header.size(); ++i) {
        const unsigned char byte = (i >= 148 && i < 156) ? static_cast<unsigned char>(' ')
                                                         : static_cast<unsigned char>(header[i]);
        checksum += byte;
    }
    return checksum;
}

std::pair<std::string, std::string> splitTarPath(const std::string &path)
{
    if (path.size() <= 100) {
        return {path, ""};
    }

    std::size_t split = path.rfind('/');
    while (split != std::string::npos) {
        const auto prefix = path.substr(0, split);
        const auto name = path.substr(split + 1);
        if (prefix.size() <= 155 && name.size() <= 100) {
            return {name, prefix};
        }
        if (split == 0) {
            break;
        }
        split = path.rfind('/', split - 1);
    }

    throw std::runtime_error("Archive path is too long for tar format: " + path);
}

void writeLittleEndian32(std::ostream &output, const std::uint32_t value)
{
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>(value & 0xFFU),
        static_cast<unsigned char>((value >> 8U) & 0xFFU),
        static_cast<unsigned char>((value >> 16U) & 0xFFU),
        static_cast<unsigned char>((value >> 24U) & 0xFFU),
    };
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t readLittleEndian32(std::istream &input)
{
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Unexpected end of file while reading gzip trailer.");
    }
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint16_t readLittleEndian16(std::istream &input)
{
    std::array<unsigned char, 2> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Unexpected end of file while reading gzip header.");
    }
    return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

void skipStreamBytes(std::istream &input, std::uint64_t bytes)
{
    std::array<char, kStreamBufferSize> buffer{};
    while (bytes > 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(bytes, buffer.size()));
        input.read(buffer.data(), chunk);
        if (input.gcount() != chunk) {
            throw std::runtime_error("Unexpected end of stream while skipping archive data.");
        }
        bytes -= static_cast<std::uint64_t>(chunk);
    }
}

void copyFileIntoStream(const fs::path &source_path, std::ostream &output)
{
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open file for archiving: " + source_path.string());
    }

    std::array<char, kStreamBufferSize> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
        }
    }

    if (!input.eof()) {
        throw std::runtime_error("Failed while reading file for archiving: " + source_path.string());
    }
}

void copyStreamIntoFile(std::istream &input, const fs::path &destination_path, std::uint64_t bytes)
{
    fs::create_directories(destination_path.parent_path());
    std::ofstream output(destination_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to create extracted file: " + destination_path.string());
    }

    std::array<char, kStreamBufferSize> buffer{};
    while (bytes > 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(bytes, buffer.size()));
        input.read(buffer.data(), chunk);
        if (input.gcount() != chunk) {
            throw std::runtime_error("Unexpected end of archive while extracting file.");
        }
        output.write(buffer.data(), chunk);
        if (!output) {
            throw std::runtime_error("Failed while writing extracted file: " + destination_path.string());
        }
        bytes -= static_cast<std::uint64_t>(chunk);
    }
}

void writeTarEntryHeader(std::ostream &output, const std::string &archive_path, const std::uint64_t size_bytes,
                         const char type_flag)
{
    const auto normalized = fs::path(archive_path).lexically_normal().generic_string();
    if (pathEscapesRoot(fs::path(normalized))) {
        throw std::runtime_error("Archive entry path is unsafe: " + archive_path);
    }

    const auto [name, prefix] = splitTarPath(normalized);
    std::array<char, kTarBlockSize> header{};
    setStringField(header, 0, 100, name);
    setOctalField(header, 100, 8, type_flag == '5' ? 0755U : 0644U);
    setOctalField(header, 108, 8, 0);
    setOctalField(header, 116, 8, 0);
    setOctalField(header, 124, 12, size_bytes);
    setOctalField(header, 136, 12, static_cast<std::uint64_t>(std::time(nullptr)));
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = type_flag;
    setStringField(header, 257, 6, "ustar");
    setStringField(header, 263, 2, "00");
    setStringField(header, 345, 155, prefix);
    setChecksumField(header, computeTarHeaderChecksum(header));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void writeTarPadding(std::ostream &output, const std::uint64_t size_bytes)
{
    const auto padding = (kTarBlockSize - (size_bytes % kTarBlockSize)) % kTarBlockSize;
    if (padding == 0) {
        return;
    }

    std::array<char, kTarBlockSize> zeros{};
    output.write(zeros.data(), static_cast<std::streamsize>(padding));
}

void writeTarArchive(const fs::path &archive_path, const std::vector<ArchiveEntry> &entries, const std::string &manifest_json)
{
    std::ofstream output(archive_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to create tar archive: " + archive_path.string());
    }

    for (const auto &entry : entries) {
        const auto size_bytes = fs::file_size(entry.source_path);
        writeTarEntryHeader(output, entry.archive_path, size_bytes, '0');
        copyFileIntoStream(entry.source_path, output);
        writeTarPadding(output, size_bytes);
    }

    if (!manifest_json.empty()) {
        writeTarEntryHeader(output, "backupper-manifest.json", manifest_json.size(), '0');
        output.write(manifest_json.data(), static_cast<std::streamsize>(manifest_json.size()));
        writeTarPadding(output, manifest_json.size());
    }

    std::array<char, kTarBlockSize> zeros{};
    output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    if (!output) {
        throw std::runtime_error("Failed while finalizing tar archive.");
    }
}

struct TarEntryHeader {
    std::string path;
    char type_flag = '0';
    std::uint64_t size_bytes = 0;
};

bool tryReadTarEntryHeader(std::istream &input, TarEntryHeader &entry)
{
    std::array<char, kTarBlockSize> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (input.gcount() == 0) {
        return false;
    }
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        throw std::runtime_error("Unexpected end of tar archive.");
    }

    const bool zero_block = std::all_of(header.begin(), header.end(), [](const char character) { return character == '\0'; });
    if (zero_block) {
        return false;
    }

    const auto stored_checksum = parseOctalField(header, 148, 8);
    if (stored_checksum != computeTarHeaderChecksum(header)) {
        throw std::runtime_error("Tar archive header checksum is invalid.");
    }

    const auto name = readStringField(header, 0, 100);
    const auto prefix = readStringField(header, 345, 155);
    entry.path = prefix.empty() ? name : (prefix + "/" + name);
    entry.type_flag = header[156] == '\0' ? '0' : header[156];
    entry.size_bytes = parseOctalField(header, 124, 12);
    return true;
}

void skipTarEntryPadding(std::istream &input, const std::uint64_t size_bytes)
{
    const auto padding = (kTarBlockSize - (size_bytes % kTarBlockSize)) % kTarBlockSize;
    skipStreamBytes(input, padding);
}

std::size_t validateTarArchive(std::istream &input)
{
    std::size_t file_count = 0;
    TarEntryHeader entry;
    while (tryReadTarEntryHeader(input, entry)) {
        if (entry.path.empty()) {
            throw std::runtime_error("Tar archive contains an entry with an empty path.");
        }
        if (entry.type_flag == '0') {
            ++file_count;
        }
        skipStreamBytes(input, entry.size_bytes);
        skipTarEntryPadding(input, entry.size_bytes);
    }
    return file_count;
}

void extractTarStream(std::istream &input, const fs::path &destination_root)
{
    fs::create_directories(destination_root);
    TarEntryHeader entry;
    while (tryReadTarEntryHeader(input, entry)) {
        const auto relative = fs::path(entry.path).lexically_normal();
        if (pathEscapesRoot(relative)) {
            throw std::runtime_error("Archive contains an unsafe entry path: " + entry.path);
        }

        const auto destination = destination_root / relative;
        if (entry.type_flag == '5') {
            fs::create_directories(destination);
            continue;
        }
        if (entry.type_flag != '0') {
            skipStreamBytes(input, entry.size_bytes);
            skipTarEntryPadding(input, entry.size_bytes);
            continue;
        }

        copyStreamIntoFile(input, destination, entry.size_bytes);
        skipTarEntryPadding(input, entry.size_bytes);
    }
}

struct GzipInfo {
    std::streamoff compressed_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint32_t expected_size = 0;
};

GzipInfo readGzipInfo(const fs::path &archive_path)
{
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open gzip archive: " + archive_path.string());
    }

    std::array<unsigned char, 10> header{};
    input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input) {
        throw std::runtime_error("Gzip archive is truncated.");
    }
    if (header[0] != 0x1F || header[1] != 0x8B || header[2] != 8) {
        throw std::runtime_error("File is not a supported gzip archive.");
    }

    const auto flags = header[3];
    if ((flags & 0xE0U) != 0) {
        throw std::runtime_error("Gzip archive uses unsupported header flags.");
    }

    if ((flags & 0x04U) != 0) {
        const auto extra_size = readLittleEndian16(input);
        skipStreamBytes(input, extra_size);
    }
    if ((flags & 0x08U) != 0) {
        char character = '\0';
        do {
            input.get(character);
            if (!input) {
                throw std::runtime_error("Gzip archive filename field is truncated.");
            }
        } while (character != '\0');
    }
    if ((flags & 0x10U) != 0) {
        char character = '\0';
        do {
            input.get(character);
            if (!input) {
                throw std::runtime_error("Gzip archive comment field is truncated.");
            }
        } while (character != '\0');
    }
    if ((flags & 0x02U) != 0) {
        skipStreamBytes(input, 2);
    }

    const auto compressed_offset = input.tellg();
    input.seekg(-8, std::ios::end);
    if (!input) {
        throw std::runtime_error("Gzip archive is too small.");
    }

    GzipInfo info;
    info.expected_crc32 = readLittleEndian32(input);
    info.expected_size = readLittleEndian32(input);
    const auto trailer_offset = input.tellg() - static_cast<std::streamoff>(8);
    if (trailer_offset < compressed_offset) {
        throw std::runtime_error("Gzip archive is malformed.");
    }
    info.compressed_offset = compressed_offset;
    info.compressed_size = static_cast<std::uint64_t>(trailer_offset - compressed_offset);
    return info;
}

void compressFileToGzip(const fs::path &source_path, const fs::path &archive_path, const int compression_level)
{
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open tar payload for gzip compression: " + source_path.string());
    }

    std::ofstream output(archive_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to create gzip archive: " + archive_path.string());
    }

    const std::array<unsigned char, 10> header = {0x1F, 0x8B, 0x08, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0xFF};
    output.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));

    mz_stream stream{};
    if (mz_deflateInit2(&stream, compression_level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) !=
        MZ_OK) {
        throw std::runtime_error("Failed to initialize gzip compressor.");
    }

    std::array<unsigned char, kStreamBufferSize> input_buffer{};
    std::array<unsigned char, kStreamBufferSize> output_buffer{};
    std::uint32_t crc32_value = 0;
    std::uint32_t input_size = 0;

    try {
        while (input) {
            input.read(reinterpret_cast<char *>(input_buffer.data()), static_cast<std::streamsize>(input_buffer.size()));
            const auto read_count = static_cast<std::size_t>(input.gcount());
            if (read_count == 0 && input.eof()) {
                break;
            }
            if (read_count == 0) {
                throw std::runtime_error("Failed while reading data for gzip compression.");
            }

            crc32_value = static_cast<std::uint32_t>(mz_crc32(crc32_value, input_buffer.data(), read_count));
            input_size += static_cast<std::uint32_t>(read_count);

            stream.next_in = input_buffer.data();
            stream.avail_in = static_cast<mz_uint>(read_count);
            while (stream.avail_in > 0) {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<mz_uint>(output_buffer.size());
                const auto status = mz_deflate(&stream, MZ_NO_FLUSH);
                if (status != MZ_OK) {
                    throw std::runtime_error("Gzip compression failed.");
                }
                const auto produced = output_buffer.size() - stream.avail_out;
                if (produced > 0) {
                    output.write(reinterpret_cast<const char *>(output_buffer.data()),
                                 static_cast<std::streamsize>(produced));
                }
            }
        }

        while (true) {
            stream.next_out = output_buffer.data();
            stream.avail_out = static_cast<mz_uint>(output_buffer.size());
            const auto status = mz_deflate(&stream, MZ_FINISH);
            const auto produced = output_buffer.size() - stream.avail_out;
            if (produced > 0) {
                output.write(reinterpret_cast<const char *>(output_buffer.data()), static_cast<std::streamsize>(produced));
            }
            if (status == MZ_STREAM_END) {
                break;
            }
            if (status != MZ_OK) {
                throw std::runtime_error("Failed while finalizing gzip archive.");
            }
        }

        mz_deflateEnd(&stream);
        writeLittleEndian32(output, crc32_value);
        writeLittleEndian32(output, input_size);
        if (!output) {
            throw std::runtime_error("Failed while writing gzip archive.");
        }
    }
    catch (...) {
        mz_deflateEnd(&stream);
        throw;
    }
}

void decompressGzipFile(const fs::path &archive_path, const fs::path &destination_path)
{
    const auto info = readGzipInfo(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to reopen gzip archive: " + archive_path.string());
    }
    input.seekg(info.compressed_offset);

    fs::create_directories(destination_path.parent_path());
    std::ofstream output(destination_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to create decompressed tar payload.");
    }

    mz_stream stream{};
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        throw std::runtime_error("Failed to initialize gzip decompressor.");
    }

    std::array<unsigned char, kStreamBufferSize> input_buffer{};
    std::array<unsigned char, kStreamBufferSize> output_buffer{};
    std::uint64_t remaining = info.compressed_size;
    std::uint32_t crc32_value = 0;
    std::uint32_t output_size = 0;
    bool stream_finished = false;

    try {
        while (remaining > 0 && !stream_finished) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input_buffer.size()));
            input.read(reinterpret_cast<char *>(input_buffer.data()), static_cast<std::streamsize>(chunk));
            if (static_cast<std::size_t>(input.gcount()) != chunk) {
                throw std::runtime_error("Unexpected end of gzip archive.");
            }
            remaining -= chunk;

            stream.next_in = input_buffer.data();
            stream.avail_in = static_cast<mz_uint>(chunk);
            while (stream.avail_in > 0) {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<mz_uint>(output_buffer.size());
                const auto status = mz_inflate(&stream, MZ_NO_FLUSH);
                const auto produced = output_buffer.size() - stream.avail_out;
                if (produced > 0) {
                    crc32_value = static_cast<std::uint32_t>(mz_crc32(crc32_value, output_buffer.data(), produced));
                    output_size += static_cast<std::uint32_t>(produced);
                    output.write(reinterpret_cast<const char *>(output_buffer.data()),
                                 static_cast<std::streamsize>(produced));
                }
                if (status == MZ_STREAM_END) {
                    stream_finished = true;
                    break;
                }
                if (status != MZ_OK) {
                    throw std::runtime_error("Failed while decompressing gzip archive.");
                }
            }
        }

        mz_inflateEnd(&stream);
    }
    catch (...) {
        mz_inflateEnd(&stream);
        throw;
    }

    if (!stream_finished) {
        throw std::runtime_error("Gzip archive ended before the deflate stream completed.");
    }
    if (crc32_value != info.expected_crc32 || output_size != info.expected_size) {
        throw std::runtime_error("Gzip archive verification failed.");
    }
}

void verifyTarArchive(const fs::path &archive_path)
{
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to reopen tar archive for verification.");
    }
    if (validateTarArchive(input) == 0) {
        throw std::runtime_error("Archive verification failed: tar archive is empty.");
    }
}

void verifyTarGzArchive(const fs::path &archive_path)
{
    const auto temp_tar_path = archive_path.parent_path() / (archive_path.filename().string() + ".verify.tar");
    std::error_code cleanup_error;
    fs::remove_all(temp_tar_path, cleanup_error);
    decompressGzipFile(archive_path, temp_tar_path);
    try {
        verifyTarArchive(temp_tar_path);
        fs::remove_all(temp_tar_path, cleanup_error);
    }
    catch (...) {
        fs::remove_all(temp_tar_path, cleanup_error);
        throw;
    }
}

void writeZipArchive(const fs::path &archive_path, const std::vector<ArchiveEntry> &entries, const std::string &manifest_json,
                     const int compression_level, const bool verify_after_write)
{
    mz_zip_archive archive{};
    std::memset(&archive, 0, sizeof(archive));

    if (!mz_zip_writer_init_file(&archive, archive_path.string().c_str(), 0)) {
        throw std::runtime_error("Failed to initialize zip archive writer.");
    }

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

void extractZipArchive(const fs::path &archive_path, const fs::path &destination_root)
{
    mz_zip_archive archive{};
    std::memset(&archive, 0, sizeof(archive));

    if (!mz_zip_reader_init_file(&archive, archive_path.string().c_str(), 0)) {
        throw std::runtime_error("Failed to open zip archive for reading.");
    }

    try {
        fs::create_directories(destination_root);
        const auto file_count = mz_zip_reader_get_num_files(&archive);
        for (mz_uint index = 0; index < file_count; ++index) {
            mz_zip_archive_file_stat file_stat{};
            if (!mz_zip_reader_file_stat(&archive, index, &file_stat)) {
                throw std::runtime_error("Failed to read archive file metadata.");
            }

            const auto relative = fs::path(file_stat.m_filename);
            if (pathEscapesRoot(relative.lexically_normal())) {
                throw std::runtime_error("Archive contains an unsafe entry path: " + relative.string());
            }
            const auto destination = destination_root / relative;
            if (file_stat.m_is_directory) {
                fs::create_directories(destination);
                continue;
            }

            fs::create_directories(destination.parent_path());
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

}  // namespace

bool isSupportedArchiveFormat(const std::string_view archive_format)
{
    return archive_format == "zip" || archive_format == "tar" || archive_format == "tar.gz" ||
           archive_format == "directory";
}

std::string archiveFormatFileExtension(const std::string_view archive_format)
{
    if (archive_format == "zip") {
        return ".zip";
    }
    if (archive_format == "tar") {
        return ".tar";
    }
    if (archive_format == "tar.gz") {
        return ".tar.gz";
    }
    return {};
}

std::string detectArchiveFormat(const fs::path &archive_path)
{
    if (fs::is_directory(archive_path)) {
        return "directory";
    }

    const auto filename = archive_path.filename().string();
    if (filename.size() >= 7 && filename.ends_with(".tar.gz")) {
        return "tar.gz";
    }
    if (archive_path.extension() == ".zip") {
        return "zip";
    }
    if (archive_path.extension() == ".tar") {
        return "tar";
    }

    throw std::runtime_error("Unsupported backup archive format: " + archive_path.filename().string());
}

void writeArchive(const fs::path &archive_path, const std::string_view archive_format,
                  const std::vector<ArchiveEntry> &entries, const std::string &manifest_json,
                  const int compression_level, const bool verify_after_write)
{
    if (archive_format == "zip") {
        writeZipArchive(archive_path, entries, manifest_json, compression_level, verify_after_write);
        return;
    }
    if (archive_format == "tar") {
        writeTarArchive(archive_path, entries, manifest_json);
        if (verify_after_write) {
            verifyTarArchive(archive_path);
        }
        return;
    }
    if (archive_format == "tar.gz") {
        const auto temp_tar_path = archive_path.parent_path() / (archive_path.filename().string() + ".tar");
        std::error_code cleanup_error;
        fs::remove_all(temp_tar_path, cleanup_error);
        writeTarArchive(temp_tar_path, entries, manifest_json);
        try {
            compressFileToGzip(temp_tar_path, archive_path, compression_level);
            fs::remove_all(temp_tar_path, cleanup_error);
            if (verify_after_write) {
                verifyTarGzArchive(archive_path);
            }
        }
        catch (...) {
            fs::remove_all(temp_tar_path, cleanup_error);
            throw;
        }
        return;
    }

    throw std::runtime_error("Unsupported archive format for writing: " + std::string(archive_format));
}

void extractArchive(const fs::path &archive_path, const std::string_view archive_format, const fs::path &destination_root)
{
    if (archive_format == "zip") {
        extractZipArchive(archive_path, destination_root);
        return;
    }
    if (archive_format == "tar") {
        std::ifstream input(archive_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open tar archive for reading.");
        }
        extractTarStream(input, destination_root);
        return;
    }
    if (archive_format == "tar.gz") {
        const auto temp_tar_path = destination_root.parent_path() / (archive_path.filename().string() + ".extract.tar");
        std::error_code cleanup_error;
        fs::remove_all(temp_tar_path, cleanup_error);
        decompressGzipFile(archive_path, temp_tar_path);
        try {
            std::ifstream input(temp_tar_path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("Failed to open decompressed tar payload.");
            }
            extractTarStream(input, destination_root);
            fs::remove_all(temp_tar_path, cleanup_error);
        }
        catch (...) {
            fs::remove_all(temp_tar_path, cleanup_error);
            throw;
        }
        return;
    }

    throw std::runtime_error("Unsupported archive format for extraction: " + std::string(archive_format));
}

}  // namespace backupper
