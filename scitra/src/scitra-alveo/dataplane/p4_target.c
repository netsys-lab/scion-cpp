#include "scitra/scitra-alveo/dataplane/p4_target.h"

#include <stdio.h>
#include <string.h>


XilVitisNetP4ReturnType env_log(XilVitisNetP4EnvIf* EnvIfPtr, const char* MessagePtr);
void print_version(XilVitisNetP4TargetCtx* CtxPtr);

XilVitisNetP4ReturnType init_target(
    struct P4Target* target, struct Device* device,
    XilVitisNetP4AddressType base_addr,
    XilVitisNetP4TargetConfig *config)
{
    XilVitisNetP4ReturnType result;
    memset(target, 0, sizeof(struct P4Target));
    target->device = device;
    target->base_addr = base_addr;
    target->config = config;

    XilVitisNetP4EnvIf env;
    env.LogError = &env_log;
    env.LogInfo = &env_log;
    env.WordRead32 = &env_read32;
    env.WordWrite32 = &env_write32;
    env.UserCtx = target;

    result = XilVitisNetP4TargetInit(&target->context, &env, config);
    if (result == XIL_VITIS_NET_P4_TARGET_ERR_INCOMPATIBLE_SW_HW)
    {
        printf("Found IP and SW version differences:\n\r");
        print_version(&target->context);
        return result;
    }
    else if (result != XIL_VITIS_NET_P4_SUCCESS)
    {
        printf("Error %d (%s)\n", result, XilVitisNetP4ReturnTypeToString(result));
        return result;
    }

    for (uint32_t i = 0; i < config->CounterListSize; ++i)
    {
        target->counters = calloc(config->CounterListSize, sizeof(target->counters[0]));
        result = XilVitisNetP4CounterInit(&target->counters[i], &env,
            &config->CounterListPtr[i]->Config);
        if (result != XIL_VITIS_NET_P4_SUCCESS)
        {
            printf("Error initializing counter %s: %d (%s)\n",
                config->CounterListPtr[i]->NameStringPtr, result,
                XilVitisNetP4ReturnTypeToString(result));
            return result;
        }
    }
}

XilVitisNetP4ReturnType exit_target(struct P4Target* target)
{
    if (target->context.PrivateCtxPtr)
    {
        XilVitisNetP4ReturnType result;
        result = XilVitisNetP4TargetExit(&target->context);
        if (result != XIL_VITIS_NET_P4_SUCCESS)
        {
            printf("Error %d (%s)\n", result, XilVitisNetP4ReturnTypeToString(result));
        }
    }
    if (target->counters)
    {
        for (uint32_t i = 0; i < target->config->CounterListSize; ++i)
            XilVitisNetP4CounterExit(&target->counters[i]);
        free(target->counters);
    }
    memset(target, 0, sizeof(struct P4Target));
}

XilVitisNetP4ReturnType env_read32(
    XilVitisNetP4EnvIf* EnvIfPtr, XilVitisNetP4AddressType Address, uint32_t* ReadValuePtr)
{
    if (EnvIfPtr == NULL || ReadValuePtr == NULL)
    {
        return XIL_VITIS_NET_P4_GENERAL_ERR_NULL_PARAM;
    }
    else if (EnvIfPtr->UserCtx == NULL)
    {
        return XIL_VITIS_NET_P4_GENERAL_ERR_INTERNAL_ASSERTION;
    }

    struct P4Target* target = (struct P4Target*)EnvIfPtr->UserCtx;

    *ReadValuePtr = device_read32(target->device, target->base_addr + Address);
    return XIL_VITIS_NET_P4_SUCCESS;
}

XilVitisNetP4ReturnType env_write32(
    XilVitisNetP4EnvIf* EnvIfPtr, XilVitisNetP4AddressType Address, uint32_t WriteValue)
{
    if (EnvIfPtr == NULL)
    {
        return XIL_VITIS_NET_P4_GENERAL_ERR_NULL_PARAM;
    }
    else if (EnvIfPtr->UserCtx == NULL)
    {
        return XIL_VITIS_NET_P4_GENERAL_ERR_INTERNAL_ASSERTION;
    }

    struct P4Target* target = (struct P4Target*)EnvIfPtr->UserCtx;

    device_write32(target->device, target->base_addr + Address, WriteValue);
    return XIL_VITIS_NET_P4_SUCCESS;
}

XilVitisNetP4ReturnType env_log(XilVitisNetP4EnvIf* EnvIfPtr, const char* MessagePtr)
{
    if (EnvIfPtr == NULL)
    {
        return XIL_VITIS_NET_P4_GENERAL_ERR_NULL_PARAM;
    }
    if (MessagePtr == NULL)
    {
        return XIL_VITIS_NET_P4_GENERAL_ERR_NULL_PARAM;
    }
    fprintf(stderr, "%s", MessagePtr);
    return XIL_VITIS_NET_P4_SUCCESS;
}

void print_version(XilVitisNetP4TargetCtx* CtxPtr)
{
    XilVitisNetP4ReturnType Result;
    XilVitisNetP4Version SwVersion;
    XilVitisNetP4Version IpVersion;
    XilVitisNetP4TargetBuildInfoCtx *BuildInfoCtxPtr;

    Result =  XilVitisNetP4TargetGetSwVersion(CtxPtr, &SwVersion);
    if (Result != XIL_VITIS_NET_P4_SUCCESS)
    {
        puts("XilVitisNetP4TargetGetSwVersion failed");
        return;
    }

    /* The BuildInfo Driver provides access to the IP Version if present */
    Result = XilVitisNetP4TargetGetBuildInfoDrv(CtxPtr, &BuildInfoCtxPtr);
    if (Result != XIL_VITIS_NET_P4_SUCCESS)
    {
        puts("XilVitisNetP4TargetGetBuildInfoDrv failed");
        return;
    }

    Result = XilVitisNetP4TargetBuildInfoGetIpVersion(BuildInfoCtxPtr, &IpVersion);
    if (Result != XIL_VITIS_NET_P4_SUCCESS)
    {
        puts("XilVitisNetP4TargetBuildInfoGetIpVersion failed");
        return;
    }

    printf("----VitisNetP4Runtime Software Version\n");
    printf("\t\t Major = %d\n", SwVersion.Major);
    printf("\t\t Minor = %d\n", SwVersion.Minor);
    printf("\n");

    printf("----VitisNetP4IP Version\n");
    printf("\t\t Major = %d\n", IpVersion.Major);
    printf("\t\t Minor = %d\n", IpVersion.Minor);
}
