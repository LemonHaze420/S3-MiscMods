#include <Windows.h>
#include <iostream>
#include <tchar.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <ostream>
#include <locale>
#include <codecvt>
#include <filesystem>
#include <Shlobj.h>
#include <sstream>

// Shenmue III SDK
#include "SDK.h"

#include "CharacterIDs.h"

using namespace SDK;

// inih
#include "ini.h"
#include "INIReader.h"

// minhook
#include "MinHook.h"

#ifndef _RELEASE_MODE
#define MOD_NAME					"MiscMods - Debug Mode"
#else
#define MOD_NAME					"MiscMods"
#endif
#define MOD_VER						"1.15"
#define MOD_STRING					MOD_NAME " " MOD_VER

//#undef _RELEASE_MODE
//#define PROCESS_EVENT_LOGGER

static bool g_ThreadAlive = true;

extern bool show_overlay = true;

bool receiveTickHooked = false;

// Retro Camera Mod
static bool bRetroCameraModEnabled = true;
static bool bHookActive = true;
static bool bApplyCustomCameraValues = false;
static float customFOV = 55.f, customCamDistance = 455.f, customCamHeight = 25.f, customCameraPitchOffset = -5.0f;   // custom values

// NPC Fixes
static bool npcFixesEnabled = true;
static int maxLoadedNPCs = 256;
static int maxVisibleNPCs = 256;
static float customScale = 30000.f;
static float customDistance = 50000.f;

// No Drain Energy While Running patch
static bool bNoDrainRunEnergy = true;

static bool bNoWalkOnlyTriggers = true;

// Any Time Fishing patch
static bool bAnyTimeFishing = false;

static US3GameInstance* gameInstance = nullptr;
static ES3PlayerBehavior playerBehavior;

static bool bDumpHerbs = false;
std::map<std::string, std::string> Locations;

static bool bRyoForShenhua = false;

static bool toggleReplacements = false;

static bool verbose = true;

std::string find_name_from_chara_enum(SwapCharacter ID) {
	if (swapCharacterNameMap.find(ID) != swapCharacterNameMap.end()) {
		return swapCharacterNameMap.find(ID)->second;
	}
}

SwapCharacter find_chara_enum_from_name(std::string name)
{
	for (auto& map : swapCharacterNameMap) {
		if(map.second == name)
			return map.first;
	}
}

class SwapCharacter_t {
public:
	SwapCharacter_t() {}
	SwapCharacter_t(SwapCharacter _original, SwapCharacter _replacement) : original_chara(_original), replacement_chara(_replacement)
	{
		original	= find_name_from_chara_enum(original_chara);
		replacement = find_name_from_chara_enum(replacement_chara);
	}
	SwapCharacter_t(std::string _original, std::string _replacement) : original(_original), replacement(_replacement)	{}
	~SwapCharacter_t() = default;

	SwapCharacter original_chara;
	SwapCharacter replacement_chara;

	std::string original;
	std::string replacement;

	bool should_swap = true;
	bool has_swapped = false;

	bool is_selected = false;
};
static std::vector<SwapCharacter_t> replacements;

void read_char_replacements(std::string filename)
{
	if (!std::filesystem::exists(filename)) {
		(verbose ? printf("[Error] CSV does not exist.\n") : 0);
		return;
	}

	std::ifstream file(filename);
	if (file.good()) {
		(verbose ? printf("Loaded '%s'\n", filename.c_str()) : 0);
		std::string field, line, replacement;

		while (!file.eof()) {
			// parse csv
			std::getline(file, line);
			std::stringstream ssline(line);

			std::string toFind, toReplace;
			ssline >> std::quoted(toFind);
			std::getline(ssline, field, ',');
			ssline >> std::quoted(toReplace);
			std::getline(ssline, replacement);

			// create a swap character
			SwapCharacter_t tmp;
			tmp.original			= toFind;
			tmp.replacement			= toReplace;

			tmp.original_chara		= find_chara_enum_from_name(toFind);
			tmp.replacement_chara	= find_chara_enum_from_name(toReplace);
			replacements.push_back(tmp);

			if (replacements.size() % 16)
				replacements.shrink_to_fit();

			(verbose ? printf("Replacing '%s' with '%s'\n", find_name_from_chara_enum(tmp.original_chara).c_str(), find_name_from_chara_enum(tmp.replacement_chara).c_str()) : 0);
			(verbose ? printf("Added replacement %d ['%s' ---> '%s']\n", (int)replacements.size(), toFind.c_str(), toReplace.c_str()) : 0);	
		}
		file.close();
	}
}


bool HookUEFunc(std::string fnString, void* hookFn, void* origFn) {
	UFunction* fnPtr = UObject::FindObject<UFunction>(fnString.c_str());
	if (fnPtr && fnPtr->Func != 0) {
		MH_Initialize();
		MH_STATUS mhStatus = MH_CreateHook(reinterpret_cast<void**>(fnPtr->Func), hookFn, reinterpret_cast<void**>(&origFn));
		mhStatus = MH_EnableHook(reinterpret_cast<void*>(fnPtr->Func));
		if (mhStatus == MH_OK) 
			return true;
	}
	return false;
}

bool IsNameDefault(std::string name) {
	return name.find("Default") != std::string::npos;
}
bool IsInPersistentLevel(std::string name) {
	return name.find("PersistentLevel") != std::string::npos;
}

void LoadLocations() {
	std::ifstream file("herb_locations.txt");
	if (file.is_open()) {
		std::string line;
		while (std::getline(file, line)) {
			int sep = line.find('|');
			if (sep == std::string::npos)
				continue;
			auto name = line.substr(0, sep);
			auto pos = line.substr(sep + 1);
			Locations.insert(std::make_pair(pos, name));
		}
		file.close();
	}
}

void SaveLocations() {
	std::ofstream file("herb_locations.txt");
	file.clear();
	for (auto const& x : Locations)
		file << x.second << "|" << x.first << std::endl;
	file.close();
}

bool AddLocation(std::string name, std::string pos) {
	if (Locations.find(pos) == Locations.end()) {
		Locations.insert(std::make_pair(pos, name));
		//SaveLocations();
		return true;
	}
	return false;
}
bool IsZeroVector(const FVector& vec) {
	return vec.X == 0 && vec.Y == 0 && vec.Z == 0;
}

#define OLD_WAY 

void* tmpPtr = nullptr;

void LoadAllLevels() {
#ifdef OLD_WAY
	for (auto level : UObject::FindObjects<ALevelStreamingVolume>()) {
		if (level) {
			level->SetActorScale3D({ 100000.f, 100000.f, 100000.f });
		}
	}
#else
	if (!tmpPtr) {
		auto tmp = UObject::FindObject<ULevelStreaming>();
		if (tmp) {
			tmpPtr = &tmp;
		}
	}
#endif
}

inline ABP_S3_Character_Adventure_C* GetRyo() {
	for (auto ryo : UObject::FindObjects<ABP_S3_Character_Adventure_C>()) {
		if (!IsNameDefault(ryo->GetFullName()))
			return ryo;
	}
	return nullptr;
}

#ifdef PROCESS_EVENT_LOGGER
std::ofstream ofs;
#endif

namespace SDK {
	void customPECall(UObject* _this, UFunction* pFunction, void* pParms) {
#ifndef _RELEASE_MODE
		//if ((strstr(pFunction->GetFullName().c_str(), "Play") || (strstr(pFunction->GetFullName().c_str(), "Start")))
		//	&& (!strstr(pFunction->GetFullName().c_str(), "ReceiveTick") && (!strstr(pFunction->GetFullName().c_str(), "CutsceneCharacter"))))
		//	printf("%s\n", pFunction->GetFullName().c_str());
		
		/*if (pFunction->GetFullName().find("ReceiveTick"))
			return;

		if (pFunction->GetFullName().find("Cutscene")  != std::string::npos || _this->GetFullName().find("CutScene") != std::string::npos || _this->GetFullName().find("Cutscene") != std::string::npos || _this->GetFullName().find("Sequence") != std::string::npos) {
			printf("_this = %s\n", _this->GetFullName().c_str());
			printf("_this->Class = %s\n", _this->Class->GetFullName().c_str());
			
			printf("pFunction = %s\n", pFunction->GetFullName().c_str());
			printf("pFunction->Class = %s\n", pFunction->Class->GetFullName().c_str());
		}*/

#endif
#ifdef PROCESS_EVENT_LOGGER
		std::time_t result = std::time(nullptr);
		std::string timeStamp = std::asctime(std::localtime(&result));
		timeStamp.pop_back();
		ofs << "[" << timeStamp << "]" << "[" << _this->GetFullName() << "|" << _this->Class->GetFullName() << "] " << pFunction->GetFullName() << std::endl;
#endif
	}
};

