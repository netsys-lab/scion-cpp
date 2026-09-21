#pragma once

#include "scitra/scitra-alveo/dataplane/dataplane.hpp"

#include <spdlog/spdlog.h>


class MockDp : public Dataplane
{
    std::error_code initialize(const std::string& sysfile) override
    {
        spdlog::debug("Initialize device {}", sysfile);
    }

    void close() override
    {
        spdlog::debug("Close dataplane connection");
    }

    std::error_code printAllCounters(P4Program prog) override
    {
        printf("=== Counters ===\n");
    }

    std::error_code tableInsert(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys,
        const char* action,
        std::span<std::uint64_t> params) override
    {
        spdlog::debug("tableInsert: name={} keys={} action={} params={}",
            name, keys, action, params);
        return DriverError::Ok;
    }

    std::error_code tableUpdate(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys,
        const char* action,
        std::span<std::uint64_t> params) override
    {
        spdlog::debug("tableUpdate: name={} keys={} action={} params={}",
            name, keys, action, params);
        return DriverError::Ok;
    }

    std::error_code tableDelete(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys) override
    {
        spdlog::debug("tableUpdate: name={} keys={}", name, keys);
        return DriverError::Ok;
    }

    std::error_code counterReset(P4Program prog, const char* name) override
    {
        spdlog::debug("counterReset: name={}", name);
        return DriverError::Ok;
    }

    scion::Maybe<std::uint64_t> counterSimpleRead(
        P4Program prog, const char* name, std::uint32_t index) override
    {
        spdlog::debug("counterSimpleRead: name={} index={}", name, index);
        return 0;
    }

    std::error_code counterSimpleWrite(
        P4Program prog, const char* name, std::uint32_t index, std::uint64_t value) override
    {
        spdlog::debug("counterSimpleWrite: name={} index={} value={}", name, index, value);
        return DriverError::Ok;
    }

    scion::Maybe<std::pair<std::uint64_t, std::uint64_t>> counterComboRead(
        P4Program prog, const char* name, std::uint32_t index) override
    {
        spdlog::debug("counterComboRead: name={} index={} value={}", name, index);
        return std::make_pair(0, 0);
    }

    std::error_code counterComboWrite(
        P4Program prog, const char* name, std::uint32_t index,
        std::uint64_t pkts, std::uint64_t bytes) override
    {
        spdlog::debug("counterComboWrite: name={} index={} pkts={} bytes={}",
            name, index, pkts, bytes);
        return DriverError::Ok;
    }
};
