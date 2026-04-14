#include <libpldm/instance-id.h>
#include <unistd.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" int pldm_instance_db_init_default(struct pldm_instance_db** ctx)
{
    static uint64_t dbIndex = 0;
    static std::deque<std::string> dbPaths;

    const char* root = std::getenv("PLDMTOOL_INSTANCE_DB_DIR");
    const std::filesystem::path dbRoot =
        root == nullptr ? std::filesystem::temp_directory_path() : root;
    std::filesystem::create_directories(dbRoot);

    dbPaths.emplace_back((dbRoot / ("pldmtool_iid_" + std::to_string(getpid()) +
                                    "_" + std::to_string(dbIndex++)))
                             .string());
    auto& dbPath = dbPaths.back();

    std::ofstream ofs(dbPath, std::ios::binary | std::ios::trunc);
    std::string data(256 * 32, '\0');
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));

    return pldm_instance_db_init(ctx, dbPath.c_str());
}
