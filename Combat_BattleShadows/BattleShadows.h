#pragma once

class PatcherInstance;

namespace battle_shadows
{
// ERM functions served by this plugin. ERA forwards !!FU(Name):P to plugin handlers with the same name:
//   !!FU(BattleShadow_Radius):P(hex)/(radius 0..4)/(contour);
//   !!FU(BattleShadow_Hexes):P(hex1)/.../(hex10)/(contour);   -1 = unused slot
//   !!FU(BattleShadow_Clear):P;
// Everything is removed automatically before every !?MM0 (OnBattleMouseHint) and battle action.
void RegisterEvents();
void InstallHooks(PatcherInstance *patcher);
}
