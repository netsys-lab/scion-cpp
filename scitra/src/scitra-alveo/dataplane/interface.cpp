#include "scitra/scitra-alveo/dataplane/interface.hpp"
#include "scion/bit_stream.hpp"

extern "C" {
#include "scitra/scitra-alveo/dataplane/device.h"
#include "scitra/scitra-alveo/dataplane/p4_target.h"
#include "vitis_net_p4/vitis_net_p4_0_defs.h"
#include "vitis_net_p4/vitis_net_p4_1_defs.h"
#include "vitis_net_p4/vitis_net_p4_2_defs.h"
#include "vitis_net_p4/vitisnetp4_common.h"
}

#include <spdlog/spdlog.h>

#include <cstring>
#include <charconv>
#include <vector>

using scion::Maybe;
using scion::Error;

static constexpr size_t TARGET_COUNT = 3;

const XilVitisNetP4AddressType BASE_ADDR_IG_CLASSIFIER = 0x300000;
const XilVitisNetP4AddressType BASE_ADDR_IG_TRANSLATOR = 0x400000;
const XilVitisNetP4AddressType BASE_ADDR_EG_TRANSLATOR = 0x500000;

static Maybe<std::vector<std::byte>> formatKey(
    const XilVitisNetP4TableConfig& cfg,
    std::span<uint64_t> keys);
static Maybe<std::vector<std::byte>> formatActionParams(
    const XilVitisNetP4Action& action,
    std::span<uint64_t> params);

////////////
// Errors //
////////////

struct DriverErrorCategory : public std::error_category
{
    const char* name() const noexcept override
    {
        return "driver";
    }

    std::string message(int code) const override
    {
        switch (static_cast<DriverError>(code)) {
            case DriverError::Ok:
                return "ok";
            case DriverError::AlreadyOpen:
                return "device already open";
            case DriverError::SysfileAccess:
                return "sysfile access error";
            case DriverError::NotInitialized:
                return "not initialized";
            case DriverError::TargetInitFailed:
                return "target IP initialization failed";
            case DriverError::NotFound:
                return "named entity not found";
            case DriverError::TooFewArguments:
                return "too few arguments";
            case DriverError::NotImplemented:
                return "not implemented";
            case DriverError::InternalError:
                return "internal error";
            default:
                return "unexpected error code";
        }
    }
};

static DriverErrorCategory driverErrorCategory;

const std::error_category& driver_error_category()
{
    return driverErrorCategory;
}

std::error_code make_error_code(DriverError code)
{
    return {static_cast<int>(code), driverErrorCategory};
}

struct VitisNetErrorCategory : public std::error_category
{
    const char* name() const noexcept override
    {
        return "Xilinx_Vitis_Net_P4";
    }

    std::string message(int code) const override
    {
        return XilVitisNetP4ReturnTypeToString(static_cast<XilVitisNetP4ReturnType>(code));
    }
};

static VitisNetErrorCategory vitisNetErrorCategory;

const std::error_category& vitis_net_error_category()
{
    return vitisNetErrorCategory;
}

std::error_code make_error_code(XilVitisNetP4ReturnType code)
{
    return {static_cast<int>(code), vitisNetErrorCategory};
}

namespace std {
template <> struct is_error_code_enum<XilVitisNetP4ReturnType> : true_type {};
}

//////////////////
// DataplaneImp //
//////////////////

class DataplaneImp
{
private:
    bool m_open = false;
    std::string m_sysfile;

    P4Target m_targets[TARGET_COUNT];
    Device m_device;

public:
    ~DataplaneImp()
    {
        close();
    }

