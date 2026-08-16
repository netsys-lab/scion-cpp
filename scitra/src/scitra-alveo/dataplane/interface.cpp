#include "scitra/scitra-alveo/dataplane/interface.hpp"

extern "C" {
#include "scitra/scitra-alveo/dataplane/device.h"
#include "scitra/scitra-alveo/dataplane/p4_target.h"
#include "vitis_net_p4/vitis_net_p4_0_defs.h"
#include "vitis_net_p4/vitis_net_p4_1_defs.h"
#include "vitis_net_p4/vitis_net_p4_2_defs.h"
#include "vitis_net_p4/vitis_net_p4_3_defs.h"
#include "vitis_net_p4/vitisnetp4_common.h"
}

#include <spdlog/spdlog.h>

#include <cstring>


static constexpr size_t TARGET_COUNT = 3;
static constexpr size_t TARGET_IG_CLASSIFIER = 0;
static constexpr size_t TARGET_IG_TRANSLATOR = 1;
static constexpr size_t TARGET_EG_TRANSLATOR = 2;

const XilVitisNetP4AddressType BASE_ADDR_IG_CLASSIFIER = 0x100000;
const XilVitisNetP4AddressType BASE_ADDR_IG_TRANSLATOR = 0x200000;
const XilVitisNetP4AddressType BASE_ADDR_EG_TRANSLATOR = 0x300000;

static void enable_port0(struct Device* dev);
static void print_counters(P4Target* target);


////////////
// Errors //
////////////

struct DriverErrorCategory : public std::error_category
{
    const char* name() const noexcept override
    {
        return "scitra";
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
            case DriverError::TargetInitFailed:
                return "target IP initialization failed";
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

        spdlog::info("Open target device %s", m_sysfile);
        if (device_open(&m_device, m_sysfile.c_str())) {
            return DriverError::SysfileAccess;
        }
        sleep(1); // ?

        spdlog::info("Enable CMAC port 0");
        enable_port0(&m_device);

        spdlog::info("Initialize driver");
        spdlog::info("Ingress Classifier");
        result = init_target(&m_targets[TARGET_IG_CLASSIFIER], &m_device,
            BASE_ADDR_IG_CLASSIFIER, &XilVitisNetP4TargetConfig_vitis_net_p4_0);
        if (result) {
            device_close(&m_device);
            return DriverError::TargetInitFailed;
        }
        spdlog::info("Ingress Translator");
        result = init_target(&m_targets[TARGET_IG_TRANSLATOR], &m_device,
            BASE_ADDR_IG_TRANSLATOR, &XilVitisNetP4TargetConfig_vitis_net_p4_1);
        if (result) {
            exit_target(&m_targets[0]);
            device_close(&m_device);
            return DriverError::TargetInitFailed;
        }
        spdlog::info("Egress Translator");
        result = init_target(&m_targets[TARGET_EG_TRANSLATOR], &m_device,
            BASE_ADDR_EG_TRANSLATOR, &XilVitisNetP4TargetConfig_vitis_net_p4_2);
        if (result) {
            exit_target(&m_targets[0]);
            exit_target(&m_targets[1]);
            device_close(&m_device);
            return DriverError::TargetInitFailed;
        }

        m_targets[TARGET_IG_CLASSIFIER].prog_name = "Ingress Classifier";
        m_targets[TARGET_IG_TRANSLATOR].prog_name = "Ingress Translator";
        m_targets[TARGET_EG_TRANSLATOR].prog_name = "Egress Translator";

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
};

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

//////////
// CMAC //
//////////

static void enable_port0(struct Device* dev)
{
    device_write32(dev, 0x8014, 0x1);
    device_write32(dev, 0x800c, 0x1);
    // why two reads?
    printf("Read 0x8294: 0x%08x\n", device_read32(dev, 0x8204));
    printf("Read 0x8294: 0x%08x\n", device_read32(dev, 0x8204));
    sleep(1); // ?
};

//////////////
// Counters //
//////////////

static void print_counters(P4Target* target)
{
    XilVitisNetP4ReturnType result;
    printf("=== %s ===\n", target->prog_name);
    if (target->counters == NULL) return;
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
}