// ReceiveTick hook for AActor's
typedef void(__thiscall* ReceiveTick_t)(class UObject* _this, __int64* a2, float* DeltaSeconds);
ReceiveTick_t origReceiveTick;
ReceiveTick_t origNPCReceiveTick;

// StaticLoadObject signature
typedef class UObject* (__fastcall* loadObject_t)(UClass* Class, UObject* InOuter, const TCHAR* Name, const TCHAR* Filename, uint32_t LoadFlags, UPackageMap* Sandbox, bool bAllowObjectReconciliation);
loadObject_t StaticLoadObject_orig;


struct FObjectInstancingGraph {
	class		UObject* SourceRoot;
	class		UObject* DestinationRoot;
	bool										bCreatingArchetype;
	bool										bEnableSubobjectInstancing;
	bool										bLoadingObject;
	TMap<class UObject*, class UObject*>			SourceToDestinationMap;
	TMap<UObject*, UObject*> ReplaceMap;
};

template <typename T>
T* StaticLoadObject(UObject* InOuter, const TCHAR* Name, const TCHAR* Filename, uint32_t LoadFlags, UPackageMap* Sandbox, bool bAllowObjectReconciliation) {
	return reinterpret_cast<T*>(StaticLoadObject_orig(T::StaticClass(), InOuter, Name, Filename, LoadFlags, Sandbox, bAllowObjectReconciliation));
}
class UObject* StaticLoadObject_Hook(UClass* Class, UObject* InOuter, const TCHAR* Name, const TCHAR* Filename, uint32_t LoadFlags, UPackageMap* Sandbox, bool bAllowObjectReconciliation) {
	return StaticLoadObject_orig(Class, InOuter, Name, Filename, LoadFlags, Sandbox, bAllowObjectReconciliation);
}
enum EObjectFlags {
	RF_NoFlags = 0x00000000,	///< No flags, used to avoid a cast
	// This first group of flags mostly has to do with what kind of object it is. Other than transient, these are the persistent object flags.
	// The garbage collector also tends to look at these.
	RF_Public = 0x00000001,	///< Object is visible outside its package.
	RF_Standalone = 0x00000002,	///< Keep object around for editing even if unreferenced.
	RF_MarkAsNative = 0x00000004,	///< Object (UField) will be marked as native on construction (DO NOT USE THIS FLAG in HasAnyFlags() etc)
	RF_Transactional = 0x00000008,	///< Object is transactional.
	RF_ClassDefaultObject = 0x00000010,	///< This object is its class's default object
	RF_ArchetypeObject = 0x00000020,	///< This object is a template for another object - treat like a class default object
	RF_Transient = 0x00000040,	///< Don't save object.

	// This group of flags is primarily concerned with garbage collection.
	RF_MarkAsRootSet = 0x00000080,	///< Object will be marked as root set on construction and not be garbage collected, even if unreferenced (DO NOT USE THIS FLAG in HasAnyFlags() etc)
	RF_TagGarbageTemp = 0x00000100,	///< This is a temp user flag for various utilities that need to use the garbage collector. The garbage collector itself does not interpret it.

	// The group of flags tracks the stages of the lifetime of a uobject
	RF_NeedInitialization = 0x00000200,	///< This object has not completed its initialization process. Cleared when ~FObjectInitializer completes
	RF_NeedLoad = 0x00000400,	///< During load, indicates object needs loading.
	RF_KeepForCooker = 0x00000800,	///< Keep this object during garbage collection because it's still being used by the cooker
	RF_NeedPostLoad = 0x00001000,	///< Object needs to be postloaded.
	RF_NeedPostLoadSubobjects = 0x00002000,	///< During load, indicates that the object still needs to instance subobjects and fixup serialized component references
	RF_NewerVersionExists = 0x00004000,	///< Object has been consigned to oblivion due to its owner package being reloaded, and a newer version currently exists
	RF_BeginDestroyed = 0x00008000,	///< BeginDestroy has been called on the object.
	RF_FinishDestroyed = 0x00010000,	///< FinishDestroy has been called on the object.

	// Misc. Flags
	RF_BeingRegenerated = 0x00020000,	///< Flagged on UObjects that are used to create UClasses (e.g. Blueprints) while they are regenerating their UClass on load (See FLinkerLoad::CreateExport())
	RF_DefaultSubObject = 0x00040000,	///< Flagged on subobjects that are defaults
	RF_WasLoaded = 0x00080000,	///< Flagged on UObjects that were loaded
	RF_TextExportTransient = 0x00100000,	///< Do not export object to text form (e.g. copy/paste). Generally used for sub-objects that can be regenerated from data in their parent object.
	RF_LoadCompleted = 0x00200000,	///< Object has been completely serialized by linkerload at least once. DO NOT USE THIS FLAG, It should be replaced with RF_WasLoaded.
	RF_InheritableComponentTemplate = 0x00400000, ///< Archetype of the object can be in its super class
	RF_DuplicateTransient = 0x00800000, ///< Object should not be included in any type of duplication (copy/paste, binary duplication, etc.)
	RF_StrongRefOnFrame = 0x01000000,	///< References to this object from persistent function frame are handled as strong ones.
	RF_NonPIEDuplicateTransient = 0x02000000,  ///< Object should not be included for duplication unless it's being duplicated for a PIE session
	RF_Dynamic = 0x04000000, // Field Only. Dynamic field - doesn't get constructed during static initialization, can be constructed multiple times
	RF_WillBeLoaded = 0x08000000, // This object was constructed during load and will be loaded shortly
};

/** Objects flags for internal use (GC, low level UObject code) */
enum class EInternalObjectFlags : int32_t
{
	None = 0,
	ReachableInCluster = 1 << 23, ///< External reference to object in cluster exists
	ClusterRoot = 1 << 24, ///< Root of a cluster
	Native = 1 << 25, ///< Native (UClass only). 
	Async = 1 << 26, ///< Object exists only on a different thread than the game thread.
	AsyncLoading = 1 << 27, ///< Object is being asynchronously loaded.
	Unreachable = 1 << 28, ///< Object is not reachable on the object graph.
	PendingKill = 1 << 29, ///< Objects that are pending destruction (invalid for gameplay but valid objects)
	RootSet = 1 << 30, ///< Object will not be garbage collected, even if unreferenced.
	HadReferenceKilled = 1 << 31, ///< Object had a reference null'd out by markpendingkill
	GarbageCollectionKeepFlags = Native | Async | AsyncLoading,
	AllFlags = ReachableInCluster | ClusterRoot | Native | Async | AsyncLoading | Unreachable | PendingKill | RootSet | HadReferenceKilled
};

// StaticLoadObject signature
typedef class UObject* (__fastcall* newObject_t)(class UClass*, class UObject*, FName, enum  EObjectFlags, enum  EInternalObjectFlags, class UObject*, bool, struct FObjectInstancingGraph*, bool);
newObject_t NewObject_orig;

template <typename T>
T* StaticNewObject(UObject* InOuter, FName Name, EObjectFlags SetFlags, EInternalObjectFlags InternalSetFlags, UObject* Template, bool bCopyTransientsFromClassDefaults, struct FObjectInstancingGraph* InstanceGraph, bool bAssumeTemplateIsArchetype) {
	return reinterpret_cast<T*>(NewObject_orig(T::StaticClass(), InOuter, Name, SetFlags, InternalSetFlags, Template, bCopyTransientsFromClassDefaults, InstanceGraph, bAssumeTemplateIsArchetype));
}

class UObject* StaticNewObject_hook(UClass* Class, UObject* InOuter, FName Name, EObjectFlags SetFlags, EInternalObjectFlags InternalSetFlags, UObject* Template, bool bCopyTransientsFromClassDefaults, struct FObjectInstancingGraph* InstanceGraph, bool bAssumeTemplateIsArchetype) {
	return NewObject_orig(Class, InOuter, Name, SetFlags, InternalSetFlags, Template, bCopyTransientsFromClassDefaults, InstanceGraph, bAssumeTemplateIsArchetype);
}

struct SkelMeshMod {
	std::string origPath;
	std::string replacementPath;
};
std::vector<SkelMeshMod> skelMeshMods;

// Should Swap Ryo?
static bool hasSwappedRyo = false;
static bool shouldSwapRyo = false;
static USkeletalMesh* swapMeshRyo = nullptr;

// Should Swap Shenhua?
static bool hasSwappedShenhua = false;
static bool shouldSwapShenhua = false;
static USkeletalMesh* swapMeshShenhua = nullptr;

static bool save = false;
static bool forceCanSkipDialog = true, forceSkipIntro = true;

bool multMoneyReceived = false, hookedMoneyFunc = false;

