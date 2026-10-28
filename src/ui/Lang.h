#pragma once
// Interface language. The texts are written in German in the code; tr() looks up their English translation.
#include <string>

namespace ui {

enum class Language { German, English };

void setLanguage(Language language);
Language language();
inline bool english() { return language() == Language::English; }

// The translation of a German text in the current language; the text itself in German or if none is known.
const char* tr(const char* german);
std::string tr(const std::string& german);

}  // namespace ui
