#include "stdafx.h"

#include <iostream>
#include "WrpConsole.h"
#include "../shared/MirvDeathMsgFilter.h"

#include <cstddef>
#include <cstdint>
#include <Windows.h>
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/utlmap.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include "DeathMsg.h"
#include "MirvPovDeathPanel.h"
#include "Globals.h"
#include "ClientEntitySystem.h"
#include "SchemaSystem.h"
#include "MirvColors.h" 
#include "MirvPanorama.h"
#include "MirvPovHud.h"
#include "MirvPovFeedback.h"
#include "MirvPovCore.h"
#include "MirvTime.h"

#include "addresses.h"

#include <set>
#include <algorithm>
#include <vector>

// TODO: move panorama stuff out after addresses.cpp is done
// decompose/change myPanoramaWrapper too
// doing it messy way here for now because lazy

// credit https://github.com/danielkrupinski/Osiris

struct CPanel2D {
	const char* getClassName() {
		// in case it breaks see nearby functions in vtable, it just returns DAT
		void * pClientClass = ((void * (__fastcall *)(void *)) (*(void***)this)[61]) (this);

		if(pClientClass) {
			return *(const char**)((unsigned char*)pClientClass + 0x8);
		}

		return nullptr;
	}
};

currentGameCamera g_CurrentGameCamera;

struct PlayerInfo {
	char* name;
	uint64_t xuid;
	int specKey;
	int userId;
	u_char* playerController;
};

PlayerInfo getSpectatedPlayer() 
{
	PlayerInfo result = {0,0,0,-1,0};
	auto cameraOrigin = g_CurrentGameCamera.origin;
	auto cameraAngles = g_CurrentGameCamera.angles;

    int highestIndex = GetHighestEntityIndex();
	for(int i = 0; i < highestIndex + 1; i++)
	{
        if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i))
		{
			if(!ent->IsPlayerPawn()) continue;

			float entityOrigin[3];
			ent->GetRenderEyeOrigin(entityOrigin);
			float entityAngles[3];
			ent->GetRenderEyeAngles(entityAngles);

			std::vector<double> deltaList = {
				std::abs(std::abs(entityOrigin[0]) - std::abs(cameraOrigin[0])),
				std::abs(std::abs(entityOrigin[1]) - std::abs(cameraOrigin[1])),
				std::abs(std::abs(entityOrigin[2]) - std::abs(cameraOrigin[2])),
				std::abs(std::abs(entityAngles[0]) - std::abs(cameraAngles[0])),
				std::abs(std::abs(entityAngles[1]) - std::abs(cameraAngles[1])),
				std::abs(std::abs(entityAngles[2]) - std::abs(cameraAngles[2]))
			};

			if (
				deltaList[0] > 0.2f || deltaList[1] > 0.2f || deltaList[2] > 0.2f ||
				deltaList[3] > 0.2f || deltaList[4] > 0.2f || deltaList[5] > 0.2f
			) continue;

			auto controllerIndex = ent->GetPlayerControllerHandle().GetEntryIndex();
			if (-1 == controllerIndex) continue;

			auto playerController = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,controllerIndex);

			auto xuid = *(uint64_t*)((u_char*)(playerController) + g_clientDllOffsets.CBasePlayerController.m_steamID);
			auto name = (char*)((u_char*)(playerController) + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName);

            if (0 == xuid) advancedfx::Warning("Error: could not find xuid for entity %i\n", controllerIndex);
            if (nullptr == name || 0 == strlen(name)) advancedfx::Warning("Error: could not find name for entity %i\n", controllerIndex);

			result.name = name;
			result.xuid = xuid;
			result.userId = controllerIndex - 1;
			result.playerController = (u_char*)(playerController);
			// speckey is not really needed here
			break;
		}
	}

	if (-1 == result.userId) 
		advancedfx::Warning(
			"Could not find spectated player.\n"
			"Make sure you're in pov mode and camera fully switched.\n"		
		);

	return result;
};

PlayerInfo getPlayerInfoFromControllerIndex(int entindex)
{
	PlayerInfo result = {0,0,0,0,0};

	// Left screen side keys: 1, 2, 3, 4, 5
	// Right screen side keys: 6, 7, 8, 9, 0
	int slotCT = 0;
	int slotT = 0;

	bool swapPlayerSide = false;
	// apparently in CS2 CT is always on the left side, so we don't have to check for swap
	// but in case they change it in future we can probably check for gamephase like below

    int highestIndex = GetHighestEntityIndex();

	// find gamerules, maybe we can save it since it's pointer
    // for(int i = 0; i < highestIndex + 1; i++)
	// {
    //     if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i))
	// 	{
	// 		if (0 != strcmp("cs_gamerules", ent->GetClassName())) continue;

	// 		auto gameRules = *(u_char**)((u_char*)(ent) + CS2::C_CSGameRulesProxy::m_pGameRules);

	// 		auto gamePhase = *(int*)((gameRules) + CS2::C_CSGameRules::m_gamePhase);
	// 		auto overtimes = *(int*)((gameRules) + CS2::C_CSGameRules::m_nOvertimePlaying);

	// 		advancedfx::Message("gamePhase: %i\n", gamePhase);
	// 		advancedfx::Message("overtimes: %i\n", overtimes); // have to check if overtimes are affecting this or not

	// 		if(3 == gamePhase) swapPlayerSide = true;

	// 		// gamePhase:
	// 		// 	2 = "first"
	// 		// 	3 = "second"
	// 		// 	4 = "halftime"
	// 		// 	5 = "postgame

	// 		break;
	// 	}
	// }

    for(int i = 0; i < highestIndex + 1; i++)
	{
        if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i))
		{
			if(!ent->IsPlayerController()) continue;

			auto teamNumber = *(int*)((u_char*)(ent) + g_clientDllOffsets.C_BaseEntity.m_iTeamNum);
			if (0 == teamNumber || 1 == teamNumber) continue;

			int slot = 0;
			if (3 == teamNumber) // CT
			{
				slot = 1 + slotCT;
				if (swapPlayerSide) slot += 5;
				++slotCT;
			} 
			else if (2 == teamNumber) // T
			{
				slot = 1 + slotT;
				if (!swapPlayerSide) slot += 5;
				++slotT;
			}
			slot = slot % 10;

			if(i != entindex) continue;

			auto xuid = *(uint64_t*)((u_char*)(ent) + g_clientDllOffsets.CBasePlayerController.m_steamID);
			auto name = (char*)((u_char*)(ent) + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName);

            if (0 == xuid) advancedfx::Warning("Error: could not find xuid for entity %i\n", i);
            if (nullptr == name || 0 == strlen(name)) advancedfx::Warning("Error: could not find name for entity %i\n", i);

			result.name = name;
			result.xuid = xuid;
			result.playerController = (u_char*)(ent);
			result.userId = i - 1;
			result.specKey = slot;

			if (i == entindex) break;
        }
    }

	return result;
}

void DeathMsgId::operator=(char const * consoleValue) {
	if (!consoleValue)
		return;

	if (StringBeginsWith(consoleValue, "k"))
	{
		this->Mode = Id_Key;
		this->Id.specKey = atoi(consoleValue +1);
	}
	else if (StringBeginsWith(consoleValue, "x"))
	{
		uintptr_t val;
		
		if (0 == _stricmp("xTrace", consoleValue))
		{
			auto player = getSpectatedPlayer();
			if (-1 != player.userId)
				val = player.xuid;
			else 
				val = 0;
		}
		else
			val = strtoull(consoleValue + 1, 0, 10);

		this->operator=(val);
	}
	else
	{
		int val;

		if (0 == _stricmp("trace", consoleValue))
		{
			auto player = getSpectatedPlayer();
			if (-1 != player.userId)
				val = player.userId;
			else 
				val = 0;
		}
		else
			val = atoi(consoleValue);

		this->operator=(val);
	}
};

bool DeathMsgId::EqualsUserId(int userId)
{
	if (Mode == Id_UserId) return userId == Id.userId;

	if (userId < 0)
		return false;

	switch(Mode)
	{
		case Id_Key:
			// in CS2 playercontroller entityindex is userId + 1
			return getPlayerInfoFromControllerIndex(userId + 1).specKey == Id.specKey;
			break;

		case Id_Xuid:
			return getPlayerInfoFromControllerIndex(userId + 1).xuid == Id.xuid;
			break;
	}

	return false;
};

int DeathMsgId::ResolveToUserId()
{
	switch(Mode)
	{
	case Id_Key:
		{
			int highestIndex = GetHighestEntityIndex();
			for(int i = 0; i < highestIndex + 1; i++)
			{
				if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i))
				{
					if(!ent->IsPlayerController()) continue;
					auto player = getPlayerInfoFromControllerIndex(i);
					if (player.specKey == Id.specKey) return i - 1;
				}
			}
		}
		return 0;
	case Id_Xuid:
		{
			int highestIndex = GetHighestEntityIndex();
			for(int i = 0; i < highestIndex + 1; i++)
			{
				if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i))
				{
					if(!ent->IsPlayerController()) continue;
					auto player = getPlayerInfoFromControllerIndex(i);
					if (player.xuid == Id.xuid) return i - 1;
				}
			}
		}			
		return 0;
	}

	return Id.userId;
};

struct myPanoramaWrapper {
// credit https://github.com/danielkrupinski/Osiris 
// for engine and panel methods
	bool hooked = false;

	short lifeTimeSymbol = -1;
	short spawnTimeSymbol = -1;
	u_char** pUIEngine = nullptr;
	u_char** pHudPanel = nullptr;

	struct ColorEntry {
		u_char* pointer;
		bool use;
		uint32_t value;
		uint32_t defaultValue;
		std::string userValue = "";

		bool convertColorFromStrToInt (const char* str, uint32_t* outColor) {
			if (nullptr == str || nullptr == outColor) return false;

			auto hexStr = afxUtils::rgbaToHex(str, " ");
			if (hexStr.length() != 8) return false;

			*outColor = afxUtils::hexStrToInt(hexStr);
			return true;
		}; 

		bool setColor(const char* arg) {
			if (nullptr == arg) return false;
			if (0 == _stricmp("default", arg))
			{
				use = false;
				value = defaultValue;
			return true;
			}

			for (auto it = afxBasicColors.begin(); it != afxBasicColors.end(); ++it)
			{
				if (0 == _stricmp(it->name, arg))
				{
					use = true;
					userValue = arg;
					value = afxUtils::rgbaToHex(it->value);

					return true;
				}
			}
			return false;
		}

		bool setColor(advancedfx::ICommandArgs* args) {
			auto argc = args->ArgC();

			if (argc == 5)
			{
				uint32_t color;
				std::string str = "";
				str.append(args->ArgV(1));
				str.append(" ");
				str.append(args->ArgV(2));
				str.append(" ");
				str.append(args->ArgV(3));
				str.append(" ");
				str.append(args->ArgV(4));

				if (convertColorFromStrToInt(str.c_str(), &color))
				{
					use = true;
					userValue = str.c_str();
					value = color;
					return true;
				}
			}

			return false;
		};
	};

	ColorEntry BorderColor = { nullptr, false, 0xFF0000E1, 0xFF0000E1 };
	ColorEntry BackgroundColor = { nullptr, false, 0xA0000000, 0xA0000000 };
	ColorEntry LocalBackgroundColor = { nullptr, false, 0xE7000000, 0xE7000000 };
	ColorEntry CTcolor = { nullptr, false, 0xFFE69C6F, 0xFFE69C6F };
	ColorEntry Tcolor = { nullptr, false, 0xFF54BEEA, 0xFF54BEEA };

	void initSymbols() {
		if (-1 != lifeTimeSymbol) return;

		if (nullptr == pUIEngine){
			advancedfx::Warning("pUIEngine is null\n");
			return;
		}

		typedef short(__fastcall *makeSymbol_t)(u_char* ptr, int type, const char* name);
		const auto makeSymbol = *(makeSymbol_t*)((*(u_char**)(*pUIEngine)) + CS2::PanoramaUIEngine::makeSymbol);

		lifeTimeSymbol = makeSymbol(*pUIEngine, 0, "Lifetime");
		spawnTimeSymbol = makeSymbol(*pUIEngine, 0, "SpawnTime");
	};

	u_char* getDeathnotices(){
		if (nullptr == pHudPanel){
			advancedfx::Warning("pHudPanel is null\n");
			return nullptr;
		}

		const auto hudPanel = ((u_char***)pHudPanel)[0][1];
		if (!hudPanel) return nullptr;
		const auto hudDeathNotice = findChildInLayoutFile(hudPanel, "HudDeathNotice");
		if (!hudDeathNotice) return nullptr;
		const auto visibleNotices = findChildInLayoutFile(hudDeathNotice, "VisibleNotices");
		if (!visibleNotices) return nullptr;

		auto deathnotices = visibleNotices + CS2::PanoramaUIPanel::children;

		return deathnotices;
	};

	bool clearDeathnotices(){
		initSymbols();

		const auto pDeathnotices = getDeathnotices(); // dunno how to invalidate it, so will get it each time
		if (nullptr == pDeathnotices) return false;

		if (*(int*)pDeathnotices == 0) return false;

		bool result = false;

		for (int i = 0; i < *(int*)pDeathnotices; i++) {
			const auto panel = ((u_char***)pDeathnotices)[1][i];

			typedef const char* (__fastcall *getAttributeString_t)(u_char* ptr, short attributeName, const char* defaultValue);
			const auto getAttributeString = *(getAttributeString_t*)(*(u_char**)panel + CS2::PanoramaUIPanel::getAttributeString);

			typedef void* (__fastcall *setAttributeString_t)(u_char* ptr, short attributeName, const char* defaultValue);
			const auto setAttributeString = *(setAttributeString_t*)(*(u_char**)panel + CS2::PanoramaUIPanel::setAttributeString);

			const auto lifetimeString = getAttributeString(panel, lifeTimeSymbol, "");
			if (!lifetimeString || strlen(lifetimeString) == 0) continue;

			const auto spawnTimeString = getAttributeString(panel, spawnTimeSymbol, "");
			if (!spawnTimeString || strlen(spawnTimeString) == 0) continue;

			setAttributeString(panel, lifeTimeSymbol, "0.001");
			setAttributeString(panel, spawnTimeSymbol, std::to_string(g_CurrentGameCamera.time - 1).c_str());
			result = true;
		}

		return result;
	}

