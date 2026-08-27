#pragma once

#include "scion/error_codes.hpp"

#include <memory>
#include <span>
#include <string>
#include <system_error>


enum class DriverError : int
{
    Ok = 0,
    AlreadyOpen,
    SysfileAccess,
    NotInitialized,
    TargetInitFailed,
    NotFound,
    TooFewArguments,
    NotImplemented,
    InternalError,
};

const std::error_category& driver_error_category();
std::error_code make_error_code(DriverError code);

namespace std {
template <> struct is_error_code_enum<DriverError> : true_type {};
}

const std::error_category& vitis_net_error_category();

class DataplaneImp;

enum P4Program
{
    P4_PROG_IG_CLASSIFIER = 0,
    P4_PROG_IG_TRANSLATOR = 1,
    P4_PROG_EG_TRANSLATOR = 2,
};

/// \brief  Connection to one SCION-IP translator pipeline on the Alveo device.
class Dataplane
{
private:
    std::unique_ptr<DataplaneImp> imp;

public:
    Dataplane();
    ~Dataplane();

    /// \brief Connect to the device and initialize the dataplane.
    /// \param sysfile Path to device in /sys.
    /// \return If a non-zero error code is returned, device initialization may
    /// be tried again.
    std::error_code initialize(const std::string& sysfile);

    /// \brief Release resources and close the sysfile.
    void close();

    std::error_code printAllCounters(P4Program prog);

    /// \brief Insert an entry into an exact match table. Does not support
    /// tables with ternary, range, or lpm matches.
    /// \param prog P4 program in which the table is defined.
    /// \param name Table name.
    /// \param keys Match keys. Required number and size of keys depend on the
    /// target table. Keys larger than uint64_t must be split into 64-bit
    /// chunks.
    /// \param action Action to execute.
    /// \param key Action parameters. Required number and size of parameters
    /// depend on the action. Parameters larger than uint64_t must be split into
    /// 64-bit chunks.
    std::error_code tableInsert(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys,
        const char* action,
        std::span<std::uint64_t> params);

    /// \brief Update an entry in an exact match table.
    /// \param prog P4 program in which the table is defined.
    /// \param name Table name.
    /// \param keys Match keys. Required number and size of keys depend on the
    /// target table. Keys larger than uint64_t must be split into 64-bit
    /// chunks.
    /// \param action Action to execute.
    /// \param key Action parameters. Required number and size of parameters
    /// depend on the action. Parameters larger than uint64_t must be split into
    /// 64-bit chunks.
    std::error_code tableUpdate(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys,
        const char* action,
        std::span<std::uint64_t> params);

    /// \brief Delete an entry from an exact match table.
    /// \param prog P4 program in which the table is defined.
    /// \param name Table name.
    /// \param keys Match keys. Required number and size of keys depend on the
    /// target table. Keys larger than uint64_t must be split into 64-bit
    /// chunks.
    std::error_code tableDelete(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys);

    /// \brief Reset counter to zero.
    std::error_code counterReset(P4Program prog, const char* name);

    /// \brief Read a simple counter.
    scion::Maybe<std::uint64_t> counterSimpleRead(
        P4Program prog, const char* name, std::uint32_t index);

    /// \brief Write a simple counter.
    std::error_code counterSimpleWrite(
        P4Program prog, const char* name, std::uint32_t index, std::uint64_t value);

    /// \brief Read a combined packets and bytes counter.
    scion::Maybe<std::pair<std::uint64_t, std::uint64_t>> counterComboRead(
        P4Program prog, const char* name, std::uint32_t index);

    /// \brief Write a combined packets and bytes counter.
    std::error_code counterComboWrite(
        P4Program prog, const char* name, std::uint32_t index,
        std::uint64_t pkts, std::uint64_t bytes);
};
