#pragma once

class PatcherInstance;

namespace battle_shadows
{
// ERM functions served by this plugin. ERA forwards !!FU(Name):P to plugin handlers with the same name:
//   !!FU(BattleShadow_Radius):P(hex)/(radius 0..4)/(contour);
//   !!FU(BattleShadow_Hexes):P(hex1)/.../(hex10)/(contour);   -1 = unused slot
//   !!FU(BattleShadow_Follow):P(radius 0..4 or -1 = off)/(contour);   follows the mouse by itself
//   !!FU(BattleShadow_Clear):P;
//   !!FU(BattleShadow_Debug):P?(version)/...  diagnostics, see README.md
// Radius/Hexes shapes are dropped before every !?MM0 (OnBattleMouseHint); everything is dropped before every
// battle action.
void RegisterEvents();
void InstallHooks(PatcherInstance *patcher);
}
