#pragma once

#include "common/instance_id.hpp"
#include "test/test_tmp_utils.hpp"

#include <unistd.h>

#include <filesystem>

static constexpr uintmax_t pldmMaxInstanceIds = 32;

class TestInstanceIdDb : public pldm::InstanceIdDb
{
  public:
    TestInstanceIdDb() : TestInstanceIdDb(createDb()) {}

    ~TestInstanceIdDb()
    {
        std::filesystem::remove(dbPath);
    };

  private:
    static std::filesystem::path createDb()
    {
        auto dbPath = pldm::test::makeTempFile("db.XXXXXX");
        std::filesystem::resize_file(
            dbPath, static_cast<uintmax_t>(PLDM_MAX_TIDS) * pldmMaxInstanceIds);

        return dbPath;
    };

    TestInstanceIdDb(std::filesystem::path dbPath) :
        InstanceIdDb(dbPath), dbPath(dbPath)
    {}

    std::filesystem::path dbPath;
};