bool drainQOL = false;
typedef void(__fastcall* moneyFunc_t)(__int64** _this, __int64* a2);
moneyFunc_t SetMoneyOrig;

UWorld* theWorld = nullptr;
UEngine* theEngine = nullptr;

void SetMoneyHook(__int64** _this, __int64* a2) {

	SetMoneyOrig(_this, a2);

	// @todo: money
}

void ReceiveTickHook(class UObject* _this, __int64* a2, float* DeltaSeconds) {
	if (_this->IsA(ABP_S3BgmArea_C::StaticClass())) 
	{
		if (auto bgmArea = reinterpret_cast<ABP_S3BgmArea_C*> (_this))
		{
			printf("Area Name = %s\n", bgmArea->GetName().c_str());
			printf("Is In Area? = %d\n", bgmArea->bIsInArea);
		}
	}


	// Any Time Fishing mod
	if (bAnyTimeFishing && _this->IsA(ABP_MiniGame_FishingManager_C::StaticClass())) {
		ABP_MiniGame_FishingManager_C * fishMan = reinterpret_cast<ABP_MiniGame_FishingManager_C*> (_this);
		if (fishMan) {
			fishMan->limitTime = 23.75f;
			fishMan->LimitMaxTime = 23.75f;
		}
	}
	// Force Can Skip Dialog
	if (forceCanSkipDialog) {
		if (_this->IsA(ABP_TalkEventManager_C::StaticClass())) {
			ABP_TalkEventManager_C* talkMan = reinterpret_cast<ABP_TalkEventManager_C*> (_this);
			if (talkMan && !talkMan->CanSkip)
				talkMan->CanSkip = true; 
		}
		else if (_this->IsA(UTalkEventTaskBase::StaticClass())) {
			UTalkEventTaskBase* talkMan = reinterpret_cast<UTalkEventTaskBase*> (_this);
			if (talkMan && !talkMan->isCanSkipTask)
				talkMan->isCanSkipTask = true;
		}
	}
	// NPC fix and state following
	if (npcFixesEnabled) {
		if (_this->IsA(ABP_NPCManager_C::StaticClass())) {
			ABP_NPCManager_C* npcMan = reinterpret_cast<ABP_NPCManager_C*> (_this);
			if (npcMan) {
				if (npcMan->MaxDistanceFromPlayer != customDistance || npcMan->MaxLoadedNPC != maxLoadedNPCs) {
					npcMan->bForceInstantLoad		= true;
					npcMan->MaxLoadedNPC			= maxLoadedNPCs;
					npcMan->MaxDistanceFromPlayer	= customDistance;
				}
				if (npcMan->StreamingComponent) {
					if (npcMan->StreamingComponent->LoadTriggers.Num() > 0)
						for (int i = 0; i < npcMan->StreamingComponent->LoadTriggers.Num(); ++i)
							if (npcMan->StreamingComponent->LoadTriggers.IsValidIndex(i) && npcMan->StreamingComponent->LoadTriggers[i]) {
								if (npcMan->StreamingComponent->LoadTriggers[i]->GetActorScale3D().X != customScale) {
									npcMan->StreamingComponent->LoadTriggers[i]->SetActorScale3D({ customScale, customScale, customScale });
								}
							}
				}
			}
		}
	}
	else {
		if (_this->IsA(ABP_NPCManager_C::StaticClass())) {
			ABP_NPCManager_C* npcMan = reinterpret_cast<ABP_NPCManager_C*> (_this);
			if (npcMan) {
				if( npcMan->MaxLoadedNPC != 32 || npcMan->MaxDistanceFromPlayer != 2500.0f)
					npcMan->MaxLoadedNPC =  32;   npcMan->MaxDistanceFromPlayer =  2500.0f;
			}
		}
	}
	if (_this->IsA(US3GameInstance::StaticClass()) && !IsNameDefault(_this->GetFullName())) {
		US3GameInstance* _gameInstance = reinterpret_cast<US3GameInstance*>(_this);
		if (_gameInstance) {
			playerBehavior = _gameInstance->PlayerBehavior;
			gameInstance = _gameInstance;

			if (npcFixesEnabled && gameInstance->GraphicSettingsManager) {
				gameInstance->GraphicSettingsManager->MaxLoadedNPC = maxLoadedNPCs;
				gameInstance->GraphicSettingsManager->MaxShowNPC   = maxVisibleNPCs;
			}
			else if (!npcFixesEnabled && gameInstance->GraphicSettingsManager) {
				gameInstance->GraphicSettingsManager->MaxLoadedNPC = 32;
				gameInstance->GraphicSettingsManager->MaxShowNPC   = 32;
			}
		}
	}
	// No Drain Run Energy patch
	else if (bNoDrainRunEnergy && _this->IsA(ABP_S3EnergyManager_C::StaticClass()) && !IsNameDefault(_this->GetFullName())) {
		ABP_S3EnergyManager_C* energyManager = reinterpret_cast<ABP_S3EnergyManager_C*>(_this);
		if (energyManager)
			return;
	}
	// QoL patch for stamina
	else if (!bNoDrainRunEnergy && _this->IsA(ABP_S3EnergyManager_C::StaticClass()) && !IsNameDefault(_this->GetFullName())) {
		ABP_S3EnergyManager_C* energyManager = reinterpret_cast<ABP_S3EnergyManager_C*>(_this);

		if (drainQOL) {
			if (energyManager->NormalRunningDrainRate == 0.000667f) {
				energyManager->NormalRunningDrainRate = 0.0000667f;
			}
		} else {
			if (energyManager->NormalRunningDrainRate == 0.0000667f) {
				energyManager->NormalRunningDrainRate = 0.000667f;
			}
		}
	}
	if (gameInstance && !hookedMoneyFunc) {
		UFunction* fnPtr = UObject::FindObject<UFunction>("Function Shenmue3.S3GameInstance.SetHaveMoney");
		if (fnPtr && fnPtr->Func != 0) {
			MH_STATUS mhStatus1 = MH_CreateHook(reinterpret_cast<void**>(fnPtr->Func), SetMoneyHook, reinterpret_cast<void**>(&SetMoneyOrig));
			mhStatus1 = MH_EnableHook(reinterpret_cast<void*>(fnPtr->Func));
			if (mhStatus1 == MH_OK) {
				hookedMoneyFunc = true;
			}
		}
	}
	// Remove Walk Only Triggers
	else if (bNoWalkOnlyTriggers && _this->IsA(AS3WalkOnlyTrigger::StaticClass()) && !IsNameDefault(_this->GetFullName())) {
		AS3WalkOnlyTrigger* walkOnlyTrigger = reinterpret_cast<AS3WalkOnlyTrigger*>(_this);
		if (walkOnlyTrigger && walkOnlyTrigger->GetEnable()) {
			walkOnlyTrigger->SetEnable(false);
		}
	}
	// Herbs dumper
	else if (bDumpHerbs && _this->IsA(ABP_S3ItemSpawner_C::StaticClass()) && !IsNameDefault(_this->GetFullName())) {
		ABP_S3ItemSpawner_C* itemSpawner = reinterpret_cast<ABP_S3ItemSpawner_C*>(_this);
		if (itemSpawner) {
			auto pos = itemSpawner->K2_GetActorLocation();
			std::string itemIdAsName = "ITEM-" + std::to_string(itemSpawner->ItemId);
			std::string uniqueName(itemSpawner->GetUniqueName().GetName());

			if (pos.X <= 6200000.0f || !IsZeroVector(pos) || uniqueName.compare("None") != 0 || itemIdAsName.compare("None") != 0) {
				auto pos_string = std::to_string(pos.X) + "," + std::to_string(pos.Y) + "," + std::to_string(pos.Z);
				save = AddLocation(uniqueName + "|" + itemIdAsName, pos_string) || save;
			}

			if (save) SaveLocations();
		}
	}
	else {
		// Swap Character (for Ryo)
		// Ryo is the only ABP_S3_Character_Adventure_C instance.
		if (_this->IsA(ABP_S3_Character_Adventure_C::StaticClass())) {
			ABP_S3_Character_Adventure_C* This = reinterpret_cast<ABP_S3_Character_Adventure_C*> (_this);		
			if (shouldSwapRyo && !hasSwappedRyo) {
				if (swapCharacterPathMap.find(swapCharacterRyo) != swapCharacterPathMap.end()) {
					printf("Swapping Ryo for \'%ws\'\n", swapCharacterPathMap.at(swapCharacterRyo));

					auto temp = StaticLoadObject<USkeletalMesh>(nullptr, swapCharacterPathMap.at(swapCharacterRyo), 0, 0, nullptr, true);
					if (temp) {
						swapMeshRyo = temp;

						This->Mesh->SetSkeletalMesh(temp, true); 
						
						for (int i = 0; i < temp->Materials.Num(); ++i) {
							This->Mesh->SetMaterial(i, temp->Materials[i].MaterialInterface);
						}

						hasSwappedRyo = true;
					} else {
						printf("Couldn't load \'%ws\'\n", swapCharacterPathMap.at(swapCharacterRyo));
						hasSwappedRyo = false;
					}
				}
			}
		}

		// Swap Character (for Shenhua)
		// Shenhua is a regular S3 Character, so we gotta search for her.
		if (_this->IsA(ABP_S3Character_C::StaticClass())) {
			ABP_S3Character_C* This = reinterpret_cast<ABP_S3Character_C*> (_this);
			if (shouldSwapShenhua && !hasSwappedShenhua) {
				if (strstr(This->Name.GetName(), "SHE") && !strstr(This->Name.GetName(), "RYO"))
				{
					if (swapCharacterPathMap.find(swapCharacterShenhua) != swapCharacterPathMap.end()) {
						printf("Swapping Shenhua for \'%ws\'\n", swapCharacterPathMap.at(swapCharacterShenhua));

						auto temp = StaticLoadObject<USkeletalMesh>(nullptr, swapCharacterPathMap.at(swapCharacterShenhua), 0, 0, nullptr, true);
						if (temp) {
							swapMeshShenhua = temp;

							This->Mesh->SetSkeletalMesh(temp, true);

							for (int i = 0; i < temp->Materials.Num(); ++i) {
								This->Mesh->SetMaterial(i, temp->Materials[i].MaterialInterface);
							}

							hasSwappedShenhua = true;
						}
						else {
							printf("Couldn't load \'%ws\'\n", swapCharacterPathMap.at(swapCharacterShenhua));
							hasSwappedShenhua = false;
						}
					}
				}
			}

			if (toggleReplacements) 
			{

				/*if (1)
				{
					auto tag = This->GetTagCharaComponent();
					if (tag) {
						auto profile = tag->Profile;
						printf("tag		= %s\n", tag->Name.GetName());
						printf("gender	= %s\n", profile.Gender.GetValue() == ES3CharacterGender::Gender_Female ? "Female" : profile.Gender.GetValue() == ES3CharacterGender::Gender_Male ? "Male" : profile.Gender.GetValue() == ES3CharacterGender::Gender_Unspecified ? "Unspecified" : "UNKNOWN");
						printf("age		= %d\nheight	= %f\nweight	= %d\n", profile.Age, profile.Height, profile.Weight);
						printf("handedness	= %s\n", profile.Handedness.GetValue() == ES3HandednessType::Handedness_Left ? "Left" : "Right");
					}
				}*/

				for (auto& replacement : replacements)
				{
					// can't find either? we skipp
					if ((swapCharacterPathMap.find(replacement.replacement_chara) == swapCharacterPathMap.end() ||
						(swapCharacterPathMap.find(replacement.original_chara) == swapCharacterPathMap.end())))
						continue;

					// is the right character? have we swapped it already??
					bool bIsValid = This->GetTagCharaName().GetName() == find_name_from_chara_enum(replacement.original_chara) &&
									replacement.should_swap && !replacement.has_swapped;
					if (bIsValid)
					{
						// load the replacement
						if (auto temp = StaticLoadObject<USkeletalMesh>(nullptr, swapCharacterPathMap.at(replacement.replacement_chara), 0, 0, nullptr, true)) {
							// swap
							This->Mesh->SetSkeletalMesh(temp, true);

							// re-assign materials
							for (int i = 0; i < temp->Materials.Num(); ++i)
								This->Mesh->SetMaterial(i, temp->Materials[i].MaterialInterface);

							// update
							replacement.has_swapped = true;

							printf("Swapping \'%s\' for \'%s\'\n", replacement.original.c_str(), replacement.replacement.c_str());
						}
						else {
							printf("Couldn't load \'%s\'\n", replacement.replacement.c_str());
							replacement.has_swapped = false;
						}
					}
				}
			}
		}
	}
	origReceiveTick(_this, a2, DeltaSeconds);
}

