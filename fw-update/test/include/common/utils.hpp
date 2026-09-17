#pragma once

#include "../../../../common/utils.hpp"

#include <boost/system/error_code.hpp>

#include <concepts>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace pldm::utils::detail
{

// GCC 15 ranks a variadic async_method_call above an overload whose
// MessageId parameter is a non-deduced const std::string&. Detect the
// Create log shape here so that call is recorded instead.
template <typename... Args>
inline constexpr bool isRecordedLoggingCall = false;

template <typename Service, typename Path, typename Interface, typename Method,
          typename MessageId, typename Severity, typename AddData>
inline constexpr bool isRecordedLoggingCall<Service, Path, Interface, Method,
                                            MessageId, Severity, AddData> =
    std::convertible_to<Service, std::string> &&
    std::convertible_to<Path, std::string> &&
    std::convertible_to<Interface, std::string> &&
    std::convertible_to<Method, std::string> &&
    std::convertible_to<MessageId, std::string> &&
    std::convertible_to<Severity, std::string> &&
    std::same_as<std::remove_cvref_t<AddData>,
                 std::map<std::string, std::string>>;

} // namespace pldm::utils::detail

namespace pldm::utils
{

struct RecordedLoggingCall
{
    std::string service;
    std::string path;
    std::string interface;
    std::string method;
    std::string messageId;
    std::string severity;
    std::map<std::string, std::string> additionalData;
};

struct UnmatchedAsyncMethodCall
{
    std::string method;
    std::size_t argumentCount;
};

class RecordingAsioConnection
{
  public:
    boost::system::error_code nextError{};
    std::vector<RecordedLoggingCall> loggingCalls{};
    std::vector<UnmatchedAsyncMethodCall> unmatchedCalls{};

    void reset()
    {
        nextError.clear();
        loggingCalls.clear();
        unmatchedCalls.clear();
    }

    template <typename Callback, typename... Args>
    void async_method_call(Callback&& callback, Args&&... args)
    {
        if constexpr (detail::isRecordedLoggingCall<Args...>)
        {
            recordLoggingCall(std::forward<Callback>(callback),
                              std::forward<Args>(args)...);
            return;
        }

        unmatchedCalls.push_back({"async_method_call", sizeof...(Args)});
        if constexpr (std::is_invocable_v<Callback, boost::system::error_code>)
        {
            std::forward<Callback>(callback)(nextError);
        }
        else
        {
            // Callback expects additional return values (e.g. mapper
            // response); invoke with default-constructed arguments to
            // indicate an empty / no-match result.
            invokeWithDefaults(std::forward<Callback>(callback), nextError);
        }
    }

    template <typename Callback, typename... Args>
    void async_method_call_timed(Callback&& callback, Args&&...)
    {
        unmatchedCalls.push_back({"async_method_call_timed", sizeof...(Args)});
        std::forward<Callback>(callback)(nextError);
    }

  private:
    template <typename Callback, typename Service, typename Path,
              typename Interface, typename Method, typename MessageId,
              typename Severity, typename AddData>
    void recordLoggingCall(Callback&& callback, Service&& service, Path&& path,
                           Interface&& interface, Method&& method,
                           MessageId&& messageId, Severity&& severity,
                           AddData&& addData)
    {
        loggingCalls.push_back(
            {std::string(std::forward<Service>(service)),
             std::string(std::forward<Path>(path)),
             std::string(std::forward<Interface>(interface)),
             std::string(std::forward<Method>(method)),
             std::string(std::forward<MessageId>(messageId)),
             std::string(std::forward<Severity>(severity)),
             std::forward<AddData>(addData)});
        std::forward<Callback>(callback)(nextError);
    }

    template <typename Callback, typename... Supplied>
    static void invokeWithDefaults(Callback&& cb, Supplied&&... supplied)
    {
        invokeWithDefaultsImpl(std::forward<Callback>(cb),
                               std::index_sequence_for<Supplied...>{},
                               std::forward<Supplied>(supplied)...);
    }

    template <typename F>
    struct CallableTraits : CallableTraits<decltype(&F::operator())>
    {};

    template <typename C, typename R, typename... A>
    struct CallableTraits<R (C::*)(A...)>
    {
        using ArgsTuple = std::tuple<std::decay_t<A>...>;
        static constexpr size_t arity = sizeof...(A);
    };

    template <typename C, typename R, typename... A>
    struct CallableTraits<R (C::*)(A...) const>
    {
        using ArgsTuple = std::tuple<std::decay_t<A>...>;
        static constexpr size_t arity = sizeof...(A);
    };

    template <typename Callback, size_t... SuppliedIdx, typename... Supplied>
    static void invokeWithDefaultsImpl(Callback&& cb,
                                       std::index_sequence<SuppliedIdx...>,
                                       Supplied&&... supplied)
    {
        using Traits = CallableTraits<std::decay_t<Callback>>;
        constexpr size_t total = Traits::arity;
        constexpr size_t numSupplied = sizeof...(Supplied);

        if constexpr (total <= numSupplied)
        {
            std::forward<Callback>(cb)(std::forward<Supplied>(supplied)...);
        }
        else
        {
            fillRemaining<Callback, total, numSupplied>(
                std::forward<Callback>(cb),
                std::make_index_sequence<total - numSupplied>{},
                std::forward<Supplied>(supplied)...);
        }
    }

    template <typename Callback, size_t Total, size_t NumSupplied,
              size_t... ExtraIdx, typename... Supplied>
    static void fillRemaining(Callback&& cb, std::index_sequence<ExtraIdx...>,
                              Supplied&&... supplied)
    {
        using Traits = CallableTraits<std::decay_t<Callback>>;
        using ArgsTuple = typename Traits::ArgsTuple;
        std::forward<Callback>(
            cb)(std::forward<Supplied>(supplied)...,
                std::tuple_element_t<NumSupplied + ExtraIdx, ArgsTuple>{}...);
    }
};

class RecordingAsioConnectionProvider
{
  public:
    static auto& getAsioConnection()
    {
        static auto conn = std::make_shared<RecordingAsioConnection>();
        return conn;
    }
};

} // namespace pldm::utils
