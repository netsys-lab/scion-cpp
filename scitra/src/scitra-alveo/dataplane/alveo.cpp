#include <unistd.h>
#include "scitra/scitra-alveo/dataplane/dataplane.hpp"
#include "scitra/scitra-alveo/dataplane/alveo.hpp"
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

const XilVitisNetP4AddressType BASE_ADDR_IG_CLASSIFIER = 0x180000;
const XilVitisNetP4AddressType BASE_ADDR_IG_TRANSLATOR = 0x1C0000;
const XilVitisNetP4AddressType BASE_ADDR_EG_TRANSLATOR = 0x100000;

static Maybe<std::vector<std::byte>> formatKey(
    const XilVitisNetP4TableConfig& cfg,
    std::span<uint64_t> keys);
static Maybe<std::vector<std::byte>> formatActionParams(
    const XilVitisNetP4Action& action,
    std::span<uint64_t> params);

////////////
// Errors //
////////////

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


//////////////
// AlveoImp //
//////////////

class AlveoImp
{
private:
    bool m_open = false;
    std::string m_sysfile;

    P4Target m_targets[TARGET_COUNT];
    Device m_device;

public:
    ~AlveoImp()
    {
        close();
    }

    std::error_code initialize(const std::string& sysfile) noexcept
    {
        if (m_open) return DriverError::AlreadyOpen;
        m_sysfile = sysfile;
        std::memset(&m_targets, 0, sizeof(m_targets));
        std::memset(&m_targets, 0, sizeof(m_device));

        XilVitisNetP4ReturnType result;

        spdlog::info("Open target device {}", m_sysfile);
        if (device_open(&m_device, m_sysfile.c_str())) {
            return DriverError::SysfileAccess;
        }

        const auto tx = device_read32(&m_device, 0x8200);
        spdlog::info("Link TX status: local_fault={}", tx & 0x1);
        const auto rx = device_read32(&m_device, 0x8204);
        spdlog::info("Link RX status: link_up={} aligned={} hi_ber={} local_fault={} remote_fault={}",
            rx & 0x1, (rx >> 1) & 0x1, (rx >> 4) & 0x1, (rx >> 6) & 0x1, (rx >> 5) & 0x1);

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
        spdlog::debug("printAllCounters: target={} config={} counters={} CounterListSize={}",
            (void*)target, (void*)target->config, (void*)target->counters,
            target->config ? target->config->CounterListSize : 999999);
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

        decltype(&m_targets[prog].config->TableListPtr[0]->Config) cfgPtr = nullptr;
        for (uint32_t ti = 0; ti < m_targets[prog].config->TableListSize; ++ti) {
            if (strcmp(m_targets[prog].config->TableListPtr[ti]->NameStringPtr, name) == 0) {
                cfgPtr = &m_targets[prog].config->TableListPtr[ti]->Config;
                break;
            }
        }
        if (!cfgPtr) return DriverError::NotFound;
        const auto& cfg = *cfgPtr;
        auto key = formatKey(cfg, keys);
        if (!key) return key.error();

        auto response = formatActionParams(*cfg.ActionListPtr[actionId], params);
        if (!response) return response.error();

        spdlog::debug("tableInsert: key.data()={} key.size()={} response.data()={} response.size()={}",
            (void*)key->data(), key->size(), (void*)response->data(), response->size());
        // FIXED: added retry-with-reset for the known intermittent CAM
        // busy/INTERNAL_ASSERTION race. XilVitisNetP4TableReset operates
        // on the table's own TableCtx, regardless of underlying CAM mode.
        XilVitisNetP4ReturnType insertResult;
        for (int attempt = 0; attempt < 5; ++attempt) {
            insertResult = XilVitisNetP4TableInsert(tab,
                (uint8_t*)key->data(), NULL, 0, actionId, (uint8_t*)response->data());
            if (insertResult != XIL_VITIS_NET_P4_GENERAL_ERR_INTERNAL_ASSERTION)
                break;
            if (attempt < 4) {
                XilVitisNetP4TableReset(tab);
                usleep(50000);
            }
        }
        return insertResult;
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

        decltype(&m_targets[prog].config->TableListPtr[0]->Config) cfgPtr = nullptr;
        for (uint32_t ti = 0; ti < m_targets[prog].config->TableListSize; ++ti) {
            if (strcmp(m_targets[prog].config->TableListPtr[ti]->NameStringPtr, name) == 0) {
                cfgPtr = &m_targets[prog].config->TableListPtr[ti]->Config;
                break;
            }
        }
        if (!cfgPtr) return DriverError::NotFound;
        const auto& cfg = *cfgPtr;
        auto key = formatKey(cfg, keys);
        if (!key) return key.error();

        auto response = formatActionParams(*cfg.ActionListPtr[actionId], params);
        if (!response) return response.error();

        XilVitisNetP4ReturnType updateResult;
        for (int attempt = 0; attempt < 5; ++attempt) {
            updateResult = XilVitisNetP4TableUpdate(tab,
                (uint8_t*)key->data(), NULL, actionId, (uint8_t*)response->data());
            if (updateResult != XIL_VITIS_NET_P4_GENERAL_ERR_INTERNAL_ASSERTION)
                break;
            if (attempt < 4) {
                XilVitisNetP4TableReset(tab);
                usleep(50000);
            }
        }
        return updateResult;
    }