    std::error_code initialize(const std::string& sysfile) noexcept
    {
        if (m_open) return DriverError::AlreadyOpen;
        std::memset(&m_targets, 0, sizeof(m_targets));
        std::memset(&m_targets, 0, sizeof(m_device));

        XilVitisNetP4ReturnType result;

        spdlog::info("Open target device {}", m_sysfile);
        if (device_open(&m_device, m_sysfile.c_str())) {
            return DriverError::SysfileAccess;
        }
        sleep(1); // ?

        spdlog::info("Enable CMAC port 0");
        device_write32(&m_device, 0x8014, 0x1);
        device_write32(&m_device, 0x800c, 0x1);
        printf("Read 0x8294: 0x%08x\n", device_read32(&m_device, 0x8204));
        printf("Read 0x8294: 0x%08x\n", device_read32(&m_device, 0x8204));
        sleep(1); // ?

        spdlog::info("Initialize driver");
        spdlog::info("Ingress Classifier");
        result = init_target(&m_targets[P4_PROG_IG_CLASSIFIER], &m_device,
            BASE_ADDR_IG_CLASSIFIER, &XilVitisNetP4TargetConfig_vitis_net_p4_0);
        if (result) {
            device_close(&m_device);
            return DriverError::TargetInitFailed;
        }
        spdlog::info("Ingress Translator");
        result = init_target(&m_targets[P4_PROG_IG_TRANSLATOR], &m_device,
            BASE_ADDR_IG_TRANSLATOR, &XilVitisNetP4TargetConfig_vitis_net_p4_1);
        if (result) {
            exit_target(&m_targets[0]);
            device_close(&m_device);
            return DriverError::TargetInitFailed;
        }
        spdlog::info("Egress Translator");
        result = init_target(&m_targets[P4_PROG_EG_TRANSLATOR], &m_device,
            BASE_ADDR_EG_TRANSLATOR, &XilVitisNetP4TargetConfig_vitis_net_p4_2);
        if (result) {
            exit_target(&m_targets[0]);
            exit_target(&m_targets[1]);
            device_close(&m_device);
            return DriverError::TargetInitFailed;
        }

        m_targets[P4_PROG_IG_CLASSIFIER].prog_name = "Ingress Classifier";
        m_targets[P4_PROG_IG_TRANSLATOR].prog_name = "Ingress Translator";
        m_targets[P4_PROG_EG_TRANSLATOR].prog_name = "Egress Translator";

        m_open = true;
        return DriverError::Ok;
    }

    void close() noexcept
    {
        if (m_open) {
            for (size_t i = 0; i < TARGET_COUNT; ++i)
                exit_target(&m_targets[i]);
            device_close(&m_device);
        }
    }

    std::error_code printAllCounters(P4Program prog)
    {
        if (!m_open) return DriverError::NotInitialized;

        XilVitisNetP4ReturnType result;
        auto target = &m_targets[prog];
        printf("=== %s ===\n", target->prog_name);
        if (target->counters == NULL) return DriverError::InternalError;
        for (uint32_t i = 0; i < target->config->CounterListSize; ++i)
        {
            printf("%s =", target->config->CounterListPtr[i]->NameStringPtr);
            uint32_t n = target->config->CounterListPtr[i]->Config.NumCounters;
            auto values = (uint64_t*)calloc(n, sizeof(uint64_t));
            result = XilVitisNetP4CounterCollectRead(&target->counters[i], 0, n, values);
            if (result == XIL_VITIS_NET_P4_SUCCESS)
            {
                for (uint32_t j = 0; j < n; ++j)
                    printf(" %lu", values[j]);
                putchar('\n');
            }
            else
                puts(" error");
            free(values);
        }
        return DriverError::Ok;
    }

    std::error_code tableInsert(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys,
        const char* action,
        std::span<std::uint64_t> params)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto tab = get_table_by_name(&m_targets[prog], name);
        if (!tab) return DriverError::NotFound;

        uint32_t actionId = 0;
        auto res = XilVitisNetP4TableGetActionId(tab, const_cast<char*>(action), &actionId);
        if (res) return res;

        const auto& cfg = m_targets[prog].config->TableListPtr[0]->Config;
        auto key = formatKey(cfg, keys);
        if (!key) return key.error();

        auto response = formatActionParams(*cfg.ActionListPtr[actionId], params);
        if (!response) return response.error();

