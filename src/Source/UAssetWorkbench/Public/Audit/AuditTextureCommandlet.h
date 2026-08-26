#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditTextureCommandlet.generated.h"

// Audits Texture2D settings against fixed rules T1-T15: sampler agreement, group, sRGB, built size,
// power of two, streaming. Registry tags screen first, only candidates get loaded. The report carries a
// Spec block EditTextureAsset consumes unchanged.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditTexture [-assets=] [-scandir=] [-report=] [-rules=]
// Contract: Docs/Audit.md
UCLASS()
class UAuditTextureCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditTextureCommandlet();

    virtual int32 Main(const FString& Params) override;
};
