using UnrealBuildTool;
using System.IO;

public class QuickerBridge : ModuleRules
{
    public QuickerBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;

        PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Editor/ContentBrowser/Private"));

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "Slate", "SlateCore" });

        PrivateDependencyModuleNames.AddRange(new string[] { 
            "CoreUObject", 
            "Engine", 
            "InputCore", 
            "ApplicationCore", 
            "DeveloperSettings",
            "UnrealEd",
            "EditorSubsystem", 
            "HTTP",         
            "HTTPServer",
            "Json",
            "PythonScriptPlugin",
            "BlueprintGraph",
            "Kismet",
            "GraphEditor", 
            "LevelEditor",
            "ContentBrowser",
            "ContentBrowserData",
            "AssetRegistry",
            "AssetDefinition",
            "EditorWidgets",
            "ToolWidgets",
            "EditorFramework",             // 提供 EditorModeSubsystem 权限
            "ModelingToolsEditorMode",     // 提供建模模式专属权限
            "InteractiveToolsFramework"
        });
    }
}
