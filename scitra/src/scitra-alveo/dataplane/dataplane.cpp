#include "scitra/scitra-alveo/dataplane/dataplane.hpp"


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
