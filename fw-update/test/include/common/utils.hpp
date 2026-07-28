#pragma once

#include "../../../../common/utils.hpp"

#include <boost/system/error_code.hpp>

#include <memory>
#include <utility>

namespace pldm::utils
{

class TestLoggingAsioConnection
{
  public:
    template <typename Callback, typename... Args>
    void async_method_call(Callback&& callback, Args&&...)
    {
        std::forward<Callback>(callback)(boost::system::error_code{});
    }

    template <typename Callback, typename... Args>
    void async_method_call_timed(Callback&& callback, Args&&...)
    {
        std::forward<Callback>(callback)(boost::system::error_code{});
    }
};

class DBusLoggingTestHandler : public DBusHandler
{
  public:
    static auto& getAsioConnection()
    {
        static auto conn = std::make_shared<TestLoggingAsioConnection>();
        return conn;
    }
};

} // namespace pldm::utils

#define DBusHandler DBusLoggingTestHandler
