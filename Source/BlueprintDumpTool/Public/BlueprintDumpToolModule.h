// Copyright (c) 2026 knight-scripts. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlueprintDumpToolModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	IConsoleObject* DumpAnimBPCommand = nullptr;
	IConsoleObject* DumpBPCommand = nullptr;
	IConsoleObject* DumpBPFolderCommand = nullptr;
};
