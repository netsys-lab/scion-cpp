#pragma once

#include "scitra/scitra-alveo/dataplane/device.h"
#include "vitis_net_p4/vitisnetp4_common.h"
#include "vitis_net_p4/vitisnetp4_target.h"


struct P4Target
{
    const char* prog_name;
    struct Device* device;
    XilVitisNetP4AddressType base_addr;
    XilVitisNetP4TargetConfig* config;
    XilVitisNetP4TargetCtx context;
    XilVitisNetP4TableCtx* tables;
    XilVitisNetP4CounterCtx* counters;
    // FIXED: env must live as long as the target itself. Both
    // XilVitisNetP4TargetInit and XilVitisNetP4CounterInit store the raw
    // EnvIf pointer they're given (confirmed via source:
    // counter_extern.c's CtxPtr->EnvIfPtr = EnvIfPtr, no copy) -- a local
    // variable in init_target() would dangle once that function returns,
    // causing crashes (calling a garbage function pointer) whenever
    // counter/table functions are called later.
    XilVitisNetP4EnvIf env;
};

XilVitisNetP4ReturnType init_target(
    struct P4Target* target, struct Device* device,
    XilVitisNetP4AddressType base_addr,
    XilVitisNetP4TargetConfig* config);

XilVitisNetP4ReturnType exit_target(struct P4Target *target);

XilVitisNetP4TableCtx* get_table_by_name(struct P4Target *target, const char* name);
XilVitisNetP4CounterCtx* get_counter_by_name(struct P4Target *target, const char* name);

XilVitisNetP4ReturnType env_read32(
    XilVitisNetP4EnvIf* EnvIfPtr, XilVitisNetP4AddressType Address, uint32_t* ReadValuePtr);
XilVitisNetP4ReturnType env_write32(
    XilVitisNetP4EnvIf* EnvIfPtr, XilVitisNetP4AddressType Address, uint32_t WriteValue);