	void printChildren(const char* parentId) {
		auto parentPanel = ((u_char***)pHudPanel)[0][1];
		if (!parentPanel) return;

		if (strlen(parentId) > 0) {
			auto r = findChildInLayoutFile(parentPanel, parentId);
			if (r) {
				parentPanel = r;
			} else {
				auto r2 = findChildrenInLayoutFileByClassName(parentPanel, parentId);
				if (!r2.empty()) parentPanel = r2[0]; // could be multiple there
			}
		} 

		auto parentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
		auto parentPanel2D = *(CPanel2D**)(parentPanel + 0x8);

		advancedfx::Message("ClientClass / PanelId:\n");
		if (0 != parentPanel2D)
			advancedfx::Message("%s / %s\n", parentPanel2D->getClassName(), parentPanelId);
		else 
			advancedfx::Message("%s / %s\n", "null", parentPanelId);

		const auto children = parentPanel + CS2::PanoramaUIPanel::children;
		if (!children) {
			advancedfx::Warning("No children found.\n");
			return;
		};

		for (int i = 0; i < *(int*)children; ++i) {
			const auto panel = ((u_char***)children)[1][i];
			const auto panelId = *(char**)(panel + CS2::PanoramaUIPanel::panelId);
			auto panel2D = *(CPanel2D**)(panel + 0x8);

			if (!panelId && !panel2D) continue;

			if (0 != panel2D)
				advancedfx::Message("\t%s / %s\n", panel2D->getClassName(), panelId);
			else 
				advancedfx::Message("\t%s / %s\n", "null", panelId);
		}

	}

	std::vector<u_char*> findChildrenInLayoutFileByClassName(u_char* parentPanel, const char* classNameToFind) {
		std::vector<u_char*> res;
		if (!parentPanel) return res;

		const auto children = parentPanel + CS2::PanoramaUIPanel::children;
		if (!children) return res;

		for (int i = 0; i < *(int*)children; ++i) {
			const auto panel = ((u_char***)children)[1][i];
			auto panel2D = *(CPanel2D**)(panel + 0x8);
			if (!panel2D) continue;

			auto panelClassName = panel2D->getClassName();

			if (strcmp(panelClassName, classNameToFind) == 0) {
				res.emplace_back(panel);
			};
		}

		for (int i = 0; i < *(int*)children; ++i) {
			const auto panel = ((u_char***)children)[1][i];
			const auto panelFlags = *(u_char *)(panel + CS2::PanoramaUIPanel::panelFlags);
			if ((panelFlags & CS2::PanoramaUIPanel::k_EPanelFlag_HasOwnLayoutFile) == 0) {
				auto found = findChildrenInLayoutFileByClassName(panel, classNameToFind);
				if (!found.empty()) {
					for (auto i : found) {
						res.emplace_back(i);
					}
				}
			}
		}

		return res;
	}

	u_char* findChildInLayoutFile(u_char* parentPanel, const char* idToFind){
		if (!parentPanel) return nullptr;

		const auto children = parentPanel + CS2::PanoramaUIPanel::children;
		if (!children) return nullptr;

		for (int i = 0; i < *(int*)children; ++i) {
			const auto panel = ((u_char***)children)[1][i];
			const auto panelId = *(char**)(panel + CS2::PanoramaUIPanel::panelId);
			if (!panelId) continue;
			if (strcmp(panelId, idToFind) == 0) {
				return panel;
			};
		}

		for (int i = 0; i < *(int*)children; ++i) {
			const auto panel = ((u_char***)children)[1][i];
			const auto panelFlags = *(u_char *)(panel + CS2::PanoramaUIPanel::panelFlags);
			if ((panelFlags & CS2::PanoramaUIPanel::k_EPanelFlag_HasOwnLayoutFile) == 0) {
				if (const auto found = findChildInLayoutFile(panel, idToFind)) {
					return found;
				}
			}
		}

		return nullptr;
	};

	void applyColors() {
		if (nullptr != BorderColor.pointer) {
			*(uint32_t*)(BorderColor.pointer + 0x38 + 4*0) = BorderColor.use ? BorderColor.value : BorderColor.defaultValue;
			*(uint32_t*)(BorderColor.pointer + 0x38 + 4*1) = BorderColor.use ? BorderColor.value : BorderColor.defaultValue;
			*(uint32_t*)(BorderColor.pointer + 0x38 + 4*2) = BorderColor.use ? BorderColor.value : BorderColor.defaultValue;
			*(uint32_t*)(BorderColor.pointer + 0x38 + 4*3) = BorderColor.use ? BorderColor.value : BorderColor.defaultValue;
		}
		if (nullptr != LocalBackgroundColor.pointer) {
			*(uint32_t*)(LocalBackgroundColor.pointer +0x20) = LocalBackgroundColor.use ? LocalBackgroundColor.value : LocalBackgroundColor.defaultValue;
		}	
		if (nullptr != BackgroundColor.pointer) {
			*(uint32_t*)(BackgroundColor.pointer +0x20) = BackgroundColor.use ? BackgroundColor.value : BackgroundColor.defaultValue;
		}	
		if (nullptr != CTcolor.pointer) {
			*(uint32_t*)(CTcolor.pointer + 0x20) = CTcolor.use ? CTcolor.value : CTcolor.defaultValue;
		}
		if (nullptr != Tcolor.pointer) {
			*(uint32_t*)(Tcolor.pointer + 0x20) = Tcolor.use ? Tcolor.value : Tcolor.defaultValue;
		}
	}

	/*
	void updateHudPanelStyles() {
		// currently not required.
		const auto hudPanel = ((u_char***)pHudPanel)[0][1];
		if (hudPanel) {
			// Function is called also in if after refrence to "CUIPanel::AddClassesInternal - apply old dirty styles":
			// and is also at vtable entry 71 of panorama panel class.
			void (__fastcall * applyStyleFn)(void *, signed short int) = (void (__fastcall *)(void *, signed short int))((*(void***)hudPanel)[71]);
			applyStyleFn(hudPanel, 0);
		}
	}
	*/

} g_myPanoramaWrapper;

void * MirvPanorama_FindChildInLayoutFile(void * parentPanel, const char * panelId)
{
	return g_myPanoramaWrapper.findChildInLayoutFile(
		reinterpret_cast<u_char *>(parentPanel),
		panelId);
}

#if AFX_MIRV_POV_DIAGNOSTICS

CON_COMMAND(__mirv_panorama_print_children, "") {
	const auto arg0 = args->ArgV(0);
	int argc = args->ArgC();

	if (2 <= argc)
	{
		const char * arg1 = args->ArgV(1);
		g_myPanoramaWrapper.printChildren(arg1);
	} else {
		g_myPanoramaWrapper.printChildren("");
	}
}

#endif

static int DeathMsg_ResolveEntityUserId(CEntityInstance * controller)
{
	if (nullptr == controller) return -1;
	int entityIndex = -1;
	__try {
		entityIndex = controller->GetHandle().GetEntryIndex();
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		entityIndex = -1;
	}
	return 0 < entityIndex ? entityIndex - 1 : -1;
}


class MyDeathMsgGameEventWrapper : public SOURCESDK::CS2::IGameEvent, public MyDeathMsgGameEventWrapperBase
{
public:
	MyDeathMsgGameEventWrapper(
		SOURCESDK::CS2::IGameEvent * event,
		bool allowDeathMsgOverrides = true)
	: m_Event(event)
	, m_AllowDeathMsgOverrides(allowDeathMsgOverrides) { }

	// The native player_death handler decides whether to show the local death
	// banner from the event's userid -> controller -> pawn chain. In mirv_pov
	// the event userid belongs to the watched player, while the native local
	// pawn is the real split-screen player. Keep this remap on the temporary
	// wrapper only; never mutate the game's event or entity state.
	void SetNativeLocalVictimRemap(
		SOURCESDK::CS2::CEntityInstance * controller,
		SOURCESDK::CS2::CEntityInstance * pawn,
		int userId)
	{
		nativeLocalVictimController = controller;
		nativeLocalVictimPawn = pawn;
		nativeLocalVictimUserId = userId;

		nativeLocalVictimRemap =
			nativeLocalVictimUserId >= 0
			|| nullptr != nativeLocalVictimController
			|| nullptr != nativeLocalVictimPawn;
	}

	SOURCESDK::CS2::CKV3MemberName hashString(const char * string) const {
		size_t len = strlen(string);
		unsigned int hash = g_MirvPovHashString(string, (unsigned int)len, (unsigned int)len ^ 0x31415926);
		return SOURCESDK::CS2::CKV3MemberName((int)hash, -1, string);
	}

		bool IsHashStringEqual(const char * a, const SOURCESDK::CS2::CKV3MemberName & b) const {
		return IsHashEqual(hashString(a),b);
	}

		bool IsNativeVictimPawnKey(const SOURCESDK::CS2::GameEventKeySymbol_t & keySymbol) const {
			return nativeLocalVictimRemap
				&& nullptr != nativeLocalVictimPawn
				&& IsHashStringEqual("userid", keySymbol);
		}

		bool IsNativeVictimKey(const SOURCESDK::CS2::GameEventKeySymbol_t & keySymbol) const {
			return nativeLocalVictimRemap && IsHashStringEqual("userid", keySymbol);
		}

		SOURCESDK::CS2::CEntityHandle GetNativeControllerHandle() const {
			if (nullptr == nativeLocalVictimController) return SOURCESDK::CS2::CEntityHandle();
			return reinterpret_cast<::CEntityInstance *>(nativeLocalVictimController)->GetHandle();
		}

