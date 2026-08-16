#pragma once

#include <memory>
#include <string>
#include <system_error>


enum class DriverError : int
{
    Ok = 0,
    AlreadyOpen,
    SysfileAccess,
    TargetInitFailed,
};

const std::error_category& driver_error_category();
std::error_code make_error_code(DriverError code);

namespace std {
template <> struct is_error_code_enum<DriverError> : true_type {};
}

class DataplaneImp;

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

    /// @brief Release resources and close the sysfile.
    void close();
};
