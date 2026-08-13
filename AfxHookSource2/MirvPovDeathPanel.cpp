#include "stdafx.h"

#include "MirvPovDeathPanel.h"

MirvPovDeathPanelState g_MirvPovDeathPanelState;
thread_local CEntityInstance * g_MirvPovDeathPanelLocalPawnOverride = nullptr;

void MirvPovDeathPanel_LogStatus()
{
    MirvPovDeathPanelImpl_LogStatus();
}

void MirvPovDeathPanel_Clear()
{
    MirvPovDeathPanelImpl_Clear();
}

bool MirvPovDeathPanel_Reapply(const char * source)
{
    return MirvPovDeathPanelImpl_Reapply(source);
}

void MirvPovDeathPanel_Update()
{
    MirvPovDeathPanelImpl_Update();
}

int MirvPovDeathPanel_GetMode()
{
    return MirvPovDeathPanelImpl_GetMode();
}

bool MirvPovDeathPanel_SetMode(int mode)
{
    return MirvPovDeathPanelImpl_SetMode(mode);
}