        return XilVitisNetP4TableInsert(tab,
            (uint8_t*)key->data(), NULL, 0, actionId, (uint8_t*)response->data());
    }

    std::error_code tableUpdate(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys,
        const char* action,
        std::span<std::uint64_t> params)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto tab = get_table_by_name(&m_targets[prog], name);
        if (!tab) return DriverError::NotFound;

        uint32_t actionId = 0;
        auto res = XilVitisNetP4TableGetActionId(tab, const_cast<char*>(action), &actionId);
        if (res) return res;

        const auto& cfg = m_targets[prog].config->TableListPtr[0]->Config;
        auto key = formatKey(cfg, keys);
        if (!key) return key.error();

        auto response = formatActionParams(*cfg.ActionListPtr[actionId], params);
        if (!response) return response.error();

        return XilVitisNetP4TableUpdate(tab,
            (uint8_t*)key->data(), NULL, actionId, (uint8_t*)response->data());
    }

    std::error_code tableDelete(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto tab = get_table_by_name(&m_targets[prog], name);
        if (!tab) return DriverError::NotFound;

        const auto& cfg = m_targets[prog].config->TableListPtr[0]->Config;
        auto key = formatKey(cfg, keys);
        if (!key) return key.error();

        return XilVitisNetP4TableDelete(tab, (uint8_t*)key->data(), NULL);
    }

    std::error_code counterReset(P4Program prog, const char* name)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto ctr = get_counter_by_name(&m_targets[prog], name);
        if (!ctr) return DriverError::NotFound;
        return XilVitisNetP4CounterReset(ctr);
    }

    Maybe<uint64_t> counterSimpleRead(P4Program prog, const char* name, uint32_t index)
    {
        if (!m_open) return Error(DriverError::NotInitialized);
        auto ctr = get_counter_by_name(&m_targets[prog], name);
        if (!ctr) return Error(DriverError::NotFound);
        uint64_t value = 0;
        if (auto ret = XilVitisNetP4CounterSimpleRead(ctr, index, &value); ret)
            return Error(ret);
        return value;
    }

    std::error_code counterSimpleWrite(
        P4Program prog, const char* name, uint32_t index, uint64_t value)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto ctr = get_counter_by_name(&m_targets[prog], name);
        if (!ctr) return DriverError::NotFound;
        return XilVitisNetP4CounterSimpleWrite(ctr, index, value);
    }

    Maybe<std::pair<uint64_t, uint64_t>> counterComboRead(
        P4Program prog, const char* name, uint32_t index)
    {
        if (!m_open) return Error(DriverError::NotInitialized);
        auto ctr = get_counter_by_name(&m_targets[prog], name);
        if (!ctr) return Error(DriverError::NotFound);
        uint64_t pkts = 0, bytes = 0;
        if (auto ret = XilVitisNetP4CounterComboRead(ctr, index, &pkts, &bytes); ret)
            return Error(ret);
        return std::make_pair(pkts, bytes);
    }

    std::error_code counterComboWrite(
        P4Program prog, const char* name, uint32_t index, uint64_t pkts, uint64_t bytes)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto ctr = get_counter_by_name(&m_targets[prog], name);
        if (!ctr) return DriverError::NotFound;
        return XilVitisNetP4CounterComboWrite(ctr, index, pkts, bytes);
    }
};

// Combine keys into a single big-endian byte string according to the tables
// key specification. For key fields larger than 64 bit, multiple keys are
// consumed.
static Maybe<std::vector<std::byte>> formatKey(
    const XilVitisNetP4TableConfig& cfg,
    std::span<uint64_t> keys)
{
    assert(cfg.CamConfig.Endian == XIL_VITIS_NET_P4_BIG_ENDIAN);
    const uint32_t keyBytes = (cfg.KeySizeBits + 7) / 8;

    std::vector<std::byte> key(keyBytes);
    scion::WriteStream keyStream(key);
    if (uint32_t paddingBits = -((uint32_t)cfg.KeySizeBits) % 8; paddingBits) {
        keyStream.advanceBits(paddingBits, scion::NullStreamError);
    }

    uint32_t k = 0;
    for (char* p = cfg.CamConfig.FormatStringPtr; *p;) {
        unsigned int width;
        char type;
        int n;
        if (std::sscanf("%u%c%n", p, &width, &type, &n) != 3)
            return Error(DriverError::InternalError);
        if (type != 'c')
            return Error(DriverError::NotImplemented);
        for (auto w = (int)width; w > 0; w -= 64) {
            if (k >= keys.size())
                return Error(DriverError::TooFewArguments);
            if (!keyStream.serializeBits(keys[k++], std::min(w, 64), scion::NullStreamError))
                return Error(DriverError::InternalError);
        }
        p += n;
        if (*p == ':') ++p;
    }
    auto [bytes, bits] = keyStream.getPos();
    assert(bytes == keyBytes && bits == 0);
    return key;
}

