#pragma once
#include <imgui.h>

// Shared menu typography, colours and controls, also used by the character creator.
namespace ui::theme {
float scale(float value);
void backdrop(float edge);
void heading(ImVec2 position, const char* title, const char* subtitle);
void columns(float width, float labelFraction = 0.42f);
bool tab(const char* label, bool active);
bool slider(const char* label, const char* id, float* value, float lo, float hi, const char* format);
void section(const char* text);
bool action(const char* label, float width);
bool choice(const char* id, const char* title, const char* description, bool active, float width);
void muted(const char* text);
float row(const char* label);  // label in the left column; returns the right column's width for the next item
bool toggle(const char* label, const char* id, bool* value);
// Colour chip (sRGB) with an accent ring when active.
bool swatch(const char* id, const float rgb[3], bool active, float size);
}
