#include "Edit/AnimAssetWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Animation/AnimMontage.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/AnimMontageFactory.h"

namespace
{
    // NextSection takes a section name, or "None" to break the chain the way the details panel does.
    const TCHAR* kNoSection = TEXT("None");

    // An unresolvable section name is the most common spec error, so failures print what was there.
    FString DescribeSections(const UAnimMontage* Montage)
    {
        TArray<FString> Lines;
        for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
        {
            const FCompositeSection& Section = Montage->CompositeSections[Index];
            Lines.Add(FString::Printf(TEXT("[%d] %s @ %.4f -> %s"), Index, *Section.SectionName.ToString(), Section.GetTime(), *Section.NextSectionName.ToString()));
        }

        return FString::Join(Lines, TEXT(", "));
    }

    class FAnimMontageSectionWriter : public IAnimAssetWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Sections");
        }

        virtual bool Apply(FAnimAssetEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Sections must be an array of ops"), *Context.AssetPath);
                return false;
            }

            UAnimMontage* Montage = AnimAssetEdit::RequireMontage(Context, GetSpecKey());
            if (!Montage)
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Sections carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                FString Op;
                if (!Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: section entry has no Op"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Montage, Desc, Op))
                {
                    return false;
                }

                ++Context.Ops;
            }

            SortSections(Montage);
            return true;
        }

    private:
        // Sections are ordered by time everywhere the runtime reads them, and the first one owns t=0.
        // Ops address by name, so this runs once the target is done, never between its ops.
        void SortSections(UAnimMontage* Montage) const
        {
            Montage->CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B)
            {
                return A.GetTime() < B.GetTime();
            });

            UAnimMontageFactory::EnsureStartingSection(Montage);
        }

        bool ApplyOp(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc, const FString& Op)
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Montage, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Montage, Desc);
            }

            if (Op == TEXT("Rename"))
            {
                return ApplyRename(Context, Montage, Desc);
            }

            if (Op == TEXT("Modify"))
            {
                return ApplyModify(Context, Montage, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown section op '%s'. Expected Add, Remove, Rename or Modify"), *Context.AssetPath, *Op);
            return false;
        }

        bool ReadName(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const TCHAR* Field, FString& OutName) const
        {
            if (!Desc->TryGetStringField(Field, OutName) || OutName.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: section op needs %s"), *Context.AssetPath, Field);
                return false;
            }

            return true;
        }

        bool RequireSection(const FAnimAssetEditContext& Context, const UAnimMontage* Montage, const FString& Name) const
        {
            if (Montage->GetSectionIndex(FName(*Name)) != INDEX_NONE)
            {
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no section named '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeSections(Montage));
            return false;
        }

        bool RefuseExistingSection(const FAnimAssetEditContext& Context, const UAnimMontage* Montage, const FString& Name) const
        {
            if (Montage->GetSectionIndex(FName(*Name)) == INDEX_NONE)
            {
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s already carries a section named '%s'"), *Context.AssetPath, *Name);
            return false;
        }

        bool ResolveTime(const FAnimAssetEditContext& Context, double Time) const
        {
            const float PlayLength = Context.AnimAsset->GetPlayLength();
            const bool bInRange = Time >= 0.0 && Time <= PlayLength;
            if (!bInRange)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: time %.4f is outside the montage (0 .. %.4f)"), *Context.AssetPath, Time, PlayLength);
            }

            return bInRange;
        }

        // A NextSection naming a section this spec has not created yet would resolve on the next run
        // and not on this one, so the target is refused instead.
        bool ReadNextSection(const FAnimAssetEditContext& Context, const UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc, FName& OutNext, bool& bOutPresent) const
        {
            FString Text;
            if (!Desc->TryGetStringField(TEXT("NextSection"), Text))
            {
                bOutPresent = false;
                return true;
            }

            bOutPresent = true;
            if (Text.IsEmpty() || Text == kNoSection)
            {
                OutNext = NAME_None;
                return true;
            }

            if (!RequireSection(Context, Montage, Text))
            {
                return false;
            }

            OutNext = FName(*Text);
            return true;
        }

        bool ApplyAdd(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name))
            {
                return false;
            }

            if (!RefuseExistingSection(Context, Montage, Name))
            {
                return false;
            }

            double Time = 0.0;
            if (!Desc->TryGetNumberField(TEXT("Time"), Time))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs Time"), *Context.AssetPath);
                return false;
            }

            if (!ResolveTime(Context, Time))
            {
                return false;
            }

            FName NextSection = NAME_None;
            bool bHasNextSection = false;
            if (!ReadNextSection(Context, Montage, Desc, NextSection, bHasNextSection))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: add section '%s' @ %.4f -> %s"), *Montage->GetName(), *Name, Time, *NextSection.ToString());

            Montage->Modify();

            const int32 NewIndex = Montage->AddAnimCompositeSection(FName(*Name), static_cast<float>(Time));
            if (NewIndex == INDEX_NONE)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s refused section '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            if (bHasNextSection)
            {
                Montage->CompositeSections[NewIndex].NextSectionName = NextSection;
            }

            return true;
        }

        bool ApplyRemove(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !RequireSection(Context, Montage, Name))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove section '%s'"), *Montage->GetName(), *Name);

            Montage->Modify();

            const int32 Index = Montage->GetSectionIndex(FName(*Name));

            // A section still named as somebody's NextSection would leave a chain pointing at nothing.
            const FName Removed = Montage->CompositeSections[Index].SectionName;
            for (FCompositeSection& Section : Montage->CompositeSections)
            {
                if (Section.NextSectionName == Removed)
                {
                    Section.NextSectionName = NAME_None;
                }
            }

            return Montage->DeleteAnimCompositeSection(Index);
        }

        bool ApplyRename(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            FString NewName;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !ReadName(Context, Desc, TEXT("NewName"), NewName))
            {
                return false;
            }

            if (!RequireSection(Context, Montage, Name) || !RefuseExistingSection(Context, Montage, NewName))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: rename section '%s' to '%s'"), *Montage->GetName(), *Name, *NewName);

            Montage->Modify();

            const int32 Index = Montage->GetSectionIndex(FName(*Name));
            const FName Old = Montage->CompositeSections[Index].SectionName;
            const FName New = FName(*NewName);
            Montage->CompositeSections[Index].SectionName = New;

            // The chain stores names, so every link into the renamed section moves with it.
            for (FCompositeSection& Section : Montage->CompositeSections)
            {
                if (Section.NextSectionName == Old)
                {
                    Section.NextSectionName = New;
                }
            }

            return true;
        }

        bool ApplyModify(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Name;
            if (!ReadName(Context, Desc, TEXT("Name"), Name) || !RequireSection(Context, Montage, Name))
            {
                return false;
            }

            const int32 Index = Montage->GetSectionIndex(FName(*Name));
            const bool bHasTime = Desc->HasField(TEXT("Time"));

            double Time = Montage->CompositeSections.IsValidIndex(Index) ? Montage->CompositeSections[Index].GetTime() : 0.0;
            if (bHasTime)
            {
                Desc->TryGetNumberField(TEXT("Time"), Time);
                if (!ResolveTime(Context, Time))
                {
                    return false;
                }
            }

            FName NextSection = NAME_None;
            bool bHasNextSection = false;
            if (!ReadNextSection(Context, Montage, Desc, NextSection, bHasNextSection))
            {
                return false;
            }

            if (!bHasTime && !bHasNextSection)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Modify writes nothing. Expected Time or NextSection"), *Context.AssetPath);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: modify section '%s' @ %.4f"), *Montage->GetName(), *Name, Time);

            Montage->Modify();

            FCompositeSection& Section = Montage->CompositeSections[Index];
            if (bHasTime)
            {
                // SetTime writes the authored value, Link re-resolves which segment now owns it.
                Section.SetTime(static_cast<float>(Time));
                Section.Link(Montage, static_cast<float>(Time));
            }

            if (bHasNextSection)
            {
                Section.NextSectionName = NextSection;
            }

            return true;
        }
    };
}

TUniquePtr<IAnimAssetWriter> MakeAnimMontageSectionWriter()
{
    return MakeUnique<FAnimMontageSectionWriter>();
}