// Combine action parameters into a single big-endian byte string according to
// hte action specification. For parameters larger than 64 bit, multiple keys
// are consumed.
static Maybe<std::vector<std::byte>> formatActionParams(
    const XilVitisNetP4Action& action,
    std::span<uint64_t> params)
{
    uint32_t paramBits = 0;
    for (uint32_t i = 0; i < action.ParamListSize; ++i) {
        paramBits += action.ParamListPtr[i].Value;
    }
    const uint32_t paramBytes = (paramBits + 7) / 8;

    std::vector<std::byte> param(paramBytes);
    scion::WriteStream paramStream(param);
    if (uint32_t paddingBits = -((uint32_t)paramBits) % 8; paddingBits) {
        paramStream.advanceBits(paddingBits, scion::NullStreamError);
    }

    uint32_t p = 0;
    for (uint32_t i = 0; i < action.ParamListSize; ++i) {
        auto width = action.ParamListPtr[i].Value;
        for (auto w = (int)width; w > 0; w -= 64) {
            if (p >= params.size())
                return Error(DriverError::TooFewArguments);
            if (!paramStream.serializeBits(params[p++], std::min(w, 64), scion::NullStreamError))
                return Error(DriverError::InternalError);
        }
    }
    auto [bytes, bits] = paramStream.getPos();
    assert(bytes == paramBytes && bits == 0);
    return param;
}

///////////////
// Dataplane //
///////////////

Dataplane::Dataplane()
    : imp(std::make_unique<DataplaneImp>())
{}

Dataplane::~Dataplane() = default;

std::error_code Dataplane::initialize(const std::string& sysfile)
{
    return imp->initialize(sysfile);
}

void Dataplane::close()
{
    return imp->close();
}

std::error_code Dataplane::printAllCounters(P4Program prog)
{
    return imp->printAllCounters(prog);
}

std::error_code Dataplane::tableInsert(
    P4Program prog, const char* name,
    std::span<std::uint64_t> keys,
    const char* action,
    std::span<std::uint64_t> params)
{
    return imp->tableInsert(prog, name, keys, action, params);
}

std::error_code Dataplane::tableUpdate(
    P4Program prog, const char* name,
    std::span<std::uint64_t> keys,
    const char* action,
    std::span<std::uint64_t> params)
{
    return imp->tableUpdate(prog, name, keys, action, params);
}

std::error_code Dataplane::tableDelete(
    P4Program prog, const char* name,
    std::span<std::uint64_t> keys)
{
    return imp->tableDelete(prog, name, keys);
}

std::error_code Dataplane::counterReset(P4Program prog, const char* name)
{
    return imp->counterReset(prog, name);
}

Maybe<uint64_t> Dataplane::counterSimpleRead(
    P4Program prog, const char* name, uint32_t index)
{
    return imp->counterSimpleRead(prog, name, index);
}

std::error_code Dataplane::counterSimpleWrite(
    P4Program prog, const char* name, uint32_t index, uint64_t value)
{
    return imp->counterSimpleWrite(prog, name, index, value);
}

Maybe<std::pair<uint64_t, uint64_t>> Dataplane::counterComboRead(
    P4Program prog, const char* name, uint32_t index)
{
    return imp->counterComboRead(prog, name, index);
}

std::error_code Dataplane::counterComboWrite(
    P4Program prog, const char* name, uint32_t index, uint64_t pkts, uint64_t bytes)
{
    return imp->counterComboWrite(prog, name, index, pkts, bytes);
}
