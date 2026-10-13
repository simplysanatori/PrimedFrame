#pragma once

// Port: pointer helpers shared by the game's menu screens (port_pointer.h).
// A screen calls them from its ProcessUserInput, behind the same gates as its
// pad input, so a click only acts when a press would.
//
// Rows: on the mouse, hover selects and a click selects and confirms. On
// touch, a tap selects and a tap on the selected row confirms.

class CFinalInput;
class CGuiSliderGroup;
class CGuiTableGroup;
class CGuiWidget;

namespace PortPointerMenu {

// Pointer menus are on; marks the screen as pointer-driven for this frame
// (cursor shown, mouse lock released). Call once per handled frame.
bool Begin();

enum class Action { None, Select, Advance };

// The selectable row of table drawn under the GUI point, or -1.
int RowAt(CGuiTableGroup* table, float gx, float gy);

// Hover and clicks on table's rows. A click outside the rows is left for
// someone else. With callAdvance, a confirm fires the table's advance
// callback; otherwise the caller acts on Action::Advance itself. Without
// hover, only clicks select (rows whose selection is a setting's value).
Action Table(CGuiTableGroup* table, bool callAdvance = true, bool hover = true);

// The row of table under the pending click, without taking it, or -1. Works
// on an inactive table (a click there usually means "go back to it").
int PeekClickRow(CGuiTableGroup* table);

// Press or drag on an active slider sets its value from the pointer's x.
// True while the slider holds the pointer (the caller skips other handlers).
bool Slider(CGuiSliderGroup* slider);

// A click or tap on widget (or its children), taken if it hit.
bool TakeClickOn(const CGuiWidget* widget);
// Any click or tap, taken.
bool TakeClick();
// A right click (or scripted back), taken.
bool TakeBack();

// Wheel notches and vertical drag, as whole row steps (positive = up).
// stepGui is the drag distance in EFB pixels that makes one step.
int TakeScrollSteps(float stepGui);

// Merge a confirm or cancel into a copy of the frame's input.
// The caller turned a Table Advance (callAdvance off) into a confirm: the
// click's double-click partner is swallowed like Table's own advances.
void NoteAdvanced();

void PressA(CFinalInput& input);
void PressB(CFinalInput& input);

} // namespace PortPointerMenu
