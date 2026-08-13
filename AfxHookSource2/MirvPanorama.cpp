#include "stdafx.h"

#include "MirvPanorama.h"

#include "Globals.h"
#include "WrpConsole.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/utlmap.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"
#include "../shared/binutils.h"

#include "addresses.h"

#include <Windows.h>
#include <stdint.h>
#include <string.h>

static void* g_CStylePropertyOpacity_vtable = 0;
static void* g_CStylePropertyVisible_vtable = 0;
static void** g_PanoramaUIEngine = nullptr;

typedef void(__fastcall *g_CPanelStyleSetStyleProperty_t)(void* This, void* property, bool transition);
static g_CPanelStyleSetStyleProperty_t g_CPanelStyleSetStyleProperty = nullptr;

struct StylePropertySymbolMap {
	typedef uint8_t* (__fastcall *Resolve_t)(uint8_t* out, const char* stylePropertyName);

	uint8_t findSymbol(const char* stylePropertyName) {
		if(resolve) {
			uint8_t result = 0xFF;
			resolve(&result, stylePropertyName);
			return result;
		}

		if(!symbols) return 0xFF;

		for(int i = 0; i < symbols->numElements; ++i) {
			if(strcmp(symbols->memory[i].key.Get(), stylePropertyName) == 0) return symbols->memory[i].value;
		}

		return 0xFF;
	}

	Resolve_t resolve = nullptr;
	SOURCESDK::CS2::CUtlMap<SOURCESDK::CS2::CUtlString, uint8_t>* symbols = nullptr;
} g_PanoramaStylePropertySymbols;

CON_COMMAND(__mirv_panorama_dump_style_symbols, "") {
	auto symbols = g_PanoramaStylePropertySymbols.symbols;
	if(!symbols) {
		advancedfx::Warning("AFXWARNING: Panorama style-symbol dumping is unavailable for this CS2 build.\n");
		return;
	}

	for(int i = 0; i < symbols->numElements; ++i) {
		auto node = symbols->memory[i];
		advancedfx::Message("%i: %s\n", node.value, node.key.Get());
	}
}

struct StylePropertyOpacity {
	void* vtable;
	uint8_t id;
	bool disallowTransition = false;
	u_char pad[0x6];
	float value;

	StylePropertyOpacity() {}

	StylePropertyOpacity(void* vt, uint8_t i, float v)
		: vtable(vt), id(i), value(v) {}
};

struct StylePropertyVisible {
	void* vtable;
	uint8_t id;
	bool disallowTransition = false;
	u_char pad[0x6];
	uint16_t value;

	StylePropertyVisible() {}

	StylePropertyVisible(void* vt, uint8_t i, bool v)
		: vtable(vt), id(i), value(v ? 0x0101 : 0x0001) {}
};

static bool makeOpacityProperty(StylePropertyOpacity* out, float value) {
	auto id = g_PanoramaStylePropertySymbols.findSymbol("opacity");
	if(g_CStylePropertyOpacity_vtable == nullptr || id == 0xFF) return false;

	*out = StylePropertyOpacity { g_CStylePropertyOpacity_vtable, id, value };

	return true;
}

static bool makeVisibleProperty(StylePropertyVisible* out, bool value) {
	auto id = g_PanoramaStylePropertySymbols.findSymbol("visibility");
	if(g_CStylePropertyVisible_vtable == nullptr || id == 0xFF) return false;

	*out = StylePropertyVisible { g_CStylePropertyVisible_vtable, id, value };

	return true;
}

struct CUIPanel {
	bool setOpacity(float value) {
		auto style = (u_char*)(this + CS2::PanoramaUIPanel::panelStyle);

		StylePropertyOpacity styleProp;
		if(!makeOpacityProperty(&styleProp, value)) return false;

		g_CPanelStyleSetStyleProperty(style, &styleProp, true);

		return true;
	}

	bool setVisible(bool value) {
		auto style = (u_char*)(this + CS2::PanoramaUIPanel::panelStyle);

		StylePropertyVisible styleProp;
		if(!makeVisibleProperty(&styleProp, value)) return false;

		g_CPanelStyleSetStyleProperty(style, &styleProp, true);

		return true;
	}
};

namespace CS2 {
	namespace PanoramaUIPanel {
		ptrdiff_t getAttributeString = 0;
		ptrdiff_t setAttributeString = 0;
		void** hudPanel = nullptr;
	}

	namespace PanoramaPanelStyle {
		ptrdiff_t setPanelStyleProperty = 0;
	}

	namespace PanoramaUIEngine {
		ptrdiff_t makeSymbol = 0;
	}
};

void MirvPanorama_SetHudPanel(void** value) {
	CS2::PanoramaUIPanel::hudPanel = value;
}

