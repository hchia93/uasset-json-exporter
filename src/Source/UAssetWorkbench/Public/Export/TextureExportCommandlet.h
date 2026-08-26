#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "TextureExportCommandlet.generated.h"

// Exports Texture asset properties to JSON via UObject reflection.
//   UnrealEditor-Cmd.exe Project.uproject -run=TextureExport -assets="/Game/Path/T_A,/Game/Path/T_B"
// Contract: Docs/Export.md
UCLASS()
class UTextureExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UTextureExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportTexture(class UTexture* Texture) const;

};
