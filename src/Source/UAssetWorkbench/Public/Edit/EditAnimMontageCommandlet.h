#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditAnimMontageCommandlet.generated.h"

class FJsonObject;
class UAnimMontage;

// Edits an AnimMontage through one writer per facet, the way EditBlueprint splits a Blueprint. One
// target loads the montage once, runs every writer the spec names, then rebuilds the notify cache
// and saves once. A writer that fails aborts the whole target before anything is written, so a
// montage never lands half-edited.
//
// Usage:
//   UnrealEditor-Cmd.exe Project.uproject -run=EditAnimMontage -spec="C:/path/spec.json" [-apply]
//   Without -apply: dry run, reports only. With -apply: save each modified montage.
//
// Spec:
//   {
//     "Targets": [
//       {
//         "AssetPath": "/Game/Path/AM_Foo",
//         "Notifies": [
//           { "Op": "Add", "Class": "/Script/Engine.AnimNotify_PlaySound", "TriggerTime": 1.52,
//             "Track": 10,
//             "Parameters": { "Sound": "/Game/Path/S_Bar.S_Bar", "bFollow": "True",
//                             "AttachName": "root" } },
//           { "Op": "Add", "Class": "/Game/Path/ANS_Telegraph", "TriggerTime": 0.4,
//             "Duration": 0.447 },
//           { "Op": "Modify", "Name": "PlaySound", "At": 0.106,
//             "Parameters": { "AttachName": "Socket_NeckHead" } },
//           { "Op": "Remove", "Name": "PlaySound", "At": 0.106 }
//         ]
//       }
//     ]
//   }
//
// Notifies ops: Add, Modify, Remove.
//
// Add takes Class, an object path or a bare class name, resolved as an AnimNotify or an
// AnimNotifyState. A Blueprint notify may be named by its asset path, the generated class is found
// from it. Class may be omitted for a named notify, the kind an AnimBP answers with AnimNotify_<Name>,
// in which case Name carries it. TriggerTime is required and must land inside the montage. Duration
// is required for a state notify and rejected on any other. Track defaults to 0, and a track the
// montage has not declared yet is appended so the notify shows up on the panel where the spec put it.
//
// Name on Add is the stored NotifyName, defaulting to the class name minus its AnimNotify_ /
// AnimNotifyState_ prefix, which is what the notify panel writes on creation. It is not
// GetNotifyName(), which some notifies override to display a parameter instead.
//
// Modify and Remove address one existing notify. Index is the position AnimMontageExport printed.
// Otherwise Name addresses it, narrowed by At (trigger time, matched to the millisecond) and Track.
// An address that matches no notify, or more than one, is an error, so a re-run cannot quietly hit a
// different notify than the last one did.
//
// Modify writes NewName, TriggerTime, Duration, Track and Parameters, at least one of them. Removing
// a notify is Remove, not a Modify that empties it.
//
// Parameters go onto the notify object by property path, in the same format AnimMontageExport prints
// them, so an exported notify can be fed back as a spec. A named notify carries no object, so a
// Parameters block on one is an error.
//
// Exit codes: 0 success, 1 bad args / unresolvable address / rejected property, 2 live editor.
UCLASS()
class UEditAnimMontageCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditAnimMontageCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false once any writer fails, leaving the montage unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, bool bApply, TSet<UAnimMontage*>& OutTouched, int32& OutOps) const;
};
