#pragma once

#include "scitra/scitra-alveo/dataplane/dataplane.hpp"


class AlveoImp;

/// \brief Connection to one SCION-IP translator pipeline on the Alveo device.
class Alveo : public Dataplane
{
private:
    std::unique_ptr<AlveoImp> imp;

public:
    Alveo();
    ~Alveo() override;

    /// \brief Connect to the device and initialize the dataplane.
    /// \param sysfile Path to device in /sys.
    /// \return If a non-zero error code is returned, device initialization may
    /// be tried again.
    std::error_code initialize(const std::string& sysfile) override;

    /// \brief Release resources and close the sysfile.
    void close() override;

    std::error_code printAllCounters(P4Program prog) override;

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
        std::span<std::uint64_t> params) override;

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
        std::span<std::uint64_t> params) override;

    /// \brief Delete an entry from an exact match table.
    /// \param prog P4 program in which the table is defined.
    /// \param name Table name.
    /// \param keys Match keys. Required number and size of keys depend on the
    /// target table. Keys larger than uint64_t must be split into 64-bit
    /// chunks.
    std::error_code tableDelete(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys) override;

    /// \brief Reset counter to zero.
    std::error_code counterReset(P4Program prog, const char* name) override;

    /// \brief Read a simple counter.
    scion::Maybe<std::uint64_t> counterSimpleRead(
        P4Program prog, const char* name, std::uint32_t index) override;

    /// \brief Write a simple counter.
    std::error_code counterSimpleWrite(
        P4Program prog, const char* name, std::uint32_t index, std::uint64_t value) override;

    /// \brief Read a combined packets and bytes counter.
    scion::Maybe<std::pair<std::uint64_t, std::uint64_t>> counterComboRead(
        P4Program prog, const char* name, std::uint32_t index) override;

    /// \brief Write a combined packets and bytes counter.
    std::error_code counterComboWrite(
        P4Program prog, const char* name, std::uint32_t index,
        std::uint64_t pkts, std::uint64_t bytes) override;
};