void MirvPanorama_SetUIEngine(void** value) {
	g_PanoramaUIEngine = value;
}

static bool Panorama_MakeSymbol(const char* name, short& value) {
	if(!name || !g_PanoramaUIEngine || !*g_PanoramaUIEngine || !CS2::PanoramaUIEngine::makeSymbol) return false;

	typedef short(__fastcall * MakeSymbol_t)(void*, int, const char*);
	auto uiEngine = *g_PanoramaUIEngine;
	auto vtable = *(unsigned char**)uiEngine;
	if(!vtable) return false;

	auto makeSymbol = *(MakeSymbol_t*)(vtable + CS2::PanoramaUIEngine::makeSymbol);
	if(!makeSymbol) return false;

	value = makeSymbol(uiEngine, 0, name);
	return value != (short)-1;
}

bool Panorama_SetPanelClass(void* panel, const char* className, bool value) {
	if(!panel || !className) return false;

	short classSymbol = -1;
	if(!Panorama_MakeSymbol(className, classSymbol)) return false;

	typedef void (__fastcall * SetPanelClass_t)(void*, short);
	typedef bool (__fastcall * HasPanelClass_t)(void*, short);
	auto vtable = *(void***)panel;
	if(!vtable) return false;

	auto setPanelClass = (SetPanelClass_t)vtable[value ? 144 : 147];
	auto hasPanelClass = (HasPanelClass_t)vtable[157];
	if(!setPanelClass || !hasPanelClass) return false;

	setPanelClass(panel, classSymbol);
	return hasPanelClass(panel, classSymbol) == value;
}

bool Panorama_HasPanelClass(void* panel, const char* className) {
	if(!panel || !className) return false;

	short classSymbol = -1;
	if(!Panorama_MakeSymbol(className, classSymbol)) return false;

	auto vtable = *(void***)panel;
	if(!vtable) return false;

	typedef bool (__fastcall * HasPanelClass_t)(void*, short);
	auto hasPanelClass = (HasPanelClass_t)vtable[157];
	return nullptr != hasPanelClass && hasPanelClass(panel, classSymbol);
}

bool Panorama_SetPanelOpacity(void* panel, float value) {
	if(!panel) return false;
	return ((CUIPanel*)panel)->setOpacity(value);
}

bool Panorama_SetPanelVisible(void* panel, bool value) {
	if(!panel) return false;
	return ((CUIPanel*)panel)->setVisible(value);
}

bool MirvPanorama_InitStyleProperties(HMODULE panoramaDll) {
	{
		g_CStylePropertyOpacity_vtable = (void**)Afx::BinUtils::FindClassVtable(panoramaDll, ".?AVCStylePropertyOpacity@panorama@@", 0, 0);
		if(nullptr == g_CStylePropertyOpacity_vtable) {
			ErrorBox(MkErrStr(__FILE__, __LINE__));
			return false;
		}
	}

	{
		g_CStylePropertyVisible_vtable = (void**)Afx::BinUtils::FindClassVtable(panoramaDll, ".?AVCStylePropertyVisible@panorama@@", 0, 0);
		if(nullptr == g_CStylePropertyVisible_vtable) {
			ErrorBox(MkErrStr(__FILE__, __LINE__));
			return false;
		}
	}

	{
		auto addr = getAddress(panoramaDll, "40 55 56 57 41 54 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 48 8B F9 65 48 8B 04 25 58 00 00 00 45 33 E4 C6 01 FF 48 8B F2");
		if(addr) g_PanoramaStylePropertySymbols.resolve = (StylePropertySymbolMap::Resolve_t)addr;
	}

	{
		auto addr = getAddress(panoramaDll, "0f 10 45 f7 48 8d 0d ?? ?? ?? ?? 41 f7 c0 ff ff ff 7f");
		if(0 == addr) {
			if(!g_PanoramaStylePropertySymbols.resolve) {
				ErrorBox(MkErrStr(__FILE__, __LINE__));
				return false;
			}
			advancedfx::Warning("AFXWARNING: Panorama style-symbol map is unavailable; style lookup will use the resolver.\n");
		} else {
			auto out = addr + 11 + *(int32_t*)(addr + 7);
			g_PanoramaStylePropertySymbols.symbols = (SOURCESDK::CS2::CUtlMap<SOURCESDK::CS2::CUtlString, uint8_t>*)out;
		}
	}

	{
		auto addr = getAddress(panoramaDll, "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 45 ?? EB");
		if(!addr) {
			ErrorBox(MkErrStr(__FILE__, __LINE__));
			return false;
		}

		g_CPanelStyleSetStyleProperty = (g_CPanelStyleSetStyleProperty_t)(addr + 5 + *(int32_t*)(addr + 1));
	}

	return true;
}
