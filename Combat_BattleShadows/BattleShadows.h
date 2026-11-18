#pragma once

class PatcherInstance;

namespace battle_shadows
{
// ERM functions served by this plugin. ERA forwards !!FU(Name):P to plugin handlers with the same name:
//   !!FU(BattleShadow_Follow):P(radius 0..4, -1 = off)/(contour);       follows the mouse by itself
//   !!FU(BattleShadow_Radius):P(hex)/(radius 0..4)/(contour);
//   !!FU(BattleShadow_Hexes):P(hex1)/.../(hex10)/(contour);             -1 = unused slot
//   !!FU(BattleShadow_Stacks):P(hex1)/.../(hex10)/(contour);            whole stacks standing on those hexes
//   !!FU(BattleShadow_SkipStacks):P(hex1)/.../(hex16);                  stacks never shaded or outlined
//   !!FU(BattleShadow_Clear):P;
//   !!FU(BattleShadow_Debug):P?(version)/...                            diagnostics, see README.md
// Radius/Hexes/Stacks/SkipStacks are dropped before every !?MM0 (OnBattleMouseHint); everything, follow mode
// included, is dropped before every battle action. The shadows are drawn by the game's cursor shadow function
// (0x493A20), together with the hex under the mouse, once per combat screen message.
void RegisterEvents();
void InstallHooks(PatcherInstance *patcher);
}
