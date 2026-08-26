#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "DataTableExportCommandlet.generated.h"

// Exports DataTable rows and their struct properties to JSON.
//   UnrealEditor-Cmd.exe Project.uproject -run=DataTableExport -assets="/Game/Path/DT_A,/Game/Path/DT_B"
// Contract: Docs/Export.md
UCLASS()
class UDataTableExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UDataTableExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportDataTable(class UDataTable* DataTable) const;
    TSharedPtr<FJsonObject> ExportRow(const UScriptStruct* RowStruct, const void* RowData) const;

};
