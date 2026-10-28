#include "ui/Lang.h"

#include <string_view>
#include <unordered_map>

namespace ui {
namespace {

Language g_language = Language::German;

const std::unordered_map<std::string_view, const char*>& translations() {
    static const std::unordered_map<std::string_view, const char*> table = {
        // ---- pause menu, HUD
        {"Optionen", "Options"}, {"Fortsetzen", "Resume"}, {"Charakter erstellen", "Create character"},
        {"Beenden", "Quit"}, {"Zurück", "Back"}, {"Esc  Zurück zum Spiel", "Esc  Back to game"},
        {"Esc  Zurück", "Esc  Back"}, {"Esc  Menü", "Esc  Menu"}, {"Lade Gelände ...", "Loading terrain ..."},
        {"%.0f m ü. NN   ·   %s", "%.0f m above sea level   ·   %s"}, {"FLUGMODUS  ·  %.0f m/s", "FLIGHT MODE  ·  %.0f m/s"},
        {"NO", "NE"}, {"O", "E"}, {"SO", "SE"},
        {"Wald", "Forest"}, {"Buschland", "Shrubland"}, {"Grasland", "Grassland"}, {"Ackerland", "Cropland"},
        {"Bebaut", "Built-up"}, {"Kahl / spärlich", "Bare / sparse"}, {"Schnee / Eis", "Snow / ice"},
        {"Wasser", "Water"}, {"Feuchtgebiet", "Wetland"}, {"Mangroven", "Mangroves"},
        {"Moos / Flechten", "Moss / lichen"}, {"Unbekannt", "Unknown"},
        // ---- options
        {"Grafik", "Graphics"}, {"Welt", "World"}, {"Steuerung", "Controls"}, {"Anzeige", "Display"},
        {"QUALITÄT", "QUALITY"}, {"Voreinstellung", "Preset"}, {"Niedrig", "Low"}, {"Mittel", "Medium"},
        {"Hoch", "High"}, {"Benutzerdefiniert", "Custom"}, {"Grafikkarte: %s", "Graphics card: %s"},
        {"Renderauflösung", "Render resolution"}, {"Dynamische Auflösung", "Dynamic resolution"},
        {"Ziel-Bildrate", "Target frame rate"}, {"Schatten", "Shadows"}, {"Aus", "Off"}, {"Gras", "Grass"},
        {"Grasdichte", "Grass density"}, {"Geländedetails", "Terrain detail"}, {"Baumdetails", "Tree detail"},
        {"Kantenglättung (TAA)", "Anti-aliasing (TAA)"}, {"Umgebungsverdeckung (SSAO)", "Ambient occlusion (SSAO)"},
        {"DARSTELLUNG", "DISPLAY"}, {"Sichtfeld", "Field of view"}, {"Automatische Belichtung", "Automatic exposure"},
        {"Belichtung", "Exposure"}, {"Vollbild", "Fullscreen"}, {"Bildraten-Limit", "Frame rate limit"},
        {"SICHTWEITE", "VIEW DISTANCE"}, {"Sichtweite", "View distance"}, {"Bäume", "Trees"}, {"Dunst", "Haze"},
        {"Drahtgitter (Terrain)", "Wireframe (terrain)"},
        {"SONNENSTAND", "SUN"}, {"Uhrzeit", "Time of day"}, {"Datum", "Date"}, {"WETTER", "WEATHER"},
        {"Bewölkung", "Cloud cover"},
        {"Uhrzeit ist lokale Sonnenzeit am aktuellen Standort.", "Time of day is local solar time at the current location."},
        {"Januar", "January"}, {"Februar", "February"}, {"März", "March"}, {"Mai", "May"}, {"Juni", "June"},
        {"Juli", "July"}, {"Oktober", "October"}, {"Dezember", "December"},
        {"MAUS", "MOUSE"}, {"Empfindlichkeit", "Sensitivity"}, {"Y-Achse invertieren", "Invert Y axis"},
        {"FLIEGEN", "FLYING"}, {"Fluggeschwindigkeit", "Flying speed"}, {"TASTENBELEGUNG", "KEY BINDINGS"},
        {"Laufen", "Walk"}, {"Rennen / schneller fliegen", "Run / fly faster"}, {"Leertaste", "Space"},
        {"Springen", "Jump"}, {"Knien / Aufstehen", "Kneel / stand up"}, {"Flugmodus an / aus", "Flight mode on / off"},
        {"Steigen / Sinken (Flug)", "Climb / descend (flying)"}, {"Ich- / Verfolgeransicht", "First / third person view"},
        {"Mausrad", "Mouse wheel"}, {"Fluggeschwindigkeit / Kamera-Abstand", "Flying speed / camera distance"},
        {"Debug-Info", "Debug info"}, {"Menü", "Menu"},
        {"HUD anzeigen", "Show HUD"}, {"Fadenkreuz", "Crosshair"}, {"Debug-Info (F3)", "Debug info (F3)"},
        {"SPRACHE", "LANGUAGE"}, {"Sprache", "Language"}, {"GARDEROBE", "WARDROBE"}, {"Garderobe", "Wardrobe"},
        {"Installiert", "Installed"}, {"Nicht installiert", "Not installed"},
        {"Neu laden (%.0f MB)", "Download again (%.0f MB)"}, {"Herunterladen (%.0f MB)", "Download (%.0f MB)"},
        {"Herunterladen", "Download"}, {"Weiterspielen", "Continue"}, {"Neues Spiel", "New game"},
        {"Spiel laden", "Load game"}, {"Spiel speichern", "Save game"}, {"Hauptmenü", "Main menu"},
        {"Automatisch gespeichert", "Autosave"}, {"Spielstand %d", "Save %d"}, {"leer", "empty"},
        {"Lade Welt ...", "Loading world ..."}, {"Gespeichert", "Saved"},
        {"Gelände, Bäume und Gras werden aus echten Geodaten erzeugt.",
         "Terrain, trees and grass are generated from real geodata."}, {"Erneut versuchen", "Try again"}, {"Später", "Later"},
        {"Für deinen Charakter gibt es rund 300 Kleidungsstücke, Schuhe, Unterwäsche und Accessoires. Sie werden "
         "einmalig heruntergeladen und eingerichtet.",
         "There are about 300 garments, shoes, underwear and accessories for your character. They are downloaded and "
         "set up once."},
        {"Die Garderobe konnte nicht eingerichtet werden.", "The wardrobe could not be set up."},
        {"Download: %.0f MB. Du kannst das auch später unter Optionen > Anzeige nachholen.",
         "Download: %.0f MB. You can also do this later under Options > Display."},
        {"Lade herunter ... %.0f von %.0f MB", "Downloading ... %.0f of %.0f MB"},
        {"Richte ein ... %.0f %%", "Setting up ... %.0f %%"},
        {"Du kannst währenddessen weiterspielen, wenn du das Fenster schließt: Esc.",
         "You can keep playing meanwhile: close this screen with Esc."},
        // ---- teleport
        {"KOORDINATEN", "COORDINATES"}, {"Breite", "Latitude"}, {"Länge", "Longitude"}, {"Hinspringen", "Go there"},
        {"ORTE", "PLACES"}, {"Schwarzwald", "Black Forest"}, {"Wettersteingebirge", "Wetterstein Mountains"},
        {"Berner Oberland", "Bernese Oberland"}, {"Lofoten, Norwegen", "Lofoten, Norway"},
        {"Kalifornien, USA", "California, USA"},
        // ---- character creator
        {"Dein Charakter", "Your character"},
        {"Eine Vorlage wählen. Deinen eigenen Weg gestalten.", "Pick a template. Make it your own."},
        {"VORLAGE", "TEMPLATE"}, {"Männlich", "Male"}, {"Markant & athletisch", "Striking & athletic"},
        {"Weiblich", "Female"}, {"Weich & ausgewogen", "Soft & balanced"}, {"Zufällig", "Random"},
        {"Überrasch mich", "Surprise me"}, {"Körper", "Body"}, {"Aussehen", "Appearance"}, {"Kleidung", "Clothing"},
        {"PROPORTIONEN", "PROPORTIONS"}, {"Körperform", "Body shape"}, {"Alter", "Age"}, {"Muskelmasse", "Muscle mass"},
        {"Gewicht", "Weight"}, {"Größe", "Height"}, {"Proportionen", "Proportions"}, {"BRUST", "BREASTS"},
        {"Volumen", "Volume"}, {"Form", "Shape"}, {"INTIMBEREICH", "INTIMATE AREA"}, {"Genitalien", "Genitals"},
        {"Sichtbar, wenn weder Kleidung noch Unterwäsche getragen wird.",
         "Visible when neither clothes nor underwear are worn."},
        {"HERKUNFT", "ANCESTRY"}, {"Afrikanisch", "African"}, {"Asiatisch", "Asian"}, {"Europäisch", "European"},
        {"Mischt die typischen Gesichts- und Körpermerkmale.", "Blends the typical facial and body features."},
        {"Details suchen ...", "Search details ..."},
        {"Rechtsklick auf einen Regler setzt ihn zurück.", "Right-click a slider to reset it."},
        {"GESICHT", "FACE"}, {"KÖRPER", "BODY"}, {"Alle Details zurücksetzen", "Reset all details"},
        {"HAUT", "SKIN"}, {"Hauttyp", "Skin type"}, {"Helligkeit", "Brightness"}, {"Wärme", "Warmth"},
        {"AUGEN", "EYES"}, {"Augenfarbe", "Eye colour"}, {"Augenmodell", "Eye model"}, {"HAARE", "HAIR"},
        {"Frisur", "Hairstyle"}, {"Frisur von ", "Hairstyle by "}, {"unbekannt", "unknown"},
        {"Eigene Haarfarbe", "Custom hair colour"}, {"Farbton", "Hue"}, {"Sättigung", "Saturation"},
        {"Die Frisur behält ihre natürliche Farbe.", "The hairstyle keeps its natural colour."},
        {"Augenbrauen", "Eyebrows"}, {"Wimpern", "Eyelashes"},
        {"Augenbrauen und Wimpern übernehmen die Haarfarbe.", "Eyebrows and eyelashes take on the hair colour."},
        {"KLEIDUNG", "CLOTHING"}, {"In Vorschau zeigen", "Show in preview"}, {"Unterwäsche", "Underwear"},
        {"Schuhe", "Shoes"}, {"Hut", "Hat"}, {"Komplett-Outfit", "Full outfit"}, {"Oberteil", "Top"},
        {"Unterteil", "Bottom"}, {"Kopfbedeckung", "Headwear"}, {"Handschuhe", "Gloves"}, {"Schmuck", "Jewellery"},
        {"Accessoire", "Accessory"}, {"Bart", "Beard"}, {"BH / Top", "Bra / top"},
        {"Slip / Badehose", "Briefs / swim trunks"}, {"Strümpfe / Socken", "Stockings / socks"}, {"Brille", "Glasses"}, {"Einfache Unterwäsche", "Plain underwear"},
        {"Die Kleidung passt sich automatisch an deine Körperform an.", "Clothes adapt to your body shape automatically."},
        {"VORSCHAU", "PREVIEW"}, {"Ganzkörper", "Full body"}, {"Oberkörper", "Upper body"}, {"Gesicht", "Face"},
        {"Ziehen: drehen  ·  Mausrad: zoomen  ·  Rechts ziehen: verschieben  ·  Doppelklick: zurück",
         "Drag: turn  ·  Wheel: zoom  ·  Right drag: move  ·  Double-click: reset"},
        {"Mit Kleidung, so wie du im Spiel aussiehst.", "With clothes, as you look in the game."},
        {"Ohne Kleidung, damit du jede Form gut erkennen kannst.", "Without clothes, so you can see every shape."},
        {"Vorlage zurücksetzen", "Reset template"}, {"Gespeichert laden", "Load saved"},
        {"Kein gültiger gespeicherter Charakter gefunden.", "No valid saved character found."},
        {"Die Vorschau konnte nicht aktualisiert werden.", "The preview could not be updated."},
        {"Übernehmen", "Apply"}, {"Speichern fehlgeschlagen. Bitte erneut versuchen.", "Saving failed. Please try again."},
        {"Abbrechen", "Cancel"}, {"Esc  Zurück ohne Änderungen", "Esc  Back without changes"}, {"Keine", "None"},
        {"kleiner", "smaller"}, {"größer", "larger"}, {"weniger", "less"}, {"mehr", "more"},
        {" (Damen)", " (women's)"}, {" (Herren)", " (men's)"}, {"Augenbrauen ", "Eyebrows "}, {"Wimpern ", "Eyelashes "},
        {"Schuhe ", "Shoes "},
        // colours
        {"Blau", "Blue"}, {"Blaugrün", "Blue-green"}, {"Braun", "Brown"}, {"Hellbraun", "Light brown"},
        {"Tiefblau", "Deep blue"}, {"Grün", "Green"}, {"Grau", "Grey"}, {"Eisblau", "Ice blue"}, {"Hellblau", "Light blue"},
        {"Schwarz", "Black"}, {"Dunkelbraun", "Dark brown"}, {"Kastanie", "Auburn"}, {"Kupfer", "Copper"},
        {"Dunkelblond", "Dark blond"}, {"Platin", "Platinum"}, {"Weiß", "White"}, {"Nachtblau", "Midnight blue"},
        {"Rosé", "Rose"}, {"Smaragd", "Emerald"},
        // ---- asset labels (human.json and the creator's names for MakeHuman assets)
        {"Lang, glatt", "Long, straight"}, {"Pferdeschwanz", "Ponytail"}, {"Dutt mit Pony", "Bun with bangs"},
        {"Lang, offen (Alexxa)", "Long, loose (Alexxa)"}, {"Lang mit Zöpfchen (Faith)", "Long with braids (Faith)"},
        {"Seitenzopf (Freya)", "Side ponytail (Freya)"}, {"Zwei Zöpfe (Kitty)", "Two ponytails (Kitty)"},
        {"Sehr lang (Lilia)", "Very long (Lilia)"}, {"Lang mit Pony (Lissa)", "Long with bangs (Lissa)"},
        {"Lang, gestuft (Misty)", "Long, layered (Misty)"}, {"Lang, glatt (Nina)", "Long, straight (Nina)"},
        {"Halblang (Selena)", "Medium length (Selena)"}, {"Lang mit Pony (Dawn)", "Long with bangs (Dawn)"},
        {"Weiblich (realistisch)", "Female (realistic)"}, {"Weiblich (vereinfacht)", "Female (simplified)"},
        {"Männlich (realistisch)", "Male (realistic)"}, {"Männlich (MakeHuman)", "Male (MakeHuman)"},
        {"Bob, lang", "Bob, long"}, {"Bob, kurz", "Bob, short"}, {"Geflochtener Zopf", "Braid"},
        {"Lang & offen", "Long & loose"}, {"Kurz, gescheitelt", "Short, parted"}, {"Kurz, frech", "Short, cheeky"},
        {"Kurz, zurückgekämmt", "Short, slicked back"}, {"Kurzhaarschnitt", "Crew cut"}, {"Bob, gerade", "Bob, blunt"},
        {"Bob, gerade mit Pony", "Bob, blunt with bangs"}, {"Bob, eingedreht", "Bob, curled under"},
        {"Bob, eingedreht mit Pony", "Bob, curled under with bangs"}, {"Bob, gestuft", "Bob, inverted"},
        {"Bob, gestuft mit Pony", "Bob, inverted with bangs"}, {"Bob, kinnlang", "Bob, chin length"},
        {"Lang, wellig", "Long, wavy"}, {"Kurz, fransig", "Short, choppy"}, {"Dutt", "Bun"},
        {"Zwei Zöpfe", "Two braids"}, {"Französischer Zopf", "French braid"}, {"Zopf mit Dutt", "Braid with bun"},
        {"Zopf, locker", "Braid, loose"}, {"Mit Haarband", "With headband"}, {"Kurz, seitlich", "Short, side swept"},
        {"Kurz, voll", "Short, full"}, {"Mittellang, gescheitelt", "Medium, parted"}, {"Kurz, zerzaust", "Short, messy"},
        {"Lang, zottelig", "Long, shaggy"}, {"Stufig, schulterlang", "Layered, shoulder length"},
        {"Glatt mit Pony", "Straight with bangs"}, {"Lockenwolke", "Curly cloud"}, {"Locken, lang", "Curls, long"},
        {"Locken, sehr lang", "Curls, very long"}, {"50er-Hochsteckfrisur", "50s updo"},
        {"Lang, Seitenscheitel", "Long, side parting"}, {"Schulterlang, wellig", "Shoulder length, wavy"},
        {"Flechtdutt", "Braided bun"}, {"Lang, Mittelscheitel", "Long, centre parting"},
        {"Zurückgekämmt", "Slicked back"}, {"Lang, gesträhnt", "Long, highlighted"}, {"Bob, fransig", "Bob, choppy"},
        {"Lang mit Schleife", "Long with bow"}, {"Asymmetrisch mit Pony", "Asymmetric with bangs"},
        {"Zurückgebunden", "Tied back"}, {"Hippie, seitlich", "Hippie, side swept"},
        {"Hochgesteckt mit Strähnen", "Updo with strands"}, {"Bob, Mittelscheitel", "Bob, centre parting"},
        {"80er-Wellen", "80s waves"}, {"Kurz, Seitenscheitel", "Short, side parting"}, {"Locken-Bob", "Curly bob"},
        {"Meister mit Bart", "Master with beard"}, {"Anime, wellig", "Anime, wavy"}, {"Kurz, stufig", "Short, layered"},
        {"Kurz, gewellt", "Short, wavy"}, {"Lang, voluminös", "Long, voluminous"}, {"Pagenkopf", "Pageboy"},
        {"Lang, über dem Auge", "Long, over the eye"}, {"Lang, schlicht", "Long, plain"}, {"Stachelig", "Spiky"},
        {"Bob, lang und stufig", "Bob, long and layered"}, {"Locken, seitlich", "Curls, side swept"},
        {"Mit Zopfband", "With hair tie"}, {"Kurz, hochgesteckt", "Short, pinned up"}, {"Bob mit Pony", "Bob with bangs"},
        {"Detailliert", "Detailed"}, {"Einfach", "Simple"}, {"Fedora, schräg", "Fedora, tilted"},
        {"Freizeit 1", "Casual 1"}, {"Freizeit 2", "Casual 2"}, {"Freizeit 3", "Casual 3"}, {"Freizeit 4", "Casual 4"},
        {"Freizeit 5", "Casual 5"}, {"Freizeit 6", "Casual 6"}, {"Sport", "Sports"}, {"Anzug", "Suit"},
        {"Arbeitskleidung", "Workwear"},
    };
    return table;
}

}  // namespace

void setLanguage(Language language) { g_language = language; }
Language language() { return g_language; }

const char* tr(const char* german) {
    if (g_language == Language::German || !german) return german;
    auto it = translations().find(german);
    return it == translations().end() ? german : it->second;
}

std::string tr(const std::string& german) { return tr(german.c_str()); }

}  // namespace ui
