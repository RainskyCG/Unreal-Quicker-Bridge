#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// 必须加上 QUICKERBRIDGE_API，否则项目会一直报 missing 错误
class QUICKERBRIDGE_API FQuickerBridgeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};