		SOURCESDK::CS2::CEntityHandle GetNativePawnHandle() const {
			if (nullptr == nativeLocalVictimPawn) return SOURCESDK::CS2::CEntityHandle();
			return reinterpret_cast<::CEntityInstance *>(nativeLocalVictimPawn)->GetHandle();
		}

private:
		bool IsHashEqual(const SOURCESDK::CS2::CKV3MemberName & a, const SOURCESDK::CS2::CKV3MemberName & b) const {
		return a.GetHashCode() == b.GetHashCode();
	}

public:
	virtual ~MyDeathMsgGameEventWrapper() {};
	virtual const char *GetName() const {
		return m_Event->GetName();
	}
	virtual int GetID() const {
		return m_Event->GetID();
	}
	virtual bool IsReliable() const {
		return m_Event->IsReliable();
	}
	virtual bool IsLocal() const {
		return m_Event->IsLocal();
	}
	virtual bool IsEmpty( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
		return m_Event->IsEmpty(keySymbol);
	}
	virtual bool GetBool( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol) {
		return m_Event->GetBool(keySymbol);
	}
	virtual int GetInt( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol) {
		if (nativeLocalVictimRemap && nativeLocalVictimUserId >= 0
			&& IsHashStringEqual("userid", keySymbol)) {
			return nativeLocalVictimUserId;
		}
		if (m_AllowDeathMsgOverrides) {
		if (assistedflash.use && IsHashStringEqual("assistedflash", keySymbol)) return assistedflash.value;
		if (headshot.use && IsHashStringEqual("headshot", keySymbol)) return headshot.value;
		if (penetrated.use && IsHashStringEqual("penetrated", keySymbol)) return penetrated.value;
		if (dominated.use && IsHashStringEqual("dominated", keySymbol)) return dominated.value;
		if (revenge.use && IsHashStringEqual("revenge", keySymbol)) return revenge.value;
		if (wipe.use && IsHashStringEqual("wipe", keySymbol)) return wipe.value;
		if (noscope.use && IsHashStringEqual("noscope", keySymbol)) return noscope.value;
		if (thrusmoke.use && IsHashStringEqual("thrusmoke", keySymbol)) return thrusmoke.value;
		if (attackerblind.use && IsHashStringEqual("attackerblind", keySymbol)) return attackerblind.value;
		if (attackerinair.use && IsHashStringEqual("attackerinair", keySymbol)) return attackerinair.value;
		}
		
		return m_Event->GetInt(keySymbol);
	}
	virtual uint64_t GetUint64( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol) {
		return m_Event->GetUint64(keySymbol);
	}
	virtual float GetFloat( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol) {
		return m_Event->GetFloat(keySymbol);
	}
	virtual const char *GetString( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol) {
			if (m_AllowDeathMsgOverrides && weapon.use && IsHashStringEqual("weapon", keySymbol)) return weapon.value;
		return m_Event->GetString(keySymbol);
	}
	virtual void *GetPtr( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
		return m_Event->GetPtr(keySymbol);
	}
	virtual SOURCESDK::CS2::CEntityHandle GetEHandle( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
			if (IsNativeVictimKey(keySymbol) && nullptr != nativeLocalVictimController) {
				return reinterpret_cast<::CEntityInstance *>(nativeLocalVictimController)->GetHandle();
			}
		return m_Event->GetEHandle(keySymbol);
	}
	virtual SOURCESDK::CS2::CEntityInstance *GetEntity( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol) {
			if (IsNativeVictimPawnKey(keySymbol)) {
				return nativeLocalVictimPawn;
			}
		return m_Event->GetEntity(keySymbol);
	}
	virtual void* GetEntityIndex( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
		return m_Event->GetEntityIndex(keySymbol);
	}
	virtual SOURCESDK::CS2::CPlayerSlot GetPlayerSlot( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
			if (nativeLocalVictimRemap && nativeLocalVictimUserId >= 0
				&& IsHashStringEqual("userid", keySymbol)) {
				return SOURCESDK::CS2::CPlayerSlot(nativeLocalVictimUserId);
			}
			if (m_AllowDeathMsgOverrides) {
		if (attacker.newId.use && IsHashStringEqual("attacker", keySymbol)) return SOURCESDK::CS2::CPlayerSlot(attacker.newId.value.ResolveToUserId());
		if (victim.newId.use && IsHashStringEqual("userid", keySymbol)) return SOURCESDK::CS2::CPlayerSlot(victim.newId.value.ResolveToUserId());
		if (assister.newId.use && IsHashStringEqual("assister", keySymbol)) return SOURCESDK::CS2::CPlayerSlot(assister.newId.value.ResolveToUserId());
			}

		return m_Event->GetPlayerSlot(keySymbol);
	}
	virtual SOURCESDK::CS2::CEntityInstance *GetPlayerController( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
			if (nativeLocalVictimRemap && nullptr != nativeLocalVictimController
				&& IsHashStringEqual("userid", keySymbol)) {
				return nativeLocalVictimController;
			}
		return m_Event->GetPlayerController(keySymbol);
	}
	virtual SOURCESDK::CS2::CEntityInstance *GetPlayerPawn( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
			if (IsNativeVictimPawnKey(keySymbol)) {
				return nativeLocalVictimPawn;
			}
		return m_Event->GetPlayerPawn(keySymbol);
	}
	virtual SOURCESDK::CS2::CEntityHandle GetPawnEHandle( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
			if (IsNativeVictimPawnKey(keySymbol)) {
				return reinterpret_cast<::CEntityInstance *>(nativeLocalVictimPawn)->GetHandle();
			}
		return m_Event->GetPawnEHandle(keySymbol);
	}
	virtual void* GetPawnEntityIndex( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
		return m_Event->GetPawnEntityIndex(keySymbol);
	}
	virtual void SetBool( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, bool value ) {
		m_Event->SetBool(keySymbol,value);
	}
	virtual void SetInt( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, int value ) {
		m_Event->SetInt(keySymbol,value);
	}
	virtual void SetUint64( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, uint64_t value ) {
		m_Event->SetUint64(keySymbol,value);
	}
	virtual void SetFloat( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, float value ) {
		m_Event->SetFloat(keySymbol,value);
	}
	virtual void SetString( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, const char *value ) {
		m_Event->SetString(keySymbol,value);
	}
	virtual void SetPtr( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, void *value ) {
		m_Event->SetPtr(keySymbol,value);
	}
	virtual void SetEntity(const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, SOURCESDK::CS2::CEntityInstance *value) {
		m_Event->SetEntity(keySymbol,value);
	}
	virtual void SetEntity( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, void* value ) {
		m_Event->SetEntity(keySymbol,value);
	}
	virtual void SetPlayer( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, SOURCESDK::CS2::CEntityInstance *pawn ) {
		m_Event->SetPlayer(keySymbol,pawn);
	}
	virtual void SetPlayer( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol, SOURCESDK::CS2::CPlayerSlot value ) {
		m_Event->SetPlayer(keySymbol,value);
	}
	virtual void SetPlayerRaw( const SOURCESDK::CS2::GameEventKeySymbol_t &controllerKeySymbol, const SOURCESDK::CS2::GameEventKeySymbol_t &pawnKeySymbol, SOURCESDK::CS2::CEntityInstance *pawn ) {
		m_Event->SetPlayerRaw(controllerKeySymbol,pawnKeySymbol,pawn);
	}
	virtual bool HasKey( const SOURCESDK::CS2::GameEventKeySymbol_t &keySymbol ) {
		return m_Event->HasKey(keySymbol);
	}
	virtual void CreateVMTable( void* &Table ) {
		m_Event->CreateVMTable(Table);
	}
	virtual struct SOURCESDK::CS2::KeyValues3* GetDataKeys() const {
		return m_Event->GetDataKeys();
	}

private:
	SOURCESDK::CS2::IGameEvent * m_Event;
			SOURCESDK::CS2::CEntityInstance * nativeLocalVictimController = nullptr;
			SOURCESDK::CS2::CEntityInstance * nativeLocalVictimPawn = nullptr;
			int nativeLocalVictimUserId = -1;
			bool nativeLocalVictimRemap = false;
			bool m_AllowDeathMsgOverrides = true;
};

struct CS2_MirvDeathMsgGlobals : MirvDeathMsgGlobals {
	bool hooked = false; 
	bool deathNoticeHooked = false;
	bool localTokenHooked = false;
} g_MirvDeathMsgGlobals;

// The native DeathPanel handler synchronously queries the temporary event
// through the local-token helper. Keep this state thread-local and restore it
// with RAII so an exception or an early return can never leave a dangling
// pointer to the stack wrapper behind.
thread_local MyDeathMsgGameEventWrapper * g_ActiveDeathMsgWrapper = nullptr;

class DeathMsgActiveWrapperGuard {
public:
	 explicit DeathMsgActiveWrapperGuard(MyDeathMsgGameEventWrapper * wrapper)
	 : m_Previous(g_ActiveDeathMsgWrapper) {
		g_ActiveDeathMsgWrapper = wrapper;
	}

	~DeathMsgActiveWrapperGuard() {
		g_ActiveDeathMsgWrapper = m_Previous;
	}

	DeathMsgActiveWrapperGuard(const DeathMsgActiveWrapperGuard &) = delete;
	DeathMsgActiveWrapperGuard & operator=(const DeathMsgActiveWrapperGuard &) = delete;

private:
	MyDeathMsgGameEventWrapper * m_Previous;
};

static bool DeathMsg_ShouldProcessCustomPath() {
	return nullptr != g_ActiveDeathMsgWrapper
		|| g_MirvDeathMsgGlobals.Settings.Debug != 0
		|| !g_MirvDeathMsgGlobals.Filter.empty()
		|| g_MirvDeathMsgGlobals.Lifetime.use
		|| g_MirvDeathMsgGlobals.LifetimeMod.use
		|| g_MirvDeathMsgGlobals.useHighlightId
		|| g_MirvDeathMsgGlobals.showNumbers != MirvDeathMsgGlobals::DeathnoticeShowNumbers_e::Default;
}

static bool DeathMsg_ShouldProcessLocalTokenPath() {
	return nullptr != g_ActiveDeathMsgWrapper
		|| g_MirvDeathMsgGlobals.useHighlightId;
}

static bool DeathMsg_ShouldProcessPanoramaPath() {
	return MirvPov_IsEnabled()
		|| g_myPanoramaWrapper.BorderColor.use
		|| g_myPanoramaWrapper.BackgroundColor.use
		|| g_myPanoramaWrapper.LocalBackgroundColor.use
		|| g_myPanoramaWrapper.CTcolor.use
		|| g_myPanoramaWrapper.Tcolor.use;
}

// The native helper found in the current client is sub_180CC9F70. Despite
// the old name, it does not return a SteamID/XUID: the caller compares its
// zero-extended result with the entity index returned by GetEntityIndex.
	typedef uint32_t (__fastcall *g_Original_getLocalSteamId_t)(void* param_1);
g_Original_getLocalSteamId_t g_Original_getLocalSteamId = nullptr;

static int ResolveDeathMsgEntityIndex(const DeathMsgId & id) {
	switch (id.Mode) {
	case DeathMsgId::Id_UserId:
		// CS2 player userid is the player-controller entity index minus one.
		return 0 <= id.Id.userId ? id.Id.userId + 1 : 0;

	case DeathMsgId::Id_Key:
	case DeathMsgId::Id_Xuid:
		{
			int highestIndex = GetHighestEntityIndex();
			for (int i = 0; i <= highestIndex; ++i) {
				auto ent = (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, i);
				if (nullptr == ent || !ent->IsPlayerController()) continue;

				auto player = getPlayerInfoFromControllerIndex(i);
				if (DeathMsgId::Id_Key == id.Mode && player.specKey == id.Id.specKey)
					return i;
				if (DeathMsgId::Id_Xuid == id.Mode && player.xuid == id.Id.xuid)
					return i;
			}
		}
		return 0;
	}

	return 0;
}

uint32_t __fastcall getLocalSteamId(void* param_1) {
	// getLocalSteamId is queried repeatedly by the native death-notice UI.
	// Keep the global hook a transparent pass-through until POV/deathmsg
	// customization is actually active; this removes the per-frame work and
	// prevents ordinary spectator mode from touching stale custom state.
		if(!DeathMsg_ShouldProcessLocalTokenPath()) {
		return nullptr != g_Original_getLocalSteamId
			? g_Original_getLocalSteamId(param_1)
			: 0;
	}

	MyDeathMsgPlayerEntry entry;
	bool overrideLocal = false;
	if (nullptr != g_ActiveDeathMsgWrapper) {
		auto & wrapper = *g_ActiveDeathMsgWrapper;

		// Prefer the entry whose override is actually true. The old code chose
		// attacker merely because its override was present, then returned zero
		// before it could consider a local victim.
		if (wrapper.attacker.isLocal.use && wrapper.attacker.isLocal.value) {
			entry = wrapper.attacker;
		}
		else if (wrapper.victim.isLocal.use && wrapper.victim.isLocal.value) {
			entry = wrapper.victim;
		}
			else if (wrapper.assister.isLocal.use && wrapper.assister.isLocal.value) {
				entry = wrapper.assister;
		}
				overrideLocal = true;
	}

	if (g_MirvDeathMsgGlobals.useHighlightId) {
		entry.newId.value = g_MirvDeathMsgGlobals.highlightId;
		entry.isLocal.value = true;
		overrideLocal = true;
	}

	if (overrideLocal) {
		uint32_t result = static_cast<uint32_t>(ResolveDeathMsgEntityIndex(entry.newId.value));
		return result;
	}

	if (nullptr == g_Original_getLocalSteamId) return 0;
	uint32_t result = g_Original_getLocalSteamId(param_1);
	return result;
};

// RCX is the CGameEventListener subobject at CCSGO_HudDeathPanel + 0x20.
// RDX is IGameEvent. The current client listener has a real two-argument ABI.
static u_char * __fastcall handleDeathnotice(
	u_char * hudDeathNotice,
	SOURCESDK::CS2::IGameEvent * gameEvent);