t_WindowProc OriginalWindowProc = nullptr;

using namespace ImGui;

LRESULT WINAPI DetourWindowProc(
	_In_ HWND hWnd,
	_In_ UINT Msg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam
)
{
	static std::once_flag flag;
	std::call_once(flag, []() { IndiciumEngineLogInfo("++ DetourWindowProc called"); });

	if (show_overlay) {
		ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
		switch (Msg)
		{
		case WM_LBUTTONDOWN:
			GetIO().MouseDown[0] = true; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_LBUTTONUP:
			GetIO().MouseDown[0] = false; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_RBUTTONDOWN:
			GetIO().MouseDown[1] = true; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_RBUTTONUP:
			GetIO().MouseDown[1] = false; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_MBUTTONDOWN:
			GetIO().MouseDown[2] = true; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_MBUTTONUP:
			GetIO().MouseDown[2] = false; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_MOUSEWHEEL:
			GetIO().MouseWheel += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? +1.0f : -1.0f; return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		case WM_MOUSEMOVE:
			GetIO().MousePos.x = (signed short)(lParam); GetIO().MousePos.y = (signed short)(lParam >> 16); return ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
			break;
		}
	}
	return OriginalWindowProc(hWnd, Msg, wParam, lParam);
}

int user_money = 0;

#pragma region Misc Mods UI Rendering

#ifndef _RELEASE_MODE

char user_cutsceneID[256] = { '\0' };
char user_objBrowserFilter[256] = { '\0' };
bool bailuOrChobu = false;

AS3SearchArea* gCutsceneMgr = nullptr;
AS3BgmManager* gBgmManager = nullptr;
ABP_S3BgmPlayer_C* gBgmPlayer = nullptr;


std::vector<AS3BgmPlayer*> bgmPlayers;

