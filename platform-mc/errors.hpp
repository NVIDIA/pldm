#pragma once

#include <sdbusplus/exception.hpp>

#include <string>
#include <string_view>

namespace errors
{
using namespace std::literals::string_literals;
class InvalidArgument final : public sdbusplus::exception::internal_exception
{
    public:
        explicit InvalidArgument(std::string_view propertyNameArg):
            propertyName(propertyNameArg),
            errWhatDetailed("Invalid argument was given for property: "s +
                             description()){}
        InvalidArgument(std::string_view propertyNameArg, std::string_view info):
            propertyName(propertyNameArg),
            errWhatDetailed(("Invalid argument was given for property: "s +
                            description() + ". "s).append(info)){}

        const char* name() const noexcept override
        {
            return "xyz.openbmc_project.Common.Error.InvalidArgument";
        }

        const char* description() const noexcept override
        {
            return "Out of range";
        }

        const char* what() const noexcept override
        {
            return errWhatDetailed.c_str();
        }

        int get_errno() const noexcept override
        {
            return static_cast<int>(std::errc::invalid_argument);
        }

        std::string propertyName;

    private:
        std::string errWhatDetailed;
};

} // namespace errors