// Mode 1 preserves the game's listener -> player_death handler -> UI chain,
// substitutes the POV context, and completes the native DeathPanel show stage.
// The latter is required because the listener fills the death data but does
// not itself guarantee that DeathPanelRoot/DeathPanel are unhidden in POV.
static CEntityInstance * DeathPanel_ResolveEventVictimPawn(
	SOURCESDK::CS2::IGameEvent * event,
	const SOURCESDK::CS2::GameEventKeySymbol_t & userIdKey,
	SOURCESDK::CS2::CEntityInstance * victimController)
{
	if(nullptr == event) return nullptr;
	__try {
		auto isPlayerPawn = [](SOURCESDK::CS2::CEntityInstance * entity) {
			return nullptr != entity
				&& reinterpret_cast<CEntityInstance *>(entity)->IsPlayerPawn();
		};

		SOURCESDK::CS2::CEntityInstance * eventPawn = event->GetPlayerPawn(userIdKey);
		if(isPlayerPawn(eventPawn)) {
			return reinterpret_cast<CEntityInstance *>(eventPawn);
		}

		auto eventPawnHandle = event->GetPawnEHandle(userIdKey);
		if(eventPawnHandle.IsValid()) {
			CEntityInstance * pawn = GetEntityFromIndex(eventPawnHandle.GetEntryIndex());
			if(nullptr != pawn && pawn->IsPlayerPawn()) return pawn;
		}

		CEntityInstance * controller = reinterpret_cast<CEntityInstance *>(victimController);
		if(nullptr != controller && controller->IsPlayerController()) {
			auto controllerPawnHandle = controller->GetPlayerPawnHandle();
			if(controllerPawnHandle.IsValid()) {
				CEntityInstance * pawn = GetEntityFromIndex(controllerPawnHandle.GetEntryIndex());
				if(nullptr != pawn && pawn->IsPlayerPawn()) return pawn;
			}
		}
	} __except(EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

static CEntityInstance * __fastcall DeathPanel_GetLocalPawn(int slot)
		{
	if(nullptr != g_MirvPovDeathPanelLocalPawnOverride && (0 == slot || -1 == slot)) {
			if(false) {
			advancedfx::Message(
				"[mirv_pov_feedback] native DeathPanel local-pawn override slot=%d pawn=%p\n",
				slot,
				g_MirvPovDeathPanelLocalPawnOverride);
		}
		return g_MirvPovDeathPanelLocalPawnOverride;
	}
	return nullptr != g_MirvPovDeathPanelState.originalGetLocalPawn
		? g_MirvPovDeathPanelState.originalGetLocalPawn(slot)
		: nullptr;
}

class DeathPanelLocalPawnOverrideGuard {
public:
	explicit DeathPanelLocalPawnOverrideGuard(CEntityInstance * pawn)
			: m_Previous(g_MirvPovDeathPanelLocalPawnOverride)
			{
			g_MirvPovDeathPanelLocalPawnOverride = pawn;
		}

	~DeathPanelLocalPawnOverrideGuard()
				{
		g_MirvPovDeathPanelLocalPawnOverride = m_Previous;
	}

private:
	CEntityInstance * m_Previous;
};

struct DeathPanelReplayGateState {
	unsigned char * value = nullptr;
	unsigned char previous = 0;
	bool changed = false;
};

static DeathPanelReplayGateState DeathPanel_EnableReplayOthersGate()
					{
	DeathPanelReplayGateState state;
	if(nullptr == g_MirvPovDeathPanelState.resolveReplayValue || nullptr == g_MirvPovDeathPanelState.replayObject) return state;

	__try {
		state.value = g_MirvPovDeathPanelState.resolveReplayValue(g_MirvPovDeathPanelState.replayObject, -1);
		if(nullptr == state.value
			&& nullptr != g_MirvPovDeathPanelState.replayFallbackObject
			&& nullptr != *g_MirvPovDeathPanelState.replayFallbackObject) {
			state.value = *reinterpret_cast<unsigned char **>(
				reinterpret_cast<unsigned char *>(*g_MirvPovDeathPanelState.replayFallbackObject) + 8);
		}
		if(nullptr != state.value) {
			state.previous = *state.value;
			*state.value = 1;
			state.changed = true;
		}
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		state = DeathPanelReplayGateState();
	}
	return state;
					}
					
static void DeathPanel_RestoreReplayOthersGate(const DeathPanelReplayGateState & state)
{
	if(!state.changed || nullptr == state.value) return;
	__try {
		*state.value = state.previous;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
				}
			}

class DeathPanelReplayGateGuard {
public:
	explicit DeathPanelReplayGateGuard(bool enable)
		: m_State(enable ? DeathPanel_EnableReplayOthersGate() : DeathPanelReplayGateState())
	{
		}

	~DeathPanelReplayGateGuard()
		{
		DeathPanel_RestoreReplayOthersGate(m_State);
		}

	bool Changed() const { return m_State.changed; }
	unsigned char Previous() const { return m_State.previous; }

private:
	DeathPanelReplayGateState m_State;
	};

struct DeathPanelModeResult {
	u_char * handlerResult = nullptr;
	unsigned int actionMask = 0;
	unsigned long handlerException = 0;
	unsigned long showException = 0;
	unsigned long visibilityException = 0;
};

static u_char * DeathPanel_GetPanel(u_char * listenerSubobject)
{
	return nullptr != listenerSubobject ? listenerSubobject - 0x20 : nullptr;
}

static bool DeathPanel_InvokeFullShow(
	u_char * deathPanel,
	unsigned long & exceptionCode)
{
	exceptionCode = 0;
	if(nullptr == deathPanel || nullptr == g_MirvPovDeathPanelState.show) return false;

	// MSVC forbids a C++ object with a non-trivial destructor in a function
	// containing __try/__except (C2712). Save/restore the thread-local override
	// explicitly so the native getter sees the real pawn for the retry without
	// weakening the SEH boundary around the client call.
	CEntityInstance * previousLocalPawnOverride = g_MirvPovDeathPanelLocalPawnOverride;
	bool succeeded = false;
	__try {
		g_MirvPovDeathPanelState.show(deathPanel);
		succeeded = true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		exceptionCode = GetExceptionCode();
		}
	g_MirvPovDeathPanelLocalPawnOverride = previousLocalPawnOverride;
	return succeeded;
}

static void DeathPanel_MarkTouched(u_char * deathPanel)
{
	if(nullptr == deathPanel) return;
	if(MirvPov_IsEnabled()
		&& MirvPov_IsDeathFeedbackEnabled()
		&& nullptr != g_MirvPovDeathPanelLocalPawnOverride) {
		g_MirvPovDeathPanelState.reapplyPanel = deathPanel;
		g_MirvPovDeathPanelState.reapplyPawn = g_MirvPovDeathPanelLocalPawnOverride;
		g_MirvPovDeathPanelState.reapplyPawnHandle = 0xFFFFFFFFu;
		__try {
			auto pawnHandle = g_MirvPovDeathPanelLocalPawnOverride->GetHandle();
			if(pawnHandle.IsValid()) g_MirvPovDeathPanelState.reapplyPawnHandle = pawnHandle.ToInt();
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
		g_MirvPovDeathPanelState.reapplyFrame = g_MirvTime.framecount_get();
		g_MirvPovDeathPanelState.lastRefreshFrame = -1;
		g_MirvPovDeathPanelState.reapplyArmed = true;
		if(false) {
			advancedfx::Message(
				"[mirv_pov_feedback] DeathPanel lifetime armed panel=%p pawn=%p pawnHandle=0x%08x "
				"frame=%d curtime=%.3f\n",
				deathPanel,
				g_MirvPovDeathPanelLocalPawnOverride,
				static_cast<unsigned int>(g_MirvPovDeathPanelState.reapplyPawnHandle),
				g_MirvPovDeathPanelState.reapplyFrame,
				g_MirvTime.curtime_get());
		}
	}
	}


static u_char * InvokeDeathNoticeHandler(
	u_char * hudDeathNotice,
	SOURCESDK::CS2::IGameEvent * gameEvent,
	unsigned long & exceptionCode)
{
	exceptionCode = 0;
	if (nullptr == g_MirvPovDeathPanelState.originalHandlePlayerDeath) {
		exceptionCode = ERROR_PROC_NOT_FOUND;
		return nullptr;
	}

	__try {
		return g_MirvPovDeathPanelState.originalHandlePlayerDeath(hudDeathNotice, gameEvent);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		exceptionCode = GetExceptionCode();
		return nullptr;
	}
}

static u_char * __fastcall DeathPanel_Construct(
	u_char * deathPanel)
{
	u_char * result = nullptr;
	unsigned long exceptionCode = 0;
	__try {
		result = nullptr != g_MirvPovDeathPanelState.originalConstructor
			? g_MirvPovDeathPanelState.originalConstructor(deathPanel)
			: deathPanel;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		exceptionCode = GetExceptionCode();
	}

	if(0 == exceptionCode && nullptr != result) {
		g_MirvPovDeathPanelState.nativeInstance = result;
		if(false) {
			advancedfx::Message(
				"[mirv_pov_feedback] DeathPanel native constructor captured "
				"panel=%p listener=%p.\n",
				result,
				result + 0x20);
		}
	} else if(false) {
		advancedfx::Warning(
			"[mirv_pov_feedback] DeathPanel native constructor failed "
			"panel=%p result=%p exception=0x%08lx.\n",
			deathPanel,
			result,
			exceptionCode);
	}
	return result;
}

static u_char * __fastcall DeathPanel_Destruct(
	u_char * deathPanel,
	unsigned int deleteFlags)
{
	const bool captured = deathPanel == g_MirvPovDeathPanelState.nativeInstance;
	u_char * result = nullptr;
	unsigned long exceptionCode = 0;
	__try {
		result = nullptr != g_MirvPovDeathPanelState.originalDestructor
			? g_MirvPovDeathPanelState.originalDestructor(deathPanel, deleteFlags)
			: deathPanel;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		exceptionCode = GetExceptionCode();
	}

	if(captured) {
		g_MirvPovDeathPanelState.nativeInstance = nullptr;
		if(deathPanel == g_MirvPovDeathPanelState.lastPanel) g_MirvPovDeathPanelState.lastPanel = nullptr;
	}
	if(false) {
		advancedfx::Message(
			"[mirv_pov_feedback] DeathPanel native destructor panel=%p "
				"flags=0x%x captured=%d result=%p exception=0x%08lx.\n",
			deathPanel,
			deleteFlags,
			captured ? 1 : 0,
			result,
			exceptionCode);
	}
	return result;
}

static DeathPanelModeResult DeathPanel_RunMode(
	u_char * listenerSubobject,
	SOURCESDK::CS2::IGameEvent * listenerEvent)
{
	DeathPanelModeResult result;

	u_char * deathPanel = DeathPanel_GetPanel(listenerSubobject);
	if(nullptr == deathPanel || nullptr == listenerEvent) {
		return result;
	}

	result.handlerResult = InvokeDeathNoticeHandler(
		listenerSubobject,
		listenerEvent,
		result.handlerException);
	result.actionMask |= DeathPanelAction_Listener;
	if(0 != result.handlerException) return result;
	if(DeathPanel_InvokeFullShow(deathPanel, result.showException)) {
		result.actionMask |= DeathPanelAction_FullShow;
	}
	DeathPanel_ForceVisibility(deathPanel, result.actionMask, result.visibilityException);

	const unsigned int panelMutationActions =
		DeathPanelAction_FullShow
		| DeathPanelAction_RemoveHiddenClass
		| DeathPanelAction_MainVisible
		| DeathPanelAction_NativeVisibilityFallback
			| DeathPanelAction_SecondaryVisible;
	if(0 != (result.actionMask & panelMutationActions)) {
		DeathPanel_MarkTouched(deathPanel);
	}
	return result;
}

static bool DeathPanel_TryResolvePovVictim(
	SOURCESDK::CS2::IGameEvent * gameEvent,
	SOURCESDK::CS2::CEntityInstance *& victimController,
	CEntityInstance *& victimPawn,
	unsigned long & exceptionCode)
{
	victimController = nullptr;
	victimPawn = nullptr;
	exceptionCode = 0;
	bool povVictim = false;

	__try {
		povVictim = MirvPovFeedback_IsLocalPlayerVictim(gameEvent);
		if(povVictim && nullptr != g_MirvPovHashString) {
			const char * userIdName = "userid";
			const size_t userIdLength = strlen(userIdName);
				SOURCESDK::CS2::CKV3MemberName userIdKey(
					static_cast<int>(g_MirvPovHashString(
						userIdName,
						static_cast<unsigned int>(userIdLength),
						static_cast<unsigned int>(userIdLength) ^ 0x31415926)),
					-1,
					userIdName);
				const int userId = gameEvent->GetInt(userIdKey);
				victimController = gameEvent->GetPlayerController(userIdKey);
				if(nullptr == victimController && 0 <= userId) {
					SOURCESDK::CS2::CEntityInstance * fallback =
						reinterpret_cast<SOURCESDK::CS2::CEntityInstance *>(
							GetEntityFromIndex(userId + 1));
					if(nullptr != fallback
						&& reinterpret_cast<CEntityInstance *>(fallback)->IsPlayerController()) {
						victimController = fallback;
					}
				}
				victimPawn = DeathPanel_ResolveEventVictimPawn(
					gameEvent,
					userIdKey,
				victimController);
		}
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		exceptionCode = GetExceptionCode();
		povVictim = false;
		victimController = nullptr;
		victimPawn = nullptr;
	}
	return povVictim;
}

static bool DeathPanel_TryGetEventUserId(
	SOURCESDK::CS2::IGameEvent * gameEvent,
	int & userId)
{
	userId = -1;
	if(nullptr == gameEvent || nullptr == g_MirvPovHashString) return false;

	__try {
		const char * userIdName = "userid";
		const size_t userIdLength = strlen(userIdName);
		SOURCESDK::CS2::CKV3MemberName userIdKey(
			static_cast<int>(g_MirvPovHashString(
				userIdName,
				static_cast<unsigned int>(userIdLength),
				static_cast<unsigned int>(userIdLength) ^ 0x31415926)),
			-1,
			userIdName);
		userId = gameEvent->GetInt(userIdKey);
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		userId = -1;
		return false;
	}
}

static SOURCESDK::CS2::CEntityInstance * DeathPanel_TryGetEventPawn(
	SOURCESDK::CS2::IGameEvent * gameEvent)
{
	if(nullptr == gameEvent || nullptr == g_MirvPovHashString) return nullptr;

	SOURCESDK::CS2::CEntityInstance * eventPawn = nullptr;
	__try {
		const char * userIdName = "userid";
		const size_t userIdLength = strlen(userIdName);
		SOURCESDK::CS2::CKV3MemberName userIdKey(
			static_cast<int>(g_MirvPovHashString(
				userIdName,
				static_cast<unsigned int>(userIdLength),
				static_cast<unsigned int>(userIdLength) ^ 0x31415926)),
			-1,
			userIdName);
		eventPawn = gameEvent->GetPlayerPawn(userIdKey);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		eventPawn = nullptr;
	}
	return eventPawn;
}

static u_char * HandleNativePovDeathPanel(
	u_char * listenerSubobject,
	SOURCESDK::CS2::IGameEvent * gameEvent)
{
	bool povVictim = false;
	CEntityInstance * victimPawn = nullptr;
	SOURCESDK::CS2::CEntityInstance * victimController = nullptr;
	unsigned long inspectExceptionCode = 0;
	povVictim = DeathPanel_TryResolvePovVictim(
		gameEvent,
		victimController,
		victimPawn,
		inspectExceptionCode);

	// Pure POV mode must never wrap IGameEvent. The current CS2 GetString ABI
	// no longer matches the old SDK declaration, and forwarding that return
	// value through MyDeathMsgGameEventWrapper produced the 0x6e00 pointer seen
	// in the August 9 crash dump. Let the game's listener consume its own event
	// object and override only the local Pawn during this synchronous call.
	if(!povVictim || nullptr == victimPawn) {
		unsigned long handlerExceptionCode = 0;
		u_char * result = InvokeDeathNoticeHandler(
			listenerSubobject,
			gameEvent,
			handlerExceptionCode);
		if(false
			&& (povVictim || 0 != inspectExceptionCode || 0 != handlerExceptionCode)) {
			advancedfx::Message(
				"[mirv_pov_feedback] native DeathPanel passthrough povVictim=%d victimPawn=%p inspectException=0x%08lx handlerException=0x%08lx\n",
				povVictim ? 1 : 0,
				victimPawn,
				inspectExceptionCode,
				handlerExceptionCode);
		}
		return result;
	}

	DeathPanelLocalPawnOverrideGuard localPawnOverrideGuard(victimPawn);
	// sub_180E02E60 returns before writing weapon_name/other_player_name when
	// off_18205A9D0+0x10 is non-zero. The previous POV workaround forced this
	// gate to 1 and therefore disabled the native banner content path itself.
	// Keep the game's original gate unchanged; the local Pawn override below is
	// sufficient for the native victim comparison.
	DeathPanelReplayGateGuard replayGate(false);

		// sub_180E02E60 only creates/shows the native banner when the event's
		// GetPlayerPawn("userid") is the same object as GetLocalPlayerPawn(0).
		// Keep the real game event whenever it already resolves to the POV pawn;
		// the native handler relies on more than the three accessors exposed by
		// the old SDK wrapper. Only use the short-lived proxy when the event pawn
		// is actually missing or stale.
			// POV-only remapping must preserve every real player_death field,
			// including headshot. DeathMsg filters remain enabled in the separate
			// non-POV customization path below.
			MyDeathMsgGameEventWrapper povEvent(gameEvent, false);
		SOURCESDK::CS2::IGameEvent * nativeEvent = gameEvent;
		bool eventRemapped = false;
		bool eventPawnMatches = false;
		SOURCESDK::CS2::CEntityInstance * eventPawn = nullptr;
		int victimUserId = -1;
		if(DeathPanel_TryGetEventUserId(gameEvent, victimUserId)) {
			eventPawn = DeathPanel_TryGetEventPawn(gameEvent);
			eventPawnMatches =
				nullptr != eventPawn
				&& reinterpret_cast<void *>(eventPawn)
					== reinterpret_cast<void *>(victimPawn);

			if(!eventPawnMatches) {
				povEvent.SetNativeLocalVictimRemap(
					victimController,
					reinterpret_cast<SOURCESDK::CS2::CEntityInstance *>(victimPawn),
					victimUserId);
				nativeEvent = &povEvent;
				eventRemapped = true;
			}
		}

		if(false) {
			advancedfx::Message(
				"[mirv_pov_feedback] native DeathPanel event remap=%d event=%p proxy=%p "
				"userid=%d eventPawn=%p eventPawnMatches=%d controller=%p pawn=%p\n",
				eventRemapped ? 1 : 0,
				gameEvent,
				eventRemapped ? static_cast<void *>(&povEvent) : nullptr,
				victimUserId,
				eventPawn,
				eventPawnMatches ? 1 : 0,
				victimController,
				victimPawn);
	}

	DeathMsgActiveWrapperGuard activeWrapperGuard(eventRemapped ? &povEvent : nullptr);
	DeathPanelModeResult modeResult = DeathPanel_RunMode(
		listenerSubobject,
		nativeEvent);
	return modeResult.handlerResult;
}

static bool DeathPanel_IsPlayerDeathEvent(
	SOURCESDK::CS2::IGameEvent * gameEvent,
	unsigned long & exceptionCode)
{
	exceptionCode = 0;
	if(nullptr == gameEvent) return false;
	__try {
		const char * name = gameEvent->GetName();
		return nullptr != name && 0 == strcmp(name, "player_death");
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		exceptionCode = GetExceptionCode();
		return false;
	}
}

u_char * __fastcall handleDeathnotice(
	u_char * hudDeathNotice,
	SOURCESDK::CS2::IGameEvent * gameEvent)
{

	if (nullptr == g_MirvPovDeathPanelState.originalHandlePlayerDeath) {
		return nullptr;
	}

	g_MirvPovDeathPanelState.lastPanel = DeathPanel_GetPanel(hudDeathNotice);
	// A listener callback is a second exact source of the native owner. This
	// also covers builds where the HUD panel was constructed before our ctor hook
	// was installed.
	if(nullptr != g_MirvPovDeathPanelState.lastPanel) g_MirvPovDeathPanelState.nativeInstance = g_MirvPovDeathPanelState.lastPanel;

	// This listener receives several event types. Only player_death belongs to
	// the POV DeathPanel experiments; every other event must remain a byte-for-
	// byte native pass-through (player_spawn previously triggered a false show).
	unsigned long eventNameExceptionCode = 0;
	if(!DeathPanel_IsPlayerDeathEvent(gameEvent, eventNameExceptionCode)) {
		unsigned long passThroughExceptionCode = 0;
		u_char * result = InvokeDeathNoticeHandler(
			hudDeathNotice,
			gameEvent,
			passThroughExceptionCode);
			return result;
	}

	// Every POV DeathPanel mode stays on the game's native IGameEvent object.
	// The old wrapper's GetString ABI is not compatible with the current game
	// and previously produced the 0x6e00 crash. Explicit mirv_deathmsg
	// transformations therefore remain available only outside mirv_pov.
	if(MirvPov_IsEnabled() && MirvPov_IsDeathFeedbackEnabled()) {
		u_char * result = HandleNativePovDeathPanel(hudDeathNotice, gameEvent);
		return result;
	}

	// Preserve the native handler exactly when neither mirv_pov nor any
	// mirv_deathmsg customization is active. This is the common path during
	// normal spectator playback and avoids all wrapper/entity/panel access.
	if(!DeathMsg_ShouldProcessCustomPath()) {
			unsigned long passThroughExceptionCode = 0;
			u_char * result = InvokeDeathNoticeHandler(
				hudDeathNotice, gameEvent, passThroughExceptionCode);
				return result;
		}

		// The temporary event wrapper needs the client hash helper. If its
		// pattern is unavailable, preserve the native player_death path instead
		// of dereferencing a null function pointer while building key symbols.
		if (nullptr == g_MirvPovHashString) {
				unsigned long passThroughExceptionCode = 0;
				u_char * result = InvokeDeathNoticeHandler(
					hudDeathNotice, gameEvent, passThroughExceptionCode);
				return result;
		}

		bool lifetimeOffsetsReady =
		0 != AFXADDR_GET(cs2_deathmsg_lifetime_offset)
		&& 0 != AFXADDR_GET(cs2_deathmsg_lifetimemod_offset);
	uint8_t lifetimeOffset = 0;
	uint8_t lifetimeModOffset = 0;
	float *pDeathNoticeLifetime = nullptr;
	float *pDeathNoticeLocalPlayerLifetimeMod = nullptr;
	float orgDeathNoticeLifetime = 0.0f;
	float orgDeathNoticeLocalPlayerLifetimeMod = 0.0f;

	if (lifetimeOffsetsReady) {
		lifetimeOffset = (uint8_t)AFXADDR_GET(cs2_deathmsg_lifetime_offset);
		lifetimeModOffset = (uint8_t)AFXADDR_GET(cs2_deathmsg_lifetimemod_offset);
		if (nullptr != hudDeathNotice) {
			pDeathNoticeLifetime = (float *)(hudDeathNotice + lifetimeOffset);
			pDeathNoticeLocalPlayerLifetimeMod = (float *)(hudDeathNotice + lifetimeModOffset);
		}
		}

		MyDeathMsgGameEventWrapper myWrapper(gameEvent);
		const auto weaponKey = myWrapper.hashString("weapon");
		const char * weaponName = gameEvent->GetString(weaponKey);
			if(nullptr == weaponName || '\0' == weaponName[0]) {
				// The native handler intentionally drops events without a weapon.
				// Preserve that contract before applying any POV overrides.
					unsigned long passThroughExceptionCode = 0;
					u_char * result = InvokeDeathNoticeHandler(
						hudDeathNotice, gameEvent, passThroughExceptionCode);
					if(0 != passThroughExceptionCode && false) {
					advancedfx::Warning(
						"[mirv_pov_feedback] native DeathNotice no-weapon pass-through "
						"exception code=0x%08lx\n",
						passThroughExceptionCode);
				}
					return result;
			}

	auto uidAttacker = (int)(int16_t)gameEvent->GetInt(myWrapper.hashString("attacker"));
	auto uidVictim = (int)(int16_t)gameEvent->GetInt(myWrapper.hashString("userid"));
	auto uidAssister = (int)(int16_t)gameEvent->GetInt(myWrapper.hashString("assister"));

	myWrapper.attacker.newId.value.Id.userId = uidAttacker;
	myWrapper.victim.newId.value.Id.userId = uidVictim;
	myWrapper.assister.newId.value.Id.userId = uidAssister;

	auto attackerController = gameEvent->GetPlayerController(myWrapper.hashString("attacker"));
	auto victimController = gameEvent->GetPlayerController(myWrapper.hashString("userid"));
	auto assisterController = gameEvent->GetPlayerController(myWrapper.hashString("assister"));
			const auto userIdKey = myWrapper.hashString("userid");
			auto victimPawn = DeathPanel_ResolveEventVictimPawn(
				gameEvent,
				userIdKey,
				victimController);

	if (g_MirvDeathMsgGlobals.Settings.Debug)
	{

		std::vector<std::vector<std::string>> rows = {
			{
				"weapon",
				"attackerName",
				"attackerUserId",
				"victimName",
				"victimUserId",
				"assisterName",
				"assisterUserId",
			},
			{
				gameEvent->GetString(myWrapper.hashString("weapon")),

				nullptr != attackerController ? (char*)((u_char*)attackerController + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName) : "null",
				std::to_string(uidAttacker),

				nullptr != victimController ? (char*)((u_char*)victimController + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName) : "null",
				std::to_string(uidVictim),

				nullptr != assisterController ? (char*)((u_char*)assisterController + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName) : "null",
				std::to_string(uidAssister),
			}
		};

		advancedfx::Message(
			"player_death\n"
			"%s", afxUtils::createTable(rows, " | ", "-").c_str()
		);

	}

	for(std::list<DeathMsgFilterEntry>::iterator it = g_MirvDeathMsgGlobals.Filter.begin(); it != g_MirvDeathMsgGlobals.Filter.end(); it++)
	{
		DeathMsgFilterEntry & e = *it;

		bool attackerBlocked;
		switch(e.attacker.mode)
		{
		case DMBM_ANY:
			attackerBlocked = true;
			break;
		case DMBM_EXCEPT:
			attackerBlocked = !e.attacker.id.EqualsUserId(uidAttacker);
			break;
		case DMBM_EQUAL:
		default:
			attackerBlocked = e.attacker.id.EqualsUserId(uidAttacker);
			break;
		}

		bool victimBlocked;
		switch(e.victim.mode)
		{
		case DMBM_ANY:
			victimBlocked = true;
			break;
		case DMBM_EXCEPT:
			victimBlocked = !e.victim.id.EqualsUserId(uidVictim);
			break;
		case DMBM_EQUAL:
		default:
			victimBlocked = e.victim.id.EqualsUserId(uidVictim);
			break;
		}

		bool assisterBlocked;
		switch(e.assister.mode)
		{
		case DMBM_ANY:
			assisterBlocked = true;
			break;
		case DMBM_EXCEPT:
			assisterBlocked = !e.assister.id.EqualsUserId(uidAssister);
			break;
		case DMBM_EQUAL:
		default:
			assisterBlocked = e.assister.id.EqualsUserId(uidAssister);
			break;
		}

		bool matched = attackerBlocked && victimBlocked && assisterBlocked;

		if(matched)
		{
			myWrapper.ApplyDeathMsgFilterEntry(e);

			uidAttacker = myWrapper.GetInt(myWrapper.hashString("attacker"));
			uidVictim = myWrapper.GetInt(myWrapper.hashString("userid"));
			uidAssister = myWrapper.GetInt(myWrapper.hashString("assister"));

			if (e.lastRule) break;
		}
	}

	if (myWrapper.block.use && myWrapper.block.value) {
			return nullptr;
	}

	// The native death notice decides which entry is "local" through its
	// local-SteamID helper. In POV mode the watched player is not the real
	// split-screen player, so expose the watched controller as local while the
	// original Panorama handler processes this event.
		if (MirvPov_IsEnabled()) {
			auto povController = GetCurrentPovPlayerController();
			if (nullptr == povController) {
				povController = GetObservedPlayerController();
			}
		if (nullptr != povController) {
			myWrapper.attacker.isLocal.use = true;
			myWrapper.attacker.isLocal.value = reinterpret_cast<void *>(attackerController) == reinterpret_cast<void *>(povController);
			myWrapper.victim.isLocal.use = true;
			myWrapper.victim.isLocal.value = reinterpret_cast<void *>(victimController) == reinterpret_cast<void *>(povController);
			myWrapper.assister.isLocal.use = true;
			myWrapper.assister.isLocal.value = reinterpret_cast<void *>(assisterController) == reinterpret_cast<void *>(povController);
			}
	}

	if (g_MirvDeathMsgGlobals.useHighlightId)
	{
		myWrapper.attacker.isLocal.use = true;
		myWrapper.attacker.isLocal.value = g_MirvDeathMsgGlobals.highlightId.EqualsUserId(uidAttacker);

		myWrapper.victim.isLocal.use = true;
		myWrapper.victim.isLocal.value = g_MirvDeathMsgGlobals.highlightId.EqualsUserId(uidVictim);

		myWrapper.assister.isLocal.use = true;
		myWrapper.assister.isLocal.value = g_MirvDeathMsgGlobals.highlightId.EqualsUserId(uidAssister);
	}

	if (myWrapper.attacker.name.use && nullptr != attackerController) {
		((SOURCESDK::CS2::CUtlString *)((u_char*)attackerController + g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName))->Set(myWrapper.attacker.name.value);
	}

	if (myWrapper.victim.name.use && nullptr != victimController) {
		((SOURCESDK::CS2::CUtlString *)((u_char*)victimController + g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName))->Set(myWrapper.victim.name.value);
	}

	if (myWrapper.assister.name.use && nullptr != assisterController) {
		((SOURCESDK::CS2::CUtlString *)((u_char*)assisterController + g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName))->Set(myWrapper.assister.name.value);
	}

	if (g_MirvDeathMsgGlobals.Lifetime.use)
	{
		myWrapper.lifetime.use = true;
		myWrapper.lifetime.value = g_MirvDeathMsgGlobals.Lifetime.value;
	}

	if (g_MirvDeathMsgGlobals.LifetimeMod.use)
	{
		myWrapper.lifetimeMod.use = true;
		myWrapper.lifetimeMod.value = g_MirvDeathMsgGlobals.LifetimeMod.value;
	}

	if (lifetimeOffsetsReady && nullptr != pDeathNoticeLifetime && myWrapper.lifetime.use)
	{
		orgDeathNoticeLifetime = *pDeathNoticeLifetime;
		*pDeathNoticeLifetime = myWrapper.lifetime.value;
	}

	if (lifetimeOffsetsReady && nullptr != pDeathNoticeLocalPlayerLifetimeMod && myWrapper.lifetimeMod.use)
	{
		orgDeathNoticeLocalPlayerLifetimeMod = *pDeathNoticeLocalPlayerLifetimeMod;
		*pDeathNoticeLocalPlayerLifetimeMod = myWrapper.lifetimeMod.value;
	}

				const bool povVictim = MirvPov_IsEnabled()
					&& MirvPov_IsDeathFeedbackEnabled()
					&& MirvPovFeedback_IsLocalPlayerVictim(gameEvent);
				CEntityInstance * deathPanelPovPawn = povVictim ? victimPawn : nullptr;
					if(povVictim && nullptr == deathPanelPovPawn) {
						deathPanelPovPawn = GetCurrentPovPlayerPawn();
					}
					if(povVictim) {
						// Keep the original event values, but guarantee that the native
						// DeathPanel listener resolves userid to the same Pawn returned by
						// its temporarily overridden local-player getter.
						myWrapper.SetNativeLocalVictimRemap(
							victimController,
							reinterpret_cast<SOURCESDK::CS2::CEntityInstance *>(deathPanelPovPawn),
							-1);
					}

					DeathMsgActiveWrapperGuard activeWrapperGuard(&myWrapper);
					DeathPanelLocalPawnOverrideGuard localPawnOverrideGuard(deathPanelPovPawn);
					DeathPanelReplayGateGuard replayGate(povVictim);

					unsigned long handlerExceptionCode = 0;
					u_char * result = nullptr;
					DeathPanelModeResult modeResult;
					if(povVictim) {
						modeResult = DeathPanel_RunMode(
							hudDeathNotice,
							&myWrapper);
						result = modeResult.handlerResult;
						handlerExceptionCode = modeResult.handlerException;
					} else {
						result = InvokeDeathNoticeHandler(
							hudDeathNotice,
							&myWrapper,
							handlerExceptionCode);
					}
		if (lifetimeOffsetsReady && nullptr != pDeathNoticeLocalPlayerLifetimeMod && myWrapper.lifetimeMod.use) {
		*pDeathNoticeLocalPlayerLifetimeMod = orgDeathNoticeLocalPlayerLifetimeMod;
	}
		if (lifetimeOffsetsReady && nullptr != pDeathNoticeLifetime && myWrapper.lifetime.use) {
		*pDeathNoticeLifetime = orgDeathNoticeLifetime;
	}

		return result;
};


typedef int (__fastcall * Panorama_CLayoutFile_LoadFromFile_t)(void * This, const char * pFilePath, unsigned char _unk02);
typedef unsigned char (__fastcall * Panorama_CStyleProperty_Parse_t)(void * This, void* _unk01, const char * pValueStr);
typedef void (__fastcall * Panorama_CStyleProperty_Clone_t)(void * This, void * pTarget);

bool g_b_In_Panorama_CLayoutFile_LoadFromFile = false;
bool g_b_In_Panorama_CLayoutFile_LoadFromFile_HudReticle = false;

Panorama_CLayoutFile_LoadFromFile_t g_Org_Panorama_CLayoutFile_LoadFromFile = nullptr;
Panorama_CStyleProperty_Parse_t g_Org_Panorama_CStylePropertyForegroundColor_Parse = nullptr;
Panorama_CStyleProperty_Parse_t g_Org_Panorama_CStylePropertyBackgroundColor_Parse = nullptr;
Panorama_CStyleProperty_Parse_t g_Org_Panorama_CStylePropertyBorder_Parse = nullptr;

Panorama_CStyleProperty_Parse_t g_Org_Panorama_CStylePropertyWashColor_Parse = nullptr;
Panorama_CStyleProperty_Clone_t g_Org_Panorama_CStylePropertyWashColor_Clone = nullptr;

std::set<u_char*> g_pHudReticle_WashColor_T;
std::set<u_char*> g_pHudReticle_WashColor_CT;

void SetHudReticleWashColorT(uint32_t value) {
	for(auto it= g_pHudReticle_WashColor_T.begin(); it != g_pHudReticle_WashColor_T.end(); it++) {
		*(uint32_t*)(*it + 0x10) = value;
	}
}

void SetHudReticleWashColorCT(uint32_t value) {
	for(auto it= g_pHudReticle_WashColor_CT.begin(); it != g_pHudReticle_WashColor_CT.end(); it++) {
		*(uint32_t*)(*it + 0x10) = value;
	}
}

int __fastcall My_Panorama_CLayoutFile_LoadFromFile(void * This, const char * pFilePath, unsigned char _unk02) {
	if(nullptr == pFilePath) {
		return g_Org_Panorama_CLayoutFile_LoadFromFile(This,pFilePath,_unk02);
	}
	if(!DeathMsg_ShouldProcessPanoramaPath()) {
		return g_Org_Panorama_CLayoutFile_LoadFromFile(This,pFilePath,_unk02);
	}

	if(0 == strcmp("panorama\\layout\\hud\\huddeathnotice.xml",pFilePath)) {		
		g_b_In_Panorama_CLayoutFile_LoadFromFile = true;
		int result = g_Org_Panorama_CLayoutFile_LoadFromFile(This,pFilePath,_unk02);
		g_b_In_Panorama_CLayoutFile_LoadFromFile = false;
		return result;
	}

	if(0 == strcmp("panorama\\layout\\hud\\hudreticle.xml",pFilePath)) {
		g_pHudReticle_WashColor_T.clear();
		g_pHudReticle_WashColor_CT.clear();
		g_b_In_Panorama_CLayoutFile_LoadFromFile_HudReticle = true;
		int result = g_Org_Panorama_CLayoutFile_LoadFromFile(This,pFilePath,_unk02);
		g_b_In_Panorama_CLayoutFile_LoadFromFile_HudReticle = false;
		return result;
	}

	if(0 == strcmp("panorama\\layout\\hud\\hudhealthammocenter.xml",pFilePath)
		|| 0 == strcmp("panorama\\layout\\hud\\hudlegend.xml",pFilePath)) {
		int result = g_Org_Panorama_CLayoutFile_LoadFromFile(This,pFilePath,_unk02);
		MirvPovHud_OnPanoramaLayoutFileLoaded(pFilePath);
		return result;
	}

	return g_Org_Panorama_CLayoutFile_LoadFromFile(This,pFilePath,_unk02);
}

unsigned char __fastcall My_Panorama_CStylePropertyForegroundColor_Parse(void * This, void* _unk01, const char * pValueStr) {
	unsigned char result = g_Org_Panorama_CStylePropertyForegroundColor_Parse(This,_unk01,pValueStr);
	if(g_b_In_Panorama_CLayoutFile_LoadFromFile) {
		if(0 == strcmp(pValueStr,"#6f9ce6")) {
			g_myPanoramaWrapper.CTcolor.pointer = (u_char*)This;
		}
		else if(0 == strcmp(pValueStr,"#eabe54")) {
			g_myPanoramaWrapper.Tcolor.pointer = (u_char*)This;
		}		
	}
	return result;
}

unsigned char __fastcall My_Panorama_CStylePropertyBackgroundColor_Parse(void * This, void* _unk01, const char * pValueStr) {
	unsigned char result = g_Org_Panorama_CStylePropertyBackgroundColor_Parse(This,_unk01,pValueStr);
	if(g_b_In_Panorama_CLayoutFile_LoadFromFile) {
		if(0 == strcmp(pValueStr,"#000000a0")) {
			g_myPanoramaWrapper.BackgroundColor.pointer = (u_char*)This;
		}
		else if(0 == strcmp(pValueStr,"#000000e7")) {
			g_myPanoramaWrapper.LocalBackgroundColor.pointer = (u_char*)This;
		}		
	}
	return result;
}

unsigned char __fastcall My_Panorama_CStylePropertyBorder_Parse(void * This, void* _unk01, const char * pValueStr) {
	unsigned char result = g_Org_Panorama_CStylePropertyBorder_Parse(This,_unk01,pValueStr);
	if(g_b_In_Panorama_CLayoutFile_LoadFromFile) {
		if(0 == strcmp(pValueStr,"2px solid #e10000")) {
			g_myPanoramaWrapper.BorderColor.pointer = (u_char*)This;
		}
	}
	return result;
}


unsigned char __fastcall My_Panorama_CStylePropertyWashColor_Parse(void * This, void* _unk01, const char * pValueStr) {
	unsigned char result = g_Org_Panorama_CStylePropertyWashColor_Parse(This,_unk01,pValueStr);
	if(g_b_In_Panorama_CLayoutFile_LoadFromFile_HudReticle) {
		if(0 == strcmp(pValueStr,"rgb(150, 200, 250)")) {
			g_pHudReticle_WashColor_CT.emplace((u_char*)This);
		}
		else if(0 == strcmp(pValueStr,"#eabe54")) {
			g_pHudReticle_WashColor_T.emplace((u_char*)This);
		}		
	}
	return result;
}

void __fastcall My_Panorama_CStylePropertyWashColor_Clone(void * This, void * pTarget) {
	g_Org_Panorama_CStylePropertyWashColor_Clone(This, pTarget);
	if(g_b_In_Panorama_CLayoutFile_LoadFromFile_HudReticle) {
		auto itCT = g_pHudReticle_WashColor_CT.find((u_char*)This);
		if(itCT != g_pHudReticle_WashColor_CT.end()) {
			g_pHudReticle_WashColor_CT.emplace((u_char*)pTarget);
		}
		auto itT = g_pHudReticle_WashColor_T.find((u_char*)This);
		if(itT != g_pHudReticle_WashColor_T.end()) {
			g_pHudReticle_WashColor_T.emplace((u_char*)pTarget);
		}
	}
}

bool getPanoramaAddrsFromClient(HMODULE clientDll) {
	// credit https://github.com/danielkrupinski/Osiris
	
/* In the middle of big function with MULTIPLE (3+) references to "Attempted to cast panel '%s' to type '%s'" and multiple to "file://{images}/%s.png":
        }
LAB_1809a7daf:
        if (DAT_181fc46d8 != (longlong *)0x0) {
          local_230 = FUN_1809bf960; <-- next 2 sigs in this one!!!
          local_238 = lVar12;
          (**(code **)(*DAT_181fc46d8 + 0x120))(DAT_181fc46d8,DAT_181c51f4c,plVar11,&local_238);
        }
      }
LAB_1809a7de1
*/
/*
                             LAB_1809bf9e6                                   XREF[1]:     1809bf9d9(j)  
       1809bf9e6 48 8b 4f 08     MOV        RCX,qword ptr [RDI + 0x8]
       1809bf9ea 4c 8d 05        LEA        R8,[DAT_1814ed000]
                 0f d6 b2 00
       1809bf9f1 0f b7 12        MOVZX      EDX=>DAT_181e808b8,word ptr [RDX]
       1809bf9f4 48 8b 01        MOV        RAX,qword ptr [RCX]
       1809bf9f7 ff 90 d0        CALL       qword ptr [RAX + 0x8d0]
                 08 00 00
       1809bf9fd 48 8b f0        MOV        RSI,RAX
       1809bfa00 48 85 c0        TEST       RAX,RAX
*/
	if (auto addr = getAddress(clientDll,"48 8b 4f 08 4c 8d 05 ?? ?? ?? ?? 0f b7 12 48 8b 01 ff 90 ?? ?? ?? ?? 48 8b f0 48 85 c0"); addr != 0) {
		CS2::PanoramaUIPanel::getAttributeString = *(int32_t*)((unsigned char*)addr + 19);
	} else {
		return false;
	}

/*
                             LAB_1809bfa6a                                   XREF[1]:     1809bfa5d(j)  
       1809bfa6a 48 8b 4f 08     MOV        RCX,qword ptr [RDI + 0x8]
       1809bfa6e 4c 8d 05        LEA        R8,[DAT_1814ed000]
                 8b d5 b2 00
       1809bfa75 0f b7 13        MOVZX      EDX,word ptr [RBX]=>DAT_181e808b8
       1809bfa78 48 8b 01        MOV        RAX,qword ptr [RCX]
       1809bfa7b ff 90 00        CALL       qword ptr [RAX + 0x900]
                 09 00 00
       1809bfa81 b0 01           MOV        AL,0x1
       1809bfa83 e9 1a ff        JMP        LAB_1809bf9a2
                 ff ff
*/
	if (auto addr = getAddress(clientDll,"48 8b 4f 08 4c 8d 05 ?? ?? ?? ?? 0f b7 13 48 8b 01 ff 90 ?? ?? ?? ?? b0 01 e9 ?? ?? ?? ??"); addr != 0) {
		CS2::PanoramaUIPanel::setAttributeString = *(int32_t*)((unsigned char*)addr + 19);
	} else {
		return false;
	}

	// "Can\'t call Panorama Symbol constructor outside panorama.dll until UIEngine is i nitialized! Symbol: %s"
	if (auto addr = getAddress(clientDll,"48 8B 01 4C 8B C3 BA ?? ?? ?? ?? FF 90 ?? ?? ?? ?? 48 8B 5C 24 ?? 66 89 07"); addr != 0) {
		CS2::PanoramaUIEngine::makeSymbol = *(int32_t*)((unsigned char*)addr + 13);
	} else {
		return false;
	}

	// function has "file://{resources}/layout/hud/hud.xml" string and also references CCSGO_Hud vftable
	// hudpanel is DAT that param_1 assigned to     
	size_t g_HudPanel_addr = getAddress(clientDll, "48 89 86 ?? ?? ?? ?? 48 89 35 ?? ?? ?? ??");
	if (g_HudPanel_addr == 0) {
		return false;
	} else {
		g_HudPanel_addr += 10;
	};

	// function has CreatePanelWithCurrentContext string
	// engine is DAT that param_1 assigned to
	size_t g_CUIEngine_addr = getAddress(clientDll, "48 89 78 ?? 48 89 0D ?? ?? ?? ??");
	if (g_CUIEngine_addr == 0) {
		return false;
	} else {
		g_CUIEngine_addr += 7;
	};

	uint32_t g_HudPanel_offset;
	std::memcpy(&g_HudPanel_offset, (void*)(g_HudPanel_addr), sizeof(g_HudPanel_offset));
	g_myPanoramaWrapper.pHudPanel = (u_char**)(g_HudPanel_addr + g_HudPanel_offset + 4);
	MirvPanorama_SetHudPanel((void**)g_myPanoramaWrapper.pHudPanel);

	uint32_t g_CUIEngine_offset;
	std::memcpy(&g_CUIEngine_offset, (void*)(g_CUIEngine_addr), sizeof(g_CUIEngine_offset));
	g_myPanoramaWrapper.pUIEngine = (u_char**)(g_CUIEngine_addr + g_CUIEngine_offset + 4);
	MirvPanorama_SetUIEngine((void**)g_myPanoramaWrapper.pUIEngine);

	return true;
};

bool getPanoramaAddrs(HMODULE panoramaDll) {

	// Refernces "CLayoutFile::LoadFromFile" string.
	g_Org_Panorama_CLayoutFile_LoadFromFile = (Panorama_CLayoutFile_LoadFromFile_t)getAddress(panoramaDll,"48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 48 8D 05 ?? ?? ?? ?? 48 C7 45 D0 F4 03 00 00 48");
	if(nullptr == g_Org_Panorama_CLayoutFile_LoadFromFile) {
		return false;
	}

	{
		void **vtable = (void**)Afx::BinUtils::FindClassVtable(panoramaDll,".?AVCStylePropertyForegroundColor@panorama@@",0,0);
		if(nullptr == vtable) {
			return false;
		}
		g_Org_Panorama_CStylePropertyForegroundColor_Parse = (Panorama_CStyleProperty_Parse_t)vtable[6];
	}

	{
		void **vtable = (void**)Afx::BinUtils::FindClassVtable(panoramaDll,".?AVCStylePropertyBackgroundColor@panorama@@",0,0);
		if(nullptr == vtable) {
			return false;
		}
		g_Org_Panorama_CStylePropertyBackgroundColor_Parse = (Panorama_CStyleProperty_Parse_t)vtable[6];
	}

	{
		void **vtable = (void**)Afx::BinUtils::FindClassVtable(panoramaDll,".?AVCStylePropertyBorder@panorama@@",0,0);
		if(nullptr == vtable) {
			return false;
		}
		g_Org_Panorama_CStylePropertyBorder_Parse = (Panorama_CStyleProperty_Parse_t)vtable[6];
	}

	{
		void **vtable = (void**)Afx::BinUtils::FindClassVtable(panoramaDll,".?AVCStylePropertyWashColor@panorama@@",0,0);
		if(nullptr == vtable) {
			return false;
		}
		g_Org_Panorama_CStylePropertyWashColor_Clone = (Panorama_CStyleProperty_Clone_t)vtable[1];
		g_Org_Panorama_CStylePropertyWashColor_Parse = (Panorama_CStyleProperty_Parse_t)vtable[6];
	}		

		// Style-property discovery is optional. MirvPanorama_InitStyleProperties
		// uses ErrorBox on unsupported builds, so defer it until a POV/deathmsg
		// Panorama feature is actually active during startup.
		if (DeathMsg_ShouldProcessPanoramaPath()
			&& !MirvPanorama_InitStyleProperties(panoramaDll)) return false;

	return true;
};

void HookPanorama(HMODULE panoramaDll)
{
	if (g_myPanoramaWrapper.hooked) return;

	if (!getPanoramaAddrs(panoramaDll)) return;

	LONG transactionBeginResult = DetourTransactionBegin();
	LONG updateThreadResult = NO_ERROR;
	LONG layoutAttachResult = NO_ERROR;
	LONG foregroundAttachResult = NO_ERROR;
	LONG backgroundAttachResult = NO_ERROR;
	LONG borderAttachResult = NO_ERROR;
	LONG washCloneAttachResult = NO_ERROR;
	LONG washParseAttachResult = NO_ERROR;
	LONG panoramaTransactionResult = -1;

	if (NO_ERROR == transactionBeginResult) {
		updateThreadResult = DetourUpdateThread(GetCurrentThread());
	}

	const bool transactionReady =
		NO_ERROR == transactionBeginResult
		&& NO_ERROR == updateThreadResult;
	if (transactionReady) {
		layoutAttachResult = DetourAttach(
			&(PVOID&)g_Org_Panorama_CLayoutFile_LoadFromFile,
			My_Panorama_CLayoutFile_LoadFromFile);
		foregroundAttachResult = DetourAttach(
			&(PVOID&)g_Org_Panorama_CStylePropertyForegroundColor_Parse,
			My_Panorama_CStylePropertyForegroundColor_Parse);
		backgroundAttachResult = DetourAttach(
			&(PVOID&)g_Org_Panorama_CStylePropertyBackgroundColor_Parse,
			My_Panorama_CStylePropertyBackgroundColor_Parse);
		borderAttachResult = DetourAttach(
			&(PVOID&)g_Org_Panorama_CStylePropertyBorder_Parse,
			My_Panorama_CStylePropertyBorder_Parse);
		washCloneAttachResult = DetourAttach(
			&(PVOID&)g_Org_Panorama_CStylePropertyWashColor_Clone,
			My_Panorama_CStylePropertyWashColor_Clone);
		washParseAttachResult = DetourAttach(
			&(PVOID&)g_Org_Panorama_CStylePropertyWashColor_Parse,
			My_Panorama_CStylePropertyWashColor_Parse);

		const bool allAttachSucceeded =
			NO_ERROR == layoutAttachResult
			&& NO_ERROR == foregroundAttachResult
			&& NO_ERROR == backgroundAttachResult
			&& NO_ERROR == borderAttachResult
			&& NO_ERROR == washCloneAttachResult
			&& NO_ERROR == washParseAttachResult;
		panoramaTransactionResult = allAttachSucceeded
			? DetourTransactionCommit()
			: DetourTransactionAbort();
	} else if (NO_ERROR == transactionBeginResult) {
		// Begin succeeded but the current thread could not be enlisted. Close
		// the transaction and keep the optional Panorama path unhooked.
		panoramaTransactionResult = DetourTransactionAbort();
	}

	const bool panoramaHookSucceeded =
		transactionReady
		&& NO_ERROR == layoutAttachResult
		&& NO_ERROR == foregroundAttachResult
		&& NO_ERROR == backgroundAttachResult
		&& NO_ERROR == borderAttachResult
		&& NO_ERROR == washCloneAttachResult
		&& NO_ERROR == washParseAttachResult
		&& NO_ERROR == panoramaTransactionResult;
	if (!panoramaHookSucceeded) {
		if (false) {
			advancedfx::Warning(
				"[mirv_pov_feedback] Failed to detour optional panorama functions "
				"begin=%ld update=%ld layout=%ld foreground=%ld background=%ld "
				"border=%ld washClone=%ld washParse=%ld transaction=%ld.\n",
				transactionBeginResult,
				updateThreadResult,
				layoutAttachResult,
				foregroundAttachResult,
				backgroundAttachResult,
				borderAttachResult,
				washCloneAttachResult,
				washParseAttachResult,
				panoramaTransactionResult);
		}
		return;
	}

	g_myPanoramaWrapper.hooked = true;

	// Initialize POV-specific Panorama helpers after the DLL addresses are ready.
	MirvPov_OnPanoramaDllLoaded(panoramaDll);
};

void HookDeathMsg(HMODULE clientDll) {
	if (g_MirvDeathMsgGlobals.hooked) return;

    MirvPovDeathPanel_ResolveAddresses(clientDll);
    g_Original_getLocalSteamId = reinterpret_cast<g_Original_getLocalSteamId_t>(
        MirvPovDeathPanel_ResolveEntityTokenAddress(clientDll));
    if (nullptr == g_MirvPovDeathPanelState.originalHandlePlayerDeath) {
        if (false) {
            advancedfx::Warning(
                "[mirv_pov_feedback] required player_death handler pattern missing; "
                "no DeathMsg detour installed.\n");
        }
        return;
    }

    bool panoramaAddrsReady = getPanoramaAddrsFromClient(clientDll);
    if (false) {
        advancedfx::Message(
            "[mirv_pov_feedback] DeathMsg panorama addresses ready=%d\n",
            panoramaAddrsReady ? 1 : 0);
    }

    LONG transactionBeginResult = DetourTransactionBegin();
    LONG updateThreadResult = NO_ERROR;
    LONG handlerAttachResult = NO_ERROR;
    LONG tokenAttachResult = NO_ERROR;
    LONG localPawnAttachResult = NO_ERROR;
    LONG deathPanelHideAttachResult = NO_ERROR;
    LONG deathPanelConstructorAttachResult = NO_ERROR;
    LONG deathPanelDestructorAttachResult = NO_ERROR;
    LONG deathMsgTransactionResult = -1;
    const bool attachToken = nullptr != g_Original_getLocalSteamId;
    const bool attachLocalPawn = nullptr != g_MirvPovDeathPanelState.originalGetLocalPawn;
    const bool attachDeathPanelHide = nullptr != g_MirvPovDeathPanelState.hide;
    const bool attachDeathPanelConstructor = nullptr != g_MirvPovDeathPanelState.originalConstructor;
    const bool attachDeathPanelDestructor = nullptr != g_MirvPovDeathPanelState.originalDestructor;

    if (NO_ERROR == transactionBeginResult) {
        updateThreadResult = DetourUpdateThread(GetCurrentThread());
    }

    const bool transactionReady =
        NO_ERROR == transactionBeginResult
        && NO_ERROR == updateThreadResult;
    if (transactionReady) {
        handlerAttachResult = DetourAttach(
            &(PVOID&)g_MirvPovDeathPanelState.originalHandlePlayerDeath,
            handleDeathnotice);
        if(attachDeathPanelConstructor) {
            deathPanelConstructorAttachResult = DetourAttach(
                &(PVOID&)g_MirvPovDeathPanelState.originalConstructor,
                DeathPanel_Construct);
        }
        if(NO_ERROR == deathPanelConstructorAttachResult && attachDeathPanelDestructor) {
            deathPanelDestructorAttachResult = DetourAttach(
                &(PVOID&)g_MirvPovDeathPanelState.originalDestructor,
                DeathPanel_Destruct);
        }
        if (attachToken) {
            tokenAttachResult = DetourAttach(
                &(PVOID&)g_Original_getLocalSteamId,
                getLocalSteamId);
        }
		if(NO_ERROR == tokenAttachResult && attachLocalPawn) {
			localPawnAttachResult = DetourAttach(
				&(PVOID&)g_MirvPovDeathPanelState.originalGetLocalPawn,
				DeathPanel_GetLocalPawn);
		}
		if (NO_ERROR == tokenAttachResult && NO_ERROR == localPawnAttachResult && attachDeathPanelHide) {
			deathPanelHideAttachResult = DetourAttach(
				&(PVOID&)g_MirvPovDeathPanelState.hide,
				MirvPovDeathPanel_HideWhileAlive);
		}

        const bool allAttachSucceeded =
            NO_ERROR == handlerAttachResult
            && (!attachDeathPanelConstructor || NO_ERROR == deathPanelConstructorAttachResult)
            && (!attachDeathPanelDestructor || NO_ERROR == deathPanelDestructorAttachResult)
            && (!attachToken || NO_ERROR == tokenAttachResult)
            && (!attachLocalPawn || NO_ERROR == localPawnAttachResult)
            && (!attachDeathPanelHide || NO_ERROR == deathPanelHideAttachResult);
        deathMsgTransactionResult = allAttachSucceeded
            ? DetourTransactionCommit()
            : DetourTransactionAbort();
    } else if (NO_ERROR == transactionBeginResult) {
        deathMsgTransactionResult = DetourTransactionAbort();
    }

	const bool deathMsgHookSucceeded =
		transactionReady
		&& NO_ERROR == handlerAttachResult
		&& (!attachDeathPanelConstructor || NO_ERROR == deathPanelConstructorAttachResult)
		&& (!attachDeathPanelDestructor || NO_ERROR == deathPanelDestructorAttachResult)
		&& (!attachToken || NO_ERROR == tokenAttachResult)
		&& (!attachLocalPawn || NO_ERROR == localPawnAttachResult)
        && NO_ERROR == deathMsgTransactionResult;
    if (!deathMsgHookSucceeded) {
        if (false) {
			advancedfx::Warning(
					"[mirv_pov_feedback] DeathMsg detour failed begin=%ld update=%ld "
						"handler=%ld constructor=%ld destructor=%ld token=%ld localPawn=%ld hide=%ld transaction=%ld.\n",
				transactionBeginResult,
				updateThreadResult,
				handlerAttachResult,
				deathPanelConstructorAttachResult,
				deathPanelDestructorAttachResult,
				tokenAttachResult,
                localPawnAttachResult,
                deathPanelHideAttachResult,
                deathPanelConstructorAttachResult,
                deathPanelDestructorAttachResult,
                deathMsgTransactionResult);
        }
		return;
	}

    g_MirvDeathMsgGlobals.deathNoticeHooked = true;
    g_MirvDeathMsgGlobals.localTokenHooked = attachToken;
	g_MirvPovDeathPanelState.localPawnHooked = attachLocalPawn;
	g_MirvPovDeathPanelState.hideHooked = attachDeathPanelHide;
	g_MirvPovDeathPanelState.constructorHooked = attachDeathPanelConstructor;
	g_MirvPovDeathPanelState.destructorHooked = attachDeathPanelDestructor;
	g_MirvDeathMsgGlobals.hooked = true;
    if (false) {
        advancedfx::Message(
					"[mirv_pov_feedback] DeathMsg hooks installed notice=%d token=%d localPawn=%d hide=%d "
						"constructor=%d destructor=%d panorama=%d.\n",
            g_MirvDeathMsgGlobals.deathNoticeHooked ? 1 : 0,
            g_MirvDeathMsgGlobals.localTokenHooked ? 1 : 0,
					g_MirvPovDeathPanelState.localPawnHooked ? 1 : 0,
					g_MirvPovDeathPanelState.hideHooked ? 1 : 0,
					g_MirvPovDeathPanelState.constructorHooked ? 1 : 0,
					g_MirvPovDeathPanelState.destructorHooked ? 1 : 0,
            panoramaAddrsReady ? 1 : 0);
    }
};

void deathMsgId_PrintHelp_Console(const char * cmd)
{
	advancedfx::Message(
		"%s accepts the following as <id...>:\n"
		"<iNumber> - UserID. Example: 9\n"
		"x<iNumber> - XUID. Example: x76561198106931330\n"
		"k<iNumber> - Spectator key number.\n"
		"trace - UserID from a screen trace (e.g. current POV).\n"
		"xTrace - XUID from a screen trace (e.g. current POV).\n"
		"We recommend getting the numbers from the output of \"mirv_deathmsg help players\".\n"
		, cmd
	);
};

void deathMsgPlayers_PrintHelp_Console()
{
    int highestIndex = GetHighestEntityIndex();

	std::vector<std::vector<std::string>> rows = {
		{"name", "userid", "xuid", "speckey"}, {}
	};

    for(int i = 0; i < highestIndex + 1; i++) {
        if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i)) {
			if(!ent->IsPlayerController()) continue;

			auto teamNumber = *(int*)((u_char*)(ent) + g_clientDllOffsets.C_BaseEntity.m_iTeamNum);
			if (0 == teamNumber || 1 == teamNumber) continue;

			// I know it's nested loop, but on this scale it doesn't matter
			auto playerInfo = getPlayerInfoFromControllerIndex(i);

			rows.push_back({
				playerInfo.name,
				std::to_string(playerInfo.userId), // apparently in CS2 userid is playercontroller entityindex - 1
				std::string("x").append(std::to_string(playerInfo.xuid)),
				std::string("k").append(std::to_string(playerInfo.specKey))
			});
        }
    }

	if (rows.size() == 2) return;

	advancedfx::Message(
		"%s", afxUtils::createTable(rows, " | ", "-").c_str()
	);
};

struct CS2_MirvDeathMsg : MirvDeathMsg {
	bool colors (IWrpCommandArgs * args) 
	{
		int argc = args->ArgC();
		const char * arg0 = args->ArgV(0);

		std::string colors = "";

		for (int i = 0; i < afxBasicColors.size(); i++)
		{
			auto color = afxBasicColors[i];
			colors.append(color.name);
			if (i < afxBasicColors.size() - 1) colors.append(", ");
		}

		const char* options = 
			"Where <option> is one of:\n"
			"default - use default game color\n"
			"<0-255> <0-255> <0-255> <0-255> - color in RGBA format e.g. 255 0 0 255\n"
			"<color> - one of the default colors e.g. red\n";

		if (3 > argc)
		{
			advancedfx::Message(
				"%s colors ct <option> - Control CT color.\n"
				"%s colors t <option> - Control T color.\n"
				"%s colors border <option> - Control border color of local player.\n"
				"%s colors background <option> - Control background color.\n"
				"%s colors backgroundLocal <option> - Control background color of local player.\n"
				"\n"
				"%s"
				"\n"
				"Available colors:\n"
				"%s\n"
				, arg0, arg0, arg0, arg0, arg0, options, colors.c_str()
			);
			return true;	
		}

		const char* arg2 = args->ArgV(2);
		
		if (0 == _stricmp("ct", arg2))
		{
			if (3 == argc)
			{
				advancedfx::Message(
					"%s colors %s <option> - Control CT color in death messages.\n"
					"Current value: %s\n"
					, arg0, arg2
					, g_myPanoramaWrapper.CTcolor.use ? g_myPanoramaWrapper.CTcolor.userValue.c_str() : "default"
				);
				return true;
			}

			if (4 == argc)
			{
				g_myPanoramaWrapper.CTcolor.setColor(args->ArgV(3));
				g_myPanoramaWrapper.applyColors();
				return true;
			}

			if (7 == argc)
			{
				advancedfx::CSubCommandArgs subArgs(args, 3);
				g_myPanoramaWrapper.CTcolor.setColor(&subArgs);
				g_myPanoramaWrapper.applyColors();
				return true;
			}
		}

		if (0 == _stricmp("t", arg2))
		{
			if (3 == argc)
			{
				advancedfx::Message(
					"%s colors %s <option> - Control T color in death messages.\n"
					"Current value: %s\n"
					, arg0, arg2
					, g_myPanoramaWrapper.Tcolor.use ? g_myPanoramaWrapper.Tcolor.userValue.c_str() : "default"
				);
				return true;
			}

			if (4 == argc)
			{
				g_myPanoramaWrapper.Tcolor.setColor(args->ArgV(3));
				g_myPanoramaWrapper.applyColors();
				return true;
			}

			if (7 == argc)
			{
				advancedfx::CSubCommandArgs subArgs(args, 3);
				g_myPanoramaWrapper.Tcolor.setColor(&subArgs);
				g_myPanoramaWrapper.applyColors();
				return true;
			}
		}

		if (0 == _stricmp("border", arg2))
		{
			if (3 == argc)
			{
				advancedfx::Message(
					"%s colors %s <option> - Control border color of local player in death messages.\n"
					"Current value: %s\n"
					, arg0, arg2
					, g_myPanoramaWrapper.BorderColor.use ? g_myPanoramaWrapper.BorderColor.userValue.c_str() : "default"
				);
				return true;
			}

			if (4 == argc)
			{
				g_myPanoramaWrapper.BorderColor.setColor(args->ArgV(3));
				g_myPanoramaWrapper.applyColors();
				return true;
			}

			if (7 == argc)
			{
				advancedfx::CSubCommandArgs subArgs(args, 3);
				g_myPanoramaWrapper.BorderColor.setColor(&subArgs);
				g_myPanoramaWrapper.applyColors();
				return true;
			}
		}

		if (0 == _stricmp("background", arg2))
		{
			if (3 == argc)
			{
				advancedfx::Message(
					"%s colors %s <option> - Control background color of death messages.\n"
					"Current value: %s\n"
					, arg0, arg2
					, g_myPanoramaWrapper.BackgroundColor.use ? g_myPanoramaWrapper.BackgroundColor.userValue.c_str() : "default"
				);

				return true;
			}

			if (4 == argc)
			{
				g_myPanoramaWrapper.BackgroundColor.setColor(args->ArgV(3));
				g_myPanoramaWrapper.applyColors();
				return true;
			}

			if (7 == argc)
			{
				advancedfx::CSubCommandArgs subArgs(args, 3);
				g_myPanoramaWrapper.BackgroundColor.setColor(&subArgs);
				g_myPanoramaWrapper.applyColors();
				return true;
			}
		}

		if (0 == _stricmp("backgroundLocal", arg2))
		{
			if (3 == argc)
			{
				advancedfx::Message(
					"%s colors %s <option> - Control background color of local player.\n"
					"Current value: %s\n"
					, arg0, arg2
					, g_myPanoramaWrapper.LocalBackgroundColor.use ? g_myPanoramaWrapper.LocalBackgroundColor.userValue.c_str() : "default"
				);

				return true;
			}

			if (4 == argc)
			{
				g_myPanoramaWrapper.LocalBackgroundColor.setColor(args->ArgV(3));
				g_myPanoramaWrapper.applyColors();
				return true;
			}

			if (7 == argc)
			{
				advancedfx::CSubCommandArgs subArgs(args, 3);
				g_myPanoramaWrapper.LocalBackgroundColor.setColor(&subArgs);
				g_myPanoramaWrapper.applyColors();
				return true;
			}
		}

		advancedfx::Message(
			"%s colors ct <option> - Control CT color.\n"
			"%s colors t <option> - Control T color.\n"
			"%s colors border <option> - Control border color of local player.\n"
			"%s colors background <option> - Control background color.\n"
			"%s colors backgroundLocal <option> - Control background color of local player.\n"
			"\n"
			"%s"
			"\n"
			"Available colors:\n"
			"%s\n"
			, arg0, arg0, arg0, arg0, arg0, options, colors.c_str()
		);
		return true;
	};
} g_MirvDeathMsg;

bool mirvDeathMsg_Console(advancedfx::ICommandArgs* args)
{
	const auto arg0 = args->ArgV(0);
	int argc = args->ArgC();

	if (2 <= argc)
	{
		const char * arg1 = args->ArgV(1);
		if (0 == _stricmp("clear", arg1))
		{
			auto result = g_myPanoramaWrapper.clearDeathnotices();
			return true;
		} else
		if (0 == _stricmp("filter", arg1))
		{
			return g_MirvDeathMsg.filter(args, g_MirvDeathMsgGlobals);
		} else
		if (0 == _stricmp("lifetime", arg1)) {
			return g_MirvDeathMsg.lifetime(args, g_MirvDeathMsgGlobals);
		} else
		if (0 == _stricmp("lifetimeMod", arg1)) {
			return g_MirvDeathMsg.lifetimeMod(args, g_MirvDeathMsgGlobals);
		} else
		if (0 == _stricmp("localPlayer", arg1)) {
			return g_MirvDeathMsg.localPlayer(args, g_MirvDeathMsgGlobals);
		} else
		if (0 == _stricmp("debug", arg1))
		{
			return g_MirvDeathMsg.debug(args, g_MirvDeathMsgGlobals);
		} else
		if (0 == _stricmp("colors", arg1)) {
			return g_MirvDeathMsg.colors(args);
		}
		if (0 == _stricmp("help", arg1))
		{
			if (3 <= argc)
			{
				const char * arg2 = args->ArgV(2);

				if (0 == _stricmp("id", arg2))
				{
					deathMsgId_PrintHelp_Console(arg0);
					return true;
				}

				if (0 == _stricmp("players", arg2))
				{
					deathMsgPlayers_PrintHelp_Console();
					return true;
				}

			}
			advancedfx::Message(
				"%s help id - Print help on <id...> usage.\n"
				"%s help players - Print available player ids.\n"
				, arg0, arg0, arg0
			);
			return true;
		}
	}

	advancedfx::Message(
		"%s clear - Clears all deathnotices.\n"
		"%s filter [...] - Filter death messages.\n"
		"%s lifetime [...] - Controls lifetime of death messages.\n"
		"%s lifetimeMod [...] - Controls lifetime modifier of death messages for the \"local\" player.\n"
		"%s localPlayer [...] - Controls what is considered \"local\" player (and thus highlighted in death notices).\n"
		"%s debug [...] - Enable / Disable debug spew upon death messages.\n"
		"%s colors [...] - Controls colors of death messages.\n"
		"%s help [...] - Print help.\n"
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
	);
	return true;
};

CON_COMMAND(mirv_deathmsg, "controls death notification options")
{
	mirvDeathMsg_Console(args);
};

enum panelMatchType {
	ID = 0,
	CLASS_NAME
};

void applyStyleProperty_Console(IWrpCommandArgs * args) {
	int argc = args->ArgC();
	const char * arg0 = args->ArgV(0);

	panelMatchType matchType = panelMatchType::ID;
	std::string panelId = "";
	// TODO: match by property type, when add new ones
	bool didMatchProperty = false;
	float opacity = 0;

	for (int i = 1; i < argc; ++i)
	{
		const char * argI = args->ArgV(i);
		if (StringIBeginsWith(argI, "panelId="))
		{
			panelId = argI + strlen("panelId=");
			matchType = panelMatchType::ID;
		}
		else if (StringIBeginsWith(argI, "panelClassName="))
		{
			panelId = argI + strlen("panelClassName=");
			matchType = panelMatchType::CLASS_NAME;
		}
		else if (StringIBeginsWith(argI, "opacity="))
		{
			opacity = float(atof(argI + strlen("opacity=")));
			didMatchProperty = true;
		}
	}

	if (panelId.empty()) {
		advancedfx::Warning("PanelId cannot be empty.\n");
		return;
	}

	if (!didMatchProperty) {
		advancedfx::Warning("Did not match any style property.\n");
		return;
	}

	auto parentPanel = ((u_char***)g_myPanoramaWrapper.pHudPanel)[0][1];
	if (!parentPanel) {
		advancedfx::Warning("Root panel is 0\n");
		return;
	}


	if (matchType == panelMatchType::ID) {
		u_char* targetPanel = g_myPanoramaWrapper.findChildInLayoutFile(parentPanel, panelId.c_str());

		if (0 == targetPanel) {
			advancedfx::Warning("Could not find panel %s\n", panelId.c_str());
			return;
		}

		auto res = Panorama_SetPanelOpacity(targetPanel, std::clamp(opacity, 0.0f, 1.0f));
		if (!res) {
			advancedfx::Warning("Could not set opacity property for %s\n", panelId.c_str());
		}
	} else if (matchType == panelMatchType::CLASS_NAME) {
		auto foundPanels = g_myPanoramaWrapper.findChildrenInLayoutFileByClassName(parentPanel, panelId.c_str());
		if (foundPanels.empty()) {
			advancedfx::Warning("Could not find panels with className %s\n", panelId.c_str());
		} else {
			for (auto panel : foundPanels) {
				Panorama_SetPanelOpacity(panel, std::clamp(opacity, 0.0f, 1.0f));
			}
		}
	}
}

CON_COMMAND(mirv_panorama, "")
{
	const auto arg0 = args->ArgV(0);
	int argc = args->ArgC();

	if (2 <= argc)
	{
		const char * arg1 = args->ArgV(1);

		if (0 == _stricmp("panelStyle", arg1)) {
			if (3 <= argc) {
				CSubWrpCommandArgs subArgs(args, 2);
				applyStyleProperty_Console(&subArgs);
			} else {
				advancedfx::Message(
					"%s %s panelStyle <option> <option>\n"
					"Where <option> at least 2 arguments are required: panelId or class and property to set.\n"
					"%s %s:\n"
					"\tpanelId=<str>\n"
					"\tpanelClassName=<str>\n"
					"\topacity=<fValue>\n"
					"Example:\n"
					"%s %s panelId=trueview_row opacity=0\n"
					"%s %s panelClassName=HudPerfStatsBasics opacity=0\n"
					"Warning: if matching by className the style would be applied to all instances.\n"
					, arg0, arg1
					, arg0, arg1
					, arg0, arg1
					, arg0, arg1
				);
			}
			return;
		}
	}

	advancedfx::Message(
		"%s panelStyle [...] - Set style for specific panorama panel.\n"
		, arg0
	);
}