bool isSelected = false, showObjBrowser = false, showOnlyInPersistentLevel = true;
void DebugRenderer() {
	for (auto aBgmPlayer : UObject::FindObjects<ABP_S3BgmPlayer_C>()) {
		if (IsNameDefault(aBgmPlayer->GetFullName()))
			continue;
		else if (aBgmPlayer != gBgmPlayer)
			gBgmPlayer = aBgmPlayer;
	}

	for (auto aBgmManager : UObject::FindObjects<AS3BgmManager>()) {
		if (IsNameDefault(aBgmManager->GetFullName()))
			continue;
		else if (aBgmManager != gBgmManager)
			gBgmManager = aBgmManager;
	}

	for (auto aCutsceneMgr : UObject::FindObjects<AS3SearchArea>()) {
		if (IsNameDefault(aCutsceneMgr->GetFullName()))
			continue;
		else if (gCutsceneMgr != aCutsceneMgr)
			gCutsceneMgr = aCutsceneMgr;
	}

	// Object Browser
	ImGui::Begin("Object Browser");
	ImGui::Text("Object Cnt: %d", UObject::GetGlobalObjects().Num());
	ImGui::Checkbox("Show Browser", &showObjBrowser);
	ImGui::Checkbox("Show Only Objects in Persistent Level", &showOnlyInPersistentLevel);
	if (showObjBrowser) {
		ImGui::InputText("Filter", user_objBrowserFilter, 256);
		ImGui::BeginListBox("", ImVec2(425, 550));
		if (UObject::GetGlobalObjects().Num() > 1) {
			for (int i = 0; i < UObject::GetGlobalObjects().Num(); i++) {
				if (!UObject::GetGlobalObjects().IsValidIndex(i))
					continue;
				UObject* obj = UObject::GetGlobalObjects().GetByIndex(i);
				if (obj) {
					if (showOnlyInPersistentLevel) {
						if (!IsInPersistentLevel(obj->GetFullName()))
							continue;
					}
					if (strlen(user_objBrowserFilter) > 0) {
						if (strstr(UObject::GetGlobalObjects().GetByIndex(i)->GetName().c_str(), user_objBrowserFilter)) {
							if (ImGui::Selectable(UObject::GetGlobalObjects().GetByIndex(i)->GetName().c_str(), isSelected)) {
								AActor* actorPtr = reinterpret_cast<AActor*>(obj);
								if (GetRyo() && actorPtr) {
									GetRyo()->K2_TeleportTo(actorPtr->K2_GetActorLocation(), { 0,0,0 });
								}
							}
						}
					}
					else {
						if (ImGui::Selectable(UObject::GetGlobalObjects().GetByIndex(i)->GetName().c_str(), isSelected)) {
							AActor* actorPtr = reinterpret_cast<AActor*>(obj);
							if (GetRyo() && actorPtr) {
								GetRyo()->K2_TeleportTo(actorPtr->K2_GetActorLocation(), { 0,0,0 });
							}
						}
					}
				}
			}
		}
		ImGui::EndListBox();
	}
	ImGui::End();

	ImGui::Begin("Debug UI");

	if (ImGui::Button("Restart BGM"))
	{
		for (auto bgmMan : UObject::FindObjects<AS3BgmManager>())
		{
			bgmMan->bDisableFadePause = true;
			for (auto bgmArea : UObject::FindObjects< ABP_S3BgmArea_C>())
			{
				if (bgmArea->ActiveCue)
					printf("activecue = %s\n", bgmArea->ActiveCue->GetName().c_str());

				

				if (bgmArea->bIsInArea && bgmArea->SoundAtomCue) 
				{
					printf("loaded = %d\n", bgmArea->SoundAtomCue->CueSheet->IsLoaded());
				}
			}
		}
	}


	if (ImGui::Button("List Cutscenes")) {
		for (auto dbgCut : UObject::FindObjects<US3CutsceneLevelData>()) {
			if (IsNameDefault(dbgCut->GetFullName()))
				continue;

			printf("CutsceneId = %s\n", dbgCut->CutsceneId.GetName());
		}
	}
	ImGui::InputText("Cutscene ID", user_cutsceneID, 256);
	if (ImGui::Button("Load Cutscene"))	{
		for (auto cutMgr : UObject::FindObjects<ABP_CutsceneManager_C>()) {
			if (IsNameDefault(cutMgr->GetFullName()))
				continue;

			for (auto dbgCut : UObject::FindObjects<US3CutsceneLevelData>()) {
				if (IsNameDefault(dbgCut->GetFullName()))
					continue;
				else if (strstr(dbgCut->CutsceneId.GetName(), user_cutsceneID)) {
					cutMgr->RequestLoadLevel(dbgCut->CutsceneId);
					cutMgr->LoadCutsceneLevel(dbgCut->CutsceneId);
				}
			}
		}
	}
	if (ImGui::Button("Play Cutscene")) {
		for (auto levelSequencePlayer : UObject::FindObjects<ULevelSequencePlayer>()) {
			if (IsNameDefault(levelSequencePlayer->GetFullName()))
				continue;
			else {
				levelSequencePlayer->Play();
			}
		}
	}


	if (ImGui::Button("Load All Levels"))
		LoadAllLevels();

	if (ImGui::Button("Spawn Ryo")) {
		/*auto newRyo = StaticNewObject<UObject>(theWorld->PersistentLevel, FName("None"), RF_NoFlags, EInternalObjectFlags::None, nullptr, false, nullptr, false);
		if (newRyo) {
			printf(":)\n");
		} else {
			printf(":(\n");
		}*/
	
		if (GetRyo()) {
			auto statics = UObject::FindObject<UGameplayStatics>();
			if (statics) {				
				USkeletalMesh* ryoSkelMesh = StaticLoadObject<USkeletalMesh>(nullptr, L"/Game/char/Adventure/RYO/Meshes/Body/000_00/SK_RYO_F000_00.SK_RYO_F000_00", 0, 0, nullptr, true);
				if (ryoSkelMesh) {
					printf("Loaded '%s'\n", ryoSkelMesh->GetFullName().c_str());
				}

				// Raw UObject creation, confirmed.
				AS3Character* newObject = reinterpret_cast<AS3Character*>(statics->STATIC_SpawnObject(AS3Character::StaticClass(), theWorld->PersistentLevel));
				if (newObject && ryoSkelMesh) {
					printf("Created '%s'\n", newObject->GetFullName().c_str());
					newObject->RootComponent->SetActive(true, true);
					newObject->RootComponent->ToggleVisibility(true);
					newObject->bSimGravityDisabled = true;
					newObject->GetMovementComponent()->SetActive(false, false);
					newObject->K2_SetActorLocationAndRotation(GetRyo()->GetTransform().Translation, { 0,0,0 }, false, true, nullptr);
					//newObject->SetActorLocationAndRotationWithCamera(GetRyo()->GetTransform().Translation, { 0,0,0 }, false, true);

					newObject->SpawnDefaultController();
					newObject->Mesh->SetSkeletalMesh(ryoSkelMesh, true);
				}
				//AActor* newActor = statics->BeginSpawningActorFromBlueprint(theWorld, (UBlueprint*)ABP_S3_Character_Adventure_C::StaticClass(), {}, true);
				/*AActor* newActor = statics->BeginSpawningActorFromClass(theWorld, ABP_S3_Character_Adventure_C::StaticClass(), {0,0,0}, true, nullptr);
				if (newActor) {
					statics->FinishSpawningActor(newActor, { 0,0,0 });
				}*/
			}
		}
	}

	if (ImGui::Button("Dump All Items")) {
		if (theWorld) {
			auto itemFuncLib = UObject::FindObject<UBPF_ItemTable_C>();
			auto textDataLibs = UObject::FindObjects<US3TextDataManagerBase>();
			US3TextDataManagerBase* textDataLib = nullptr;
			for (auto _textDataLib : textDataLibs) {
				if (!IsNameDefault(_textDataLib->GetFullName()))
					textDataLib = _textDataLib;
				else continue;
			}

			if (itemFuncLib && textDataLib) {
				std::string path = R"(H:\S3\Lists\General.txt)";
				{
					std::ofstream outFile(path);
					for (int i = 0; i <= 2005; ++i) {
						{
							FName nameLabel;
							itemFuncLib->STATIC_ItemTableIndexToLabel(i, theWorld, &nameLabel);
							outFile << "---------------------------------------------------" << std::endl;
							outFile << "ID: " << nameLabel.GetName() << std::endl;
							outFile << "Name: " << textDataLib->GetItemName(nameLabel).ToString() << std::endl;
							outFile << "Description: " << textDataLib->GetItemDescription(nameLabel).ToString() << std::endl;
							outFile << "---------------------------------------------------" << std::endl;
						}
					}
					outFile.close();
				}
			}
		}

	}

	ImGui::Checkbox("Force Can Skip Dialog", &forceCanSkipDialog);

	if (ImGui::Button("Get Bgm Area Infos")) {
		auto bgmAreas = UObject::FindObjects<AS3BgmArea>();
		int i = 0; 
		for (auto bgmArea : bgmAreas) {
			printf("AreaSize	      = %f %f %f\n", bgmArea->AreaSize.X, bgmArea->AreaSize.Y, bgmArea->AreaSize.Z);

			if(bgmArea->SoundAtomCue)
				printf("SoundAtomCue      = %s\n", bgmArea->SoundAtomCue->GetFullName().c_str());
			if (bgmArea->SoundAtomCueSheet) {
				printf("SoundAtomCueSheet = %s\n", bgmArea->SoundAtomCueSheet->GetFullName().c_str());



				if (!bgmArea->SoundAtomCueSheet->IsLoaded())
				{
					printf("Not Loaded!!!\n");
				}

			}





			++i;
		}
		printf("Found %d BGM cues\n", i);
	}
	if (ImGui::Button("List All NPCs")) {
		auto npcMans = UObject::FindObjects<AS3NPCManager>();
		for (auto npcMan : npcMans) {
			auto allNPCs = npcMan->GetAllNPC();
			if ((int)allNPCs.Num() > 0) {
				printf("GetAllNPC() =  %d\n", (int)allNPCs.Num());
				for (int i = 0; i < allNPCs.Num(); ++i) {
					if (allNPCs.IsValidIndex(i)) {
						AS3Character* aChar = allNPCs[i];
						if (aChar) {
							printf("[%s] DistanceFrmPlayer: %f \t Gender: %s\n", aChar->CharacterID.TagName.GetName(), 
																				 aChar->DistanceFromPlayer, 
																				 aChar->Gender.GetValue()					 == ES3CharacterGender::Gender_Female ? 
																						"Female" : (aChar->Gender.GetValue() == ES3CharacterGender::Gender_Male ? 
																						"Male"   : "Unspecified"));
						}
					}
				}
			}
		}
	}
	ImGui::End();
}
#endif

static bool charSelectedListRyo		= false;
static bool charSelectedListShenhua	= false;
static bool showWarpingWindow	= false;

static float warpPosition[3];

static bool selectedReplacement = false;

