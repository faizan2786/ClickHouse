#include <Backups/BackupCoordinationLocal.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <fmt/format.h>


namespace DB
{

using SizeAndChecksum = IBackupCoordination::SizeAndChecksum;
using FileInfo = IBackupCoordination::FileInfo;

BackupCoordinationLocal::BackupCoordinationLocal() : log(&Poco::Logger::get("BackupCoordination"))
{
}

BackupCoordinationLocal::~BackupCoordinationLocal() = default;

void BackupCoordinationLocal::addReplicatedPartNames(const String & /* host_id */, const StorageID & table_id, const std::vector<PartNameAndChecksum> & part_names_and_checksums, const String & table_zk_path)
{
    std::lock_guard lock{mutex};
    replicated_part_names.addPartNames("", table_id, part_names_and_checksums, table_zk_path);
}

bool BackupCoordinationLocal::hasReplicatedPartNames(const String & /* host_id */, const StorageID & table_id) const
{
    std::lock_guard lock{mutex};
    return replicated_part_names.has("", table_id);
}

void BackupCoordinationLocal::addReplicatedTableDataPath(const String & /* host_id */, const StorageID & table_id, const String & table_data_path)
{
    std::lock_guard lock{mutex};
    replicated_part_names.addDataPath("", table_id, table_data_path);
}

void BackupCoordinationLocal::finishPreparing(const String & /* host_id */, const String & error_message)
{
    LOG_TRACE(log, "Finished preparing{}", (error_message.empty() ? "" : (" with error " + error_message)));
    if (!error_message.empty())
        return;

    replicated_part_names.preparePartNames();
}

void BackupCoordinationLocal::waitForAllHostsPrepared(const Strings & /* host_ids */, std::chrono::seconds /* timeout */) const
{
}

Strings BackupCoordinationLocal::getReplicatedPartNames(const String & /* host_id */, const StorageID & table_id) const
{
    std::lock_guard lock{mutex};
    return replicated_part_names.getPartNames("", table_id);
}

Strings BackupCoordinationLocal::getReplicatedTableDataPaths(const String & /* host_id */, const StorageID & table_id) const
{
    std::lock_guard lock{mutex};
    return replicated_part_names.getDataPaths("", table_id);
}


void BackupCoordinationLocal::addFileInfo(const FileInfo & file_info, bool & is_data_file_required)
{
    std::lock_guard lock{mutex};
    file_names.emplace(file_info.file_name, std::pair{file_info.size, file_info.checksum});
    if (!file_info.size)
    {
        is_data_file_required = false;
        return;
    }
    bool inserted_file_info = file_infos.try_emplace(std::pair{file_info.size, file_info.checksum}, file_info).second;
    is_data_file_required = inserted_file_info && (file_info.size > file_info.base_size);
}

void BackupCoordinationLocal::updateFileInfo(const FileInfo & file_info)
{
    if (!file_info.size)
        return; /// we don't keep FileInfos for empty files, nothing to update

    std::lock_guard lock{mutex};
    auto & dest = file_infos.at(std::pair{file_info.size, file_info.checksum});
    dest.archive_suffix = file_info.archive_suffix;
}

std::vector<FileInfo> BackupCoordinationLocal::getAllFileInfos() const
{
    std::lock_guard lock{mutex};
    std::vector<FileInfo> res;
    for (const auto & [file_name, size_and_checksum] : file_names)
    {
        FileInfo info;
        UInt64 size = size_and_checksum.first;
        if (size) /// we don't keep FileInfos for empty files
            info = file_infos.at(size_and_checksum);
        info.file_name = file_name;
        res.push_back(std::move(info));
    }
    return res;
}

Strings BackupCoordinationLocal::listFiles(const String & prefix, const String & terminator) const
{
    std::lock_guard lock{mutex};
    Strings elements;
    for (auto it = file_names.lower_bound(prefix); it != file_names.end(); ++it)
    {
        const String & name = it->first;
        if (!name.starts_with(prefix))
            break;
        size_t start_pos = prefix.length();
        size_t end_pos = String::npos;
        if (!terminator.empty())
            end_pos = name.find(terminator, start_pos);
        std::string_view new_element = std::string_view{name}.substr(start_pos, end_pos - start_pos);
        if (!elements.empty() && (elements.back() == new_element))
            continue;
        elements.push_back(String{new_element});
    }
    return elements;
}

std::optional<FileInfo> BackupCoordinationLocal::getFileInfo(const String & file_name) const
{
    std::lock_guard lock{mutex};
    auto it = file_names.find(file_name);
    if (it == file_names.end())
        return std::nullopt;
    const auto & size_and_checksum = it->second;
    UInt64 size = size_and_checksum.first;
    FileInfo info;
    if (size) /// we don't keep FileInfos for empty files
        info = file_infos.at(size_and_checksum);
    info.file_name = file_name;
    return info;
}

std::optional<FileInfo> BackupCoordinationLocal::getFileInfo(const SizeAndChecksum & size_and_checksum) const
{
    std::lock_guard lock{mutex};
    auto it = file_infos.find(size_and_checksum);
    if (it == file_infos.end())
        return std::nullopt;
    return it->second;
}

std::optional<SizeAndChecksum> BackupCoordinationLocal::getFileSizeAndChecksum(const String & file_name) const
{
    std::lock_guard lock{mutex};
    auto it = file_names.find(file_name);
    if (it == file_names.end())
        return std::nullopt;
    return it->second;
}

String BackupCoordinationLocal::getNextArchiveSuffix()
{
    std::lock_guard lock{mutex};
    String new_archive_suffix = fmt::format("{:03}", ++current_archive_suffix); /// Outputs 001, 002, 003, ...
    archive_suffixes.push_back(new_archive_suffix);
    return new_archive_suffix;
}

Strings BackupCoordinationLocal::getAllArchiveSuffixes() const
{
    std::lock_guard lock{mutex};
    return archive_suffixes;
}

}