    std::error_code tableDelete(
        P4Program prog, const char* name,
        std::span<std::uint64_t> keys)
    {
        if (!m_open) return DriverError::NotInitialized;
        auto tab = get_table_by_name(&m_targets[prog], name);
        if (!tab) return DriverError::NotFound;

        decltype(&m_targets[prog].config->TableListPtr[0]->Config) cfgPtr = nullptr;
        for (uint32_t ti = 0; ti < m_targets[prog].config->TableListSize; ++ti) {
            if (strcmp(m_targets[prog].config->TableListPtr[ti]->NameStringPtr, name) == 0) {
                cfgPtr = &m_targets[prog].config->TableListPtr[ti]->Config;
                break;
            }
        }
        if (!cfgPtr) return DriverError::NotFound;
        const auto& cfg = *cfgPtr;
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
        if (std::sscanf(p, "%u%c%n", &width, &type, &n) != 2) {
            spdlog::debug("formatKey: sscanf failed on p='{}'", p);
            return Error(DriverError::InternalError);
        }
        if (type != 'c')
            return Error(DriverError::NotImplemented);
        for (auto w = (int)width; w > 0; w -= 64) {
            if (k >= keys.size())
                return Error(DriverError::TooFewArguments);
            if (!keyStream.serializeBits(keys[k++], std::min(w, 64), scion::NullStreamError)) {
                spdlog::debug("formatKey: serializeBits failed, w={}\n", w);
                return Error(DriverError::InternalError);
            }
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

    std::vector<std::byte> param(std::max(paramBytes, 1u));
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
            if (!paramStream.serializeBits(params[p++], std::min(w, 64), scion::NullStreamError)) {
                spdlog::debug("formatActionParams: serializeBits failed, w={}, paramBits total computed above", w);
                return Error(DriverError::InternalError);
            }
        }
    }
    auto [bytes, bits] = paramStream.getPos();
    assert(bytes == paramBytes && bits == 0);
    return param;
}

/////////////////////
// Alveo Dataplane //
/////////////////////

Alveo::Alveo()
    : imp(std::make_unique<AlveoImp>())
{}

Alveo::~Alveo() = default;

std::error_code Alveo::initialize(const std::string& sysfile)
{
    return imp->initialize(sysfile);
}

void Alveo::close()
{
    return imp->close();
}

std::error_code Alveo::printAllCounters(P4Program prog)
{
    return imp->printAllCounters(prog);
}

std::error_code Alveo::tableInsert(
    P4Program prog, const char* name,
    std::span<std::uint64_t> keys,
    const char* action,
    std::span<std::uint64_t> params)
{
    return imp->tableInsert(prog, name, keys, action, params);
}

std::error_code Alveo::tableUpdate(
    P4Program prog, const char* name,
    std::span<std::uint64_t> keys,
    const char* action,
    std::span<std::uint64_t> params)
{
    return imp->tableUpdate(prog, name, keys, action, params);
}

std::error_code Alveo::tableDelete(
    P4Program prog, const char* name,
    std::span<std::uint64_t> keys)
{
    return imp->tableDelete(prog, name, keys);
}

std::error_code Alveo::counterReset(P4Program prog, const char* name)
{
    return imp->counterReset(prog, name);
}

Maybe<uint64_t> Alveo::counterSimpleRead(
    P4Program prog, const char* name, uint32_t index)
{
    return imp->counterSimpleRead(prog, name, index);
}

std::error_code Alveo::counterSimpleWrite(
    P4Program prog, const char* name, uint32_t index, uint64_t value)
{
    return imp->counterSimpleWrite(prog, name, index, value);
}

Maybe<std::pair<uint64_t, uint64_t>> Alveo::counterComboRead(
    P4Program prog, const char* name, uint32_t index)
{
    return imp->counterComboRead(prog, name, index);
}

std::error_code Alveo::counterComboWrite(
    P4Program prog, const char* name, uint32_t index, uint64_t pkts, uint64_t bytes)
{
    return imp->counterComboWrite(prog, name, index, pkts, bytes);
}