void RenderScene()
{
	static std::once_flag flag;
	std::call_once(flag, []() { IndiciumEngineLogInfo("++ RenderScene called"); });

#ifndef _RELEASE_MODE
	DebugRenderer();
#endif

	if (showWarpingWindow) 
	{
		ImGui::Begin("Warping Window");
		for (auto controller : UObject::FindObjects<ADebugCameraController>()) {
			if (controller && !IsNameDefault(controller->GetFullName())) {
				ImGui::Text("Freecam Position");
				ImGui::Separator();
				ImGui::Text("X: %f \tY: %f \tZ: %f",	controller->PlayerCameraManager->GetCameraLocation().X, 
														controller->PlayerCameraManager->GetCameraLocation().Y, 
														controller->PlayerCameraManager->GetCameraLocation().Z);
			}
		}
		if (GetRyo() != nullptr) {
			ImGui::Text("Ryo Position");
			ImGui::Separator();
			ImGui::Text("X: %f \tY: %f \tZ: %f", GetRyo()->K2_GetActorLocation().X,
				GetRyo()->K2_GetActorLocation().Y,
				GetRyo()->K2_GetActorLocation().Z);
		}
		ImGui::InputFloat3("Warp Position", warpPosition);
		if (ImGui::Button("Go")) {
			FVector newLoc;
			newLoc.X = warpPosition[0];
			newLoc.Y = warpPosition[1];
			newLoc.Z = warpPosition[2];
			
			GetRyo()->K2_TeleportTo(newLoc, FRotator());
			//GetRyo()->SetActorLocationAndRotationWithCamera(newLoc, FRotator(), false, true);
		}
		ImGui::End();
	}
	if (show_overlay) 
	{
		ImGui::Begin("Misc Mods Menu", &show_overlay);

		ImGuiIO& io = ImGui::GetIO();
		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);

		if (ImGui::Button("Load All Levels"))
			LoadAllLevels();
		ImGui::Separator();

		ImGui::Text("Mods");
		ImGui::Separator();

		ImGui::Checkbox("Multiply Money Earned", &multMoneyReceived);
		ImGui::Checkbox("Energy Drain QOL", &drainQOL);
		
		ImGui::InputInt("", &user_money);
		if (ImGui::Button("Set Money") && gameInstance) {
			// MoneyFunc_Orig(user_money);

			gameInstance->SetHaveMoney(user_money);
		}

		ImGui::Checkbox("Show Warping Window", &showWarpingWindow);

		// Classic Camera Mod
		ImGui::Checkbox("Classic Camera Mod", &bRetroCameraModEnabled);
		if (bRetroCameraModEnabled) {
			ImGui::Text("Options:");
			ImGui::Separator(); ImGui::Indent();
			ImGui::Checkbox("Use Custom Values", &bApplyCustomCameraValues);
			if (bApplyCustomCameraValues) {
				ImGui::Indent();
				ImGui::DragFloat("FOV", &customFOV);
				ImGui::DragFloat("Camera Distance", &customCamDistance);
				ImGui::DragFloat("Camera Height", &customCamHeight);
				ImGui::DragFloat("Camera Pitch Offset", &customCameraPitchOffset);
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		// NPC Fixes
		ImGui::Checkbox("NPC Fixes", &npcFixesEnabled);
		if (npcFixesEnabled) {
			ImGui::Text("Options:");
			ImGui::Separator(); ImGui::Indent();
			ImGui::DragInt("Max Loaded NPCs", &maxLoadedNPCs);
			ImGui::DragInt("Max Visible NPCs", &maxVisibleNPCs);
			ImGui::DragFloat("NPC-Player Max Distance", &customDistance);
			ImGui::DragFloat("NPC Streaming Volume Scale", &customScale);
			ImGui::Unindent();
		}

		// Misc. Fixes/Utilities
		ImGui::Checkbox("No Run Energy Drain",		&bNoDrainRunEnergy);
		ImGui::Checkbox("No Walk Only Zones",		&bNoWalkOnlyTriggers);
		ImGui::Checkbox("Any Time Fishing",			&bAnyTimeFishing);
		ImGui::Checkbox("Force Can Skip Dialog",	&forceCanSkipDialog);

		ImGui::Checkbox("Herb Location Dumper", &bDumpHerbs);
		if (bDumpHerbs) {
			ImGui::Text("Options:");
			ImGui::Separator(); ImGui::Indent();
			if (ImGui::Button("Load All Levels"))
				LoadAllLevels();
			ImGui::Unindent();
		}
		
		if (replacements.size()) 
		{
			if (ImGui::Checkbox("Toggle Replacements", &toggleReplacements))
			{
				for (auto& replacement : replacements) 
				{
					if (toggleReplacements) 
					{
						replacement.should_swap = true;
						replacement.has_swapped = false;
					}
					else 
					{
						replacement.should_swap = false;
						replacement.has_swapped = false;
					}
				}
			}

			if (toggleReplacements) 
			{
				ImGui::BeginListBox("##replacements");
				for (auto& replacement : replacements) {
					std::string tmp = replacement.original + " ---> " + replacement.replacement;
					if (ImGui::Selectable(tmp.c_str(), selectedReplacement)) {
						replacement.should_swap = !replacement.should_swap;
						replacement.has_swapped = false;
					}
				}
				ImGui::EndListBox();
			}
		}

		ImGui::Checkbox("Swap Ryo", &shouldSwapRyo);
		if (swapCharacterNameMap.find(swapCharacterRyo) != swapCharacterNameMap.end()) {
			std::string tmpName = swapCharacterNameMap.find(swapCharacterRyo)->second;
			ImGui::Text("Current Char: %s", tmpName.c_str());
		}

		if (shouldSwapRyo) {
			ImGui::BeginListBox("##swapryo");
			for (auto& aChar : swapCharacterNameMap) {
				std::string& aCharName = aChar.second;
				if (ImGui::Selectable(aCharName.c_str(), charSelectedListRyo)) {
					swapCharacterRyo = aChar.first;
					hasSwappedRyo = false;
				}
			}
			ImGui::EndListBox(); 
		}

		ImGui::Checkbox("Swap Shenhua", &shouldSwapShenhua);
		if (swapCharacterNameMap.find(swapCharacterShenhua) != swapCharacterNameMap.end()) {
			std::string tmpName = swapCharacterNameMap.find(swapCharacterShenhua)->second;
			ImGui::Text("Current Char: %s", tmpName.c_str());
		}

		if (shouldSwapShenhua) {
			ImGui::BeginListBox("##swapshenhua");
			for (auto& aChar : swapCharacterNameMap) {
				std::string& aCharName = aChar.second;
				if (ImGui::Selectable(aCharName.c_str(), charSelectedListShenhua)) {
					swapCharacterShenhua = aChar.first;
					hasSwappedShenhua = false;
				}
			}
			ImGui::EndListBox();
		}

		ImGui::End();
	}
	ImGui::Render();
}
#pragma endregion

static bool hasReset = false, needsReset = false;

Version theVersion;

bool npcFuncHooked = false;

void Attach() {


	Sleep(2000);
	if (init() == -1) return;
	g_BaseAddress = (DWORD_PTR)GetModuleHandleA(NULL);
	theVersion = determineVersion();

	DWORD_PTR staticLoadOffs = 0;
	if (memcmp((void*)(g_BaseAddress + STATIC_LOAD_OBJECT_OFFS_V101_CODEX), "\x40\x55\x53\x56\x41\x54\x41\x55", 5) == 0) {
		staticLoadOffs = STATIC_LOAD_OBJECT_OFFS_V101_CODEX;
	}
	else if (memcmp((void*)(g_BaseAddress + STATIC_LOAD_OBJECT_OFFS_V101_EGS), "\x40\x55\x53\x56\x41\x54\x41\x55", 5) == 0) {
		staticLoadOffs = STATIC_LOAD_OBJECT_OFFS_V101_EGS;
	}
	else if (memcmp((void*)(g_BaseAddress + STATIC_LOAD_OBJECT_OFFS_V10401_EGS), "\x40\x55\x53\x56\x41\x54\x41\x55", 5) == 0) {
		staticLoadOffs = STATIC_LOAD_OBJECT_OFFS_V10401_EGS;
	}
	else if (memcmp((void*)(g_BaseAddress + STATIC_LOAD_OBJECT_OFFS_V10401_CODEX), "\x40\x55\x53\x56\x41\x54\x41\x55", 5) == 0) {
		staticLoadOffs = STATIC_LOAD_OBJECT_OFFS_V10401_CODEX;
	}
	else if (memcmp((void*)(g_BaseAddress + STATIC_LOAD_OBJECT_OFFS_V10501_EGS), "\x40\x55\x53\x56\x41\x54\x41\x55", 5) == 0) {
		staticLoadOffs = STATIC_LOAD_OBJECT_OFFS_V10501_EGS;
	}
	else if (memcmp((void*)(g_BaseAddress + STATIC_LOAD_OBJECT_OFFS_V10600_STEAM), "\x40\x55\x53\x56\x41\x54\x41\x55", 5) == 0) {
		staticLoadOffs = STATIC_LOAD_OBJECT_OFFS_V10600_STEAM;
	}
	else {
		MessageBoxA(NULL, "Unable to find StaticLoadObject offset. Exiting.", "Misc Mods", MB_OK);
		return;
	}

	swapCharacterRyo = RyoHazuki;
	swapCharacterShenhua = Shenhua;

	MH_CreateHook(reinterpret_cast<void*>(g_BaseAddress + staticLoadOffs), StaticLoadObject_Hook, reinterpret_cast<void**>(&StaticLoadObject_orig));
	MH_EnableHook(reinterpret_cast<void*>(g_BaseAddress + staticLoadOffs));

#ifdef PROCESS_EVENT_LOGGER
	if (std::filesystem::exists("debug.log")) {
		std::filesystem::remove("debug.log");
	}
	ofs.open("debug.log");
#endif

	bool bShouldSpawnConsole = false;

	// read user defaults from INI
	INIReader reader("miscmods.ini");
	if (reader.ParseError() < 0) {
		printf("Can't load 'miscmods.ini'\n");
	}
	else {
		bShouldSpawnConsole = reader.GetBoolean("general", "open_console", false);
		show_overlay = reader.GetBoolean("general", "open_gui", true);

		bRetroCameraModEnabled = reader.GetBoolean("mods", "retro_camera_mod", true);
		npcFixesEnabled = reader.GetBoolean("mods", "npc_fixes", true);
		bNoDrainRunEnergy = reader.GetBoolean("mods", "no_energy_manager", false);
		bNoWalkOnlyTriggers = reader.GetBoolean("mods", "no_walk_only_triggers", false);
		bDumpHerbs = reader.GetBoolean("mods", "dump_herbs", false);
		forceCanSkipDialog = reader.GetBoolean("mods", "force_dialog_can_skip", false);
		bAnyTimeFishing = reader.GetBoolean("mods", "any_time_fishing", false);

		maxLoadedNPCs = reader.GetInteger("npc_fixes", "max_loaded_npcs", 256);
		maxVisibleNPCs = reader.GetInteger("npc_fixes", "max_visible_npcs", 256);

		customDistance = (float)reader.GetReal("npc_fixes", "max_npc_distance", 50000.f);
		customScale = (float)reader.GetReal("npc_fixes", "max_npc_trigger_scale", 30000.f);

		bApplyCustomCameraValues = reader.GetBoolean("retro_camera_mod", "apply_custom_values", false);

		customFOV = (float)reader.GetReal("retro_camera_mod", "customFOV", 55.0f);
		customCamHeight = (float)reader.GetReal("retro_camera_mod", "customHeight", 25.0f);
		customCamDistance = (float)reader.GetReal("retro_camera_mod", "customDistance", 455.0f);
		customCameraPitchOffset = (float)reader.GetReal("retro_camera_mod", "customCameraPitchOffset", -5.0f);
	}

	if (bDumpHerbs) {
		LoadLocations();
	}

	if (bShouldSpawnConsole) {
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
		printf("Allocated console\n");
		SetConsoleTitleA(MOD_STRING);

		auto buildInfoConfig = UObject::FindObject<US3BuildInfoConfig>();
		if (buildInfoConfig) {
			printf("=======================\nBuild Info Config\n=======================\n");
			printf("BuildDate: %s\n", buildInfoConfig->BuildDate.ToString().c_str());
			printf("BuildNumber: %d\n", buildInfoConfig->BuildNumber);
			printf("BuildType: %s\n", buildInfoConfig->BuildType.ToString().c_str());
			printf("Changelist: %d\n", buildInfoConfig->Changelist);
			printf("Configuration: %s\n", buildInfoConfig->Configuration.ToString().c_str());
			printf("Platform: %s\n", buildInfoConfig->Platform.ToString().c_str());
			printf("=======================\n");
		}
	}

	static bool hasSetOrig = false;
	static float origDefaultFOV, origCurrentArmLength, origINDOOR_ARM_LENGTH, origTargetArmLength, origSpringArmTargetArmLength,
		origDEFAULT_ARM_LENGTH, origCAMERA_PITCH_OFFSET, origSABUN_PITCH_LIMIT, origDEFAULT_ARM_HEIGHT, origbEnableAutoRotate,
		origbUserEnableAutoCameraRotation;

	static float settingCameraCenterRot;
	static float SABUN_PITCH_LIMIT_new;
	static float CAMERA_PITCH_OFFSET_new;

	CreateProcessEventHook("Retro Camera Mod", "Function BP_S3PlayerCameraManagerBase.BP_S3PlayerCameraManagerBase_C.BlueprintUpdateCamera", [](class UObject* _this, class UFunction* a2, void* pParms) {
			if (bHookActive) {
				ABP_S3PlayerCameraManagerBase_C* camBase = reinterpret_cast<ABP_S3PlayerCameraManagerBase_C*>(_this);
				if (camBase && _this) {
					if (!hasSetOrig) {
						origDefaultFOV = camBase->LockedFOV;
						origCurrentArmLength = camBase->CurrentArmLength;
						origINDOOR_ARM_LENGTH = camBase->INDOOR_ARM_LENGTH;
						origTargetArmLength = camBase->TargetArmLength;

						if (camBase->SpringArm)
							origSpringArmTargetArmLength = camBase->SpringArm->TargetArmLength;

						origDEFAULT_ARM_LENGTH = camBase->DEFAULT_ARM_LENGTH;
						origCAMERA_PITCH_OFFSET = camBase->CAMERA_PITCH_OFFSET;
						origSABUN_PITCH_LIMIT = camBase->SABUN_PITCH_LIMIT;
						origDEFAULT_ARM_HEIGHT = camBase->DEFAULT_ARM_HEIGHT;
						origbUserEnableAutoCameraRotation = camBase->bUserEnableAutoCameraRotation;
						origbEnableAutoRotate = camBase->bEnableAutoRotate;

						hasSetOrig = true;
					}

					if (camBase->CameraState == ECameraState::ECameraState__Zoom ||
						camBase->CameraState == ECameraState::ECameraState__Search ||
						playerBehavior == ES3PlayerBehavior::ES3PlayerBehavior__Search) {
						ProcessEventOriginal(_this, a2, pParms);
						return;
					}

					if (bRetroCameraModEnabled)
					{
						camBase->IsInBuilding = bRetroCameraModEnabled;

						hasReset	= true, needsReset	= false;

						camBase->INDOOR_PITCH_LIMITS.X = -88.f;
						camBase->INDOOR_PITCH_LIMITS.Y = 88.f;
						camBase->SpringArmYOffset = 0.0f;

						camBase->CAMERA_PITCH_OFFSET = 0.0f;
						camBase->SABUN_PITCH_LIMIT = 0.0f;
						
						//camBase->PitchLimitResetFlag = 0;
						//camBase->ResetSpringArm = 0;
						//camBase->ResetThirdPerson = 0;

						if (bApplyCustomCameraValues) {
							if (camBase->CameraState != ECameraState::ECameraState__Free || playerBehavior != ES3PlayerBehavior::ES3PlayerBehavior__FreeRun) {
								camBase->LockedFOV = origDefaultFOV;
								camBase->CurrentArmLength = origCurrentArmLength;
								camBase->INDOOR_ARM_LENGTH = origINDOOR_ARM_LENGTH;
								camBase->TargetArmLength = origTargetArmLength;
								camBase->DEFAULT_ARM_LENGTH = origDEFAULT_ARM_LENGTH;
								camBase->CAMERA_PITCH_OFFSET = origCAMERA_PITCH_OFFSET;
								camBase->SABUN_PITCH_LIMIT = origSABUN_PITCH_LIMIT;
								camBase->DEFAULT_ARM_HEIGHT = origDEFAULT_ARM_HEIGHT;

								if (camBase->SpringArm)
									camBase->SpringArm->TargetArmLength = origSpringArmTargetArmLength;

								camBase->bUserEnableAutoCameraRotation = origbUserEnableAutoCameraRotation;
								camBase->bEnableAutoRotate = origbEnableAutoRotate;
							}
							else if (camBase->CameraState == ECameraState::ECameraState__Free && playerBehavior == ES3PlayerBehavior::ES3PlayerBehavior__FreeRun) {
								camBase->LockedFOV = customFOV;
								camBase->CurrentArmLength = customCamDistance;
								camBase->INDOOR_ARM_LENGTH = customCamDistance;
								camBase->TargetArmLength = customCamDistance;
								camBase->DEFAULT_ARM_LENGTH = customCamDistance;
								camBase->DEFAULT_ARM_HEIGHT = customCamHeight;

								camBase->CAMERA_PITCH_OFFSET = customCameraPitchOffset;

								if (camBase->SpringArm)
									camBase->SpringArm->TargetArmLength = customCamDistance;

								camBase->bEnableAutoRotate = false;
								camBase->bUserEnableAutoCameraRotation = false;
							}
							else {
								ProcessEventOriginal(_this, a2, pParms);
								return;
							}
						}
						else {
							if (camBase->CameraState != ECameraState::ECameraState__Free || playerBehavior != ES3PlayerBehavior::ES3PlayerBehavior__FreeRun) {
								camBase->LockedFOV = origDefaultFOV;
								camBase->CurrentArmLength = origCurrentArmLength;
								camBase->INDOOR_ARM_LENGTH = origINDOOR_ARM_LENGTH;
								camBase->TargetArmLength = origTargetArmLength;
								camBase->DEFAULT_ARM_LENGTH = origDEFAULT_ARM_LENGTH;
								camBase->CAMERA_PITCH_OFFSET = origCAMERA_PITCH_OFFSET;
								camBase->SABUN_PITCH_LIMIT = origSABUN_PITCH_LIMIT;
								camBase->DEFAULT_ARM_HEIGHT = origDEFAULT_ARM_HEIGHT;

								if (camBase->SpringArm)
									camBase->SpringArm->TargetArmLength = origSpringArmTargetArmLength;

								camBase->bUserEnableAutoCameraRotation = origbUserEnableAutoCameraRotation;
								camBase->bEnableAutoRotate = origbEnableAutoRotate;
							}
							else if(camBase->CameraState == ECameraState::ECameraState__Free && playerBehavior == ES3PlayerBehavior::ES3PlayerBehavior__FreeRun){
								/*camBase->CurrentArmLength = 285.f;
								camBase->INDOOR_ARM_LENGTH = 285.f;
								camBase->TargetArmLength = 285.f;
								camBase->SpringArm->TargetArmLength = 285.f;
								camBase->DEFAULT_ARM_LENGTH = 285.f;*/

								/*camBase->LockedFOV = 60.0f;
								camBase->CurrentArmLength = 315.f;
								camBase->INDOOR_ARM_LENGTH = 315.f;
								camBase->TargetArmLength = 315.f;
								camBase->SpringArm->TargetArmLength = 315.f;
								camBase->DEFAULT_ARM_LENGTH = 315.f;*/

								camBase->LockedFOV = 55.f;
								camBase->CurrentArmLength = 435.f;//455.f;
								camBase->INDOOR_ARM_LENGTH = 435.f;
								camBase->TargetArmLength = 435.f;
								camBase->DEFAULT_ARM_LENGTH = 435.f;
								camBase->DEFAULT_ARM_HEIGHT = 25.f;

								camBase->CAMERA_PITCH_OFFSET = -3.0f;

								if (camBase->SpringArm)
									camBase->SpringArm->TargetArmLength = 435.f;

								camBase->bEnableAutoRotate = false;
								camBase->bUserEnableAutoCameraRotation = false;
							}
							/*else {
								ProcessEventOriginal(_this, a2, pParms);
								return;
							}*/
						}
					}
					else {
						if (needsReset && !hasReset) 
						{
							camBase->LockedFOV = origDefaultFOV;
							camBase->CurrentArmLength = origCurrentArmLength;
							camBase->INDOOR_ARM_LENGTH = origINDOOR_ARM_LENGTH;
							camBase->TargetArmLength = origTargetArmLength;
							camBase->DEFAULT_ARM_LENGTH = origDEFAULT_ARM_LENGTH;
							camBase->CAMERA_PITCH_OFFSET = origCAMERA_PITCH_OFFSET;
							camBase->SABUN_PITCH_LIMIT = origSABUN_PITCH_LIMIT;
							camBase->DEFAULT_ARM_HEIGHT = origDEFAULT_ARM_HEIGHT;

							if (camBase->SpringArm) 
								camBase->SpringArm->TargetArmLength = origSpringArmTargetArmLength;

							camBase->bUserEnableAutoCameraRotation = origbUserEnableAutoCameraRotation;
							camBase->bEnableAutoRotate = origbEnableAutoRotate;

							hasReset = true, needsReset = false;
						}
					}
				}
			}
			ProcessEventOriginal(_this, a2, pParms);
		});

#ifndef _RELEASE_MODE
	/*MH_STATUS mhStatus = MH_CreateHook(reinterpret_cast<void**>(g_BaseAddress + 0x2D9300), StaticNewObject_hook, reinterpret_cast<void**>(&NewObject_orig));
	mhStatus = MH_EnableHook(reinterpret_cast<void*>(g_BaseAddress + 0x2D9300));
	if (mhStatus != MH_OK) {
		MessageBoxA(NULL, "Could not hook ConstructObjectInternal. Exiting.", "Misc Mods", MB_OK);
		return;
	}*/
#endif
	read_char_replacements("replacements.csv");

	while (!receiveTickHooked) {
		if (!receiveTickHooked) {
			UFunction* fnPtr = UObject::FindObject<UFunction>("Function BP_NPCManager.BP_NPCManager_C.ReceiveTick");
			//UFunction* fnPtr = UObject::FindObject<UFunction>("Function Engine.Actor.ReceiveTick");
			if (fnPtr && fnPtr->Func != 0) {
				MH_Initialize();
				MH_STATUS mhStatus1 = MH_CreateHook(reinterpret_cast<void**>(fnPtr->Func), ReceiveTickHook, reinterpret_cast<void**>(&origReceiveTick));

				mhStatus1 = MH_EnableHook(reinterpret_cast<void*>(fnPtr->Func));
				if (mhStatus1 == MH_OK) {
					receiveTickHooked = true;
					g_ThreadAlive = true;
				}
			}
		}
	}

	// @TODO: Update offsets for other versions...
	DWORD_PTR worldOffs, engineOffs;
	while (receiveTickHooked && g_ThreadAlive) {
		if (theWorld == nullptr && theEngine == nullptr) {
			if (theVersion == V10601_STEAM) {
				worldOffs = *(DWORD_PTR*)(g_BaseAddress + 0x33C8DA0);
				engineOffs = *(DWORD_PTR*)(g_BaseAddress + 0x33C66F8);
			}
			else if (theVersion != V10501) {
				worldOffs = *(DWORD_PTR*)(g_BaseAddress + 0x33C7D20);
				engineOffs = *(DWORD_PTR*)(g_BaseAddress + 0x33C5678);
			} else {
				worldOffs = *(DWORD_PTR*)(g_BaseAddress + 0x33C8D20);
				engineOffs = *(DWORD_PTR*)(g_BaseAddress + 0x33C6678);
			}
			theWorld = reinterpret_cast<UWorld*> (worldOffs);
			theEngine = reinterpret_cast<UEngine*> (engineOffs);
		}
		else {
		}

		// Toggle setting custom cam values
		if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_NUMPAD5) & 0x8000)) {
			bApplyCustomCameraValues = !bApplyCustomCameraValues;
			printf("setting %s\n", (bApplyCustomCameraValues ? "on" : "off")); Sleep(100);
		}
		// FOV
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_UP) & 0x8000)) {
			customFOV += 5.0f;
			printf("Set customFOV to %f\n", customFOV);  Sleep(100);
		}
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_DOWN) & 0x8000)) {
			customFOV -= 5.0f;
			printf("Set customFOV to %f\n", customFOV);  Sleep(100);
		}
		// Distance
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_NUMPAD8) & 0x8000)) {
			customCamDistance += 10.0f;
			printf("Set camDistance to %f\n", customCamDistance);  Sleep(100);
		}
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_NUMPAD2) & 0x8000)) {
			customCamDistance -= 10.0f;
			printf("Set camDistance to %f\n", customCamDistance);  Sleep(100);
		}
		// Height
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_PRIOR) & 0x8000)) {
			customCamHeight += 10.0f;
			printf("Set camHeight to %f\n", customCamHeight);  Sleep(100);
		}
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_NEXT) & 0x8000)) {
			customCamHeight -= 10.0f;
			printf("Set camHeight to %f\n", customCamHeight);  Sleep(100);
		}
		// Enable/disable hook etc
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('C') & 0x8000)) {
			bRetroCameraModEnabled = !bRetroCameraModEnabled;
			bHookActive = bRetroCameraModEnabled;
			Sleep(100);
		}
		// NPC manager fixes
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('N') & 0x8000)) {
			npcFixesEnabled = !npcFixesEnabled;
			Sleep(100);
		}
		// No Drain Run Energy patch
		else if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('D') & 0x8000)) {
			bNoDrainRunEnergy = !bNoDrainRunEnergy;
			Sleep(100);
		}
		// Load all levels
		else if (bDumpHerbs && ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('L') & 0x8000))) {
			LoadAllLevels();
			Sleep(100);
		}
		Sleep(1);
	}
#ifdef PROCESS_EVENT_LOGGER
	ofs.close();
#endif
}