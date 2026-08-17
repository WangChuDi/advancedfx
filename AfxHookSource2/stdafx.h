#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC  
#include <stdlib.h> 
#include <crtdbg.h>
#endif

#ifndef AFX_MIRV_POV_DIAGNOSTICS
#define AFX_MIRV_POV_DIAGNOSTICS 0
#endif

#if AFX_MIRV_POV_DIAGNOSTICS
#define MIRV_POV_DIAGNOSTIC_MESSAGE(...) advancedfx::Message(__VA_ARGS__)
#define MIRV_POV_DIAGNOSTIC_WARNING(...) advancedfx::Warning(__VA_ARGS__)
#else
#define MIRV_POV_DIAGNOSTIC_MESSAGE(...) do { } while(false)
#define MIRV_POV_DIAGNOSTIC_WARNING(...) do { } while(false)
#endif