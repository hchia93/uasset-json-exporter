#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditMaterialCommandlet.generated.h"

// Audits Material and MaterialInstance against fixed rules N1-N9 (Nanite compatibility) and U1-U4 (usage
// flag versus actual application). Every material gets a Cost and Permutation block, -stats adds compiled
// shader numbers. The report carries a Spec block EditMaterialAsset consumes unchanged.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditMaterial [-assets=] [-scandir=] [-report=] [-stats] [-rules=]
// See Docs/Audit.md.
UCLASS()
class UAuditMaterialCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditMaterialCommandlet();

    virtual int32 Main(const FString& Params) override;
};
