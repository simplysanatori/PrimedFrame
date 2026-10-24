"""Source checks for the merged F1 Mods page (the Remastered and Dread sections on it).

F1 used to hold a Remastered page beside Mods. The port's own two mods now live
in one page: the global controls, then "Built-in mods" with a Prime Remastered
and a Metroid Dread section (each its own switch, imported state and import),
then "Other mods" for the folder's other mods, then texture packs. What the
checks here pin down is what a compiler cannot see:

  * the page list holds one Mods page and no Remastered page, and MP_DEBUG_TAB
    still reaches it by the old names, opening the section each one named;
  * both built-in mods are drawn by the shared switch, which keeps them in
    mods_disabled as the other mods are, says whether this launch loaded them
    apart from what the switch selects, and gives way to Original experience;
    neither is listed again under Other mods;
  * every Dread control has a visible label of its own: the settings search
    counts rows by label on the page and opens only the section a row names, so
    a label shared with a closed Prime section would land on a hidden widget;
  * each built-in section keeps its import, its progress and Remastered's
    warning, and the search rows reach all of it on the Mods page.

It is a source check: no engine code runs and no game is started, so it cannot
say a jump lands on the right widget - the search audit run does that, with the
Prime section collapsed and expanded.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, root_argument  # noqa: E402

root = root_argument()
SEARCH = root / "platform/debug_ui/search.cpp"
REMASTERED = root / "platform/debug_ui/remastered.cpp"
MODS = root / "platform/debug_ui/mods.cpp"
INTERNAL = root / "platform/debug_ui/debug_ui_internal.h"
TABS = root / "platform/debug_ui/tabs.cpp"
INDEX = root / "platform/port_settings_index.inc"


def index_rows(text):
    """The rows of the settings index, as {page, section, label, keywords}."""
    rows = []
    for line in text.splitlines():
        if not line.startswith("{"):
            continue
        page, _sub, section, label, keywords = re.findall(r'"([^"]*)"', line)[:5]
        rows.append(dict(page=page, section=section, label=label, keywords=keywords))
    return rows


class Wiring(WiringCase):
    def test_one_mods_page_and_the_old_names_still_reach_it(self):
        search = SEARCH.read_text()
        self.assertEqual(search.count('{"Mods", DrawModsTab}'), 1)
        self.assertNotIn('{"Remastered", DrawRemasteredTab}', search)
        self.assertNotIn("DrawRemasteredTab", search + INTERNAL.read_text() + REMASTERED.read_text())
        # MP_DEBUG_TAB=<old name> opens the Mods page, on its section, for the captures and
        # scripts that use it.
        self.assertIn('{"Remastered", "Mods", "Prime Remastered"}', search)
        self.assertIn('{"Dread", "Mods", "Metroid Dread"}', search)

    def test_the_old_names_open_their_section(self):
        # A sub-tab name no tab bar has: StartSection spends it on the first frame the Mods page
        # is drawn, so a closed header (as ImGui last left it) still opens for the capture.
        search = SEARCH.read_text()
        self.assertIn("bool StartSection(const char* page, const char* name)", search + INTERNAL.read_text())
        self.assertIn("if (start) {\n    pending = false;\n  }", search)
        text = REMASTERED.read_text()
        for section in ("Prime Remastered", "Metroid Dread"):
            self.assertIn(f'if (StartSection("Mods", "{section}"))', text)
            self.assertIn("ImGui::SetNextItemOpen(true);", text)

    def test_the_mods_page_draws_the_built_in_sections(self):
        # The page: global controls, the built-in sections, then the other mods.
        text = MODS.read_text()
        self.assertIn("DrawBuiltinMods();", text)
        self.assertLess(text.index('ImGui::Checkbox("Load mods"'), text.index("DrawBuiltinMods();"))
        self.assertLess(text.index("DrawBuiltinMods();"), text.index('ImGui::SeparatorText("Other mods");'))
        # The built-in mods are never listed again under Other mods.
        other = text[text.index('ImGui::SeparatorText("Other mods");'):]
        self.assertIn("mod.name == PortRemastered::kImportModName", other)
        self.assertIn("mod.name == PortDreadSamus::kModName", other)
        self.assertIn('ImGui::TextDisabled("No other mods.");', other)

    def test_each_built_in_mod_has_a_section_with_its_switch_and_import(self):
        text = REMASTERED.read_text()
        sections = text[text.index("void DrawBuiltinMods() {"):]
        remastered = sections[sections.index('CollapsingHeader("Prime Remastered")'):]
        dread = remastered[remastered.index('CollapsingHeader("Metroid Dread")'):]
        # Prime Remastered: switch, the warning, the shared key row, the import and the gallery.
        self.assertIn('DrawBuiltinModToggle(PortRemastered::kImportModName, "Use the Remastered models"', remastered)
        self.assertIn("DrawRemasteredWarning();", remastered)
        self.assertIn("DrawImportKeys(0);", remastered)
        self.assertIn("DrawRemasteredImport();", remastered)
        self.assertIn('ImGui::SeparatorText("Gallery");', remastered)
        # Metroid Dread: its own switch and import, over the same prod.keys value.
        self.assertIn('DrawBuiltinModToggle(PortDreadSamus::kModName, "Use the Dread Suit models"', dread)
        self.assertIn("DrawImportKeys(1);", dread)
        self.assertIn("DrawDreadSamusImport();", dread)
        # Nothing to switch while the mod is not installed.
        self.assertIn('ImGui::TextDisabled("Not imported yet.");', text)
        # The one switch both sections use.
        self.assertEqual(text.count("void DrawBuiltinModToggle("), 1)
        # The Dread import no longer says to switch the mod off on another page.
        self.assertNotIn("switch it off on the Mods page", MODS.read_text())
        # The page's body is still the Mods function and the textures under it.
        self.assertIn("void DrawModsTab() {\n  DrawMods();\n  DrawTexturesSection();\n}", TABS.read_text())

    def test_the_switch_says_loaded_apart_from_selected(self):
        text = REMASTERED.read_text()
        # What this launch loaded, beside the switch's value (what the next reload loads).
        self.assertIn('ImGui::TextDisabled(on ? "Loaded." : "Loaded until the mods are reloaded.");', text)
        self.assertIn('ImGui::TextDisabled("Off, not loaded.");', text)
        self.assertIn('ImGui::TextDisabled("Not loaded while an import runs.");', text)
        self.assertIn('ImGui::TextColored(ThemeWarnColor(), "Not loaded: reload the mods to apply.");', text)
        # Original experience turns every mod off whatever the switch says: it is disabled, with
        # the reason, and the saved value is kept.
        self.assertIn("const bool modsOn = ModsEnabled();", text)
        self.assertIn("ImGui::BeginDisabled(!modsOn);", text)
        self.assertNotIn("ImGui::BeginDisabled(!sModsEnabled);", text)
        self.assertIn('sModsEnabled ? "(Original experience is on: no mods load)" : "(mods are off)"', text)
        # The tooltip may not claim the retail game: the other built-in mod may stay on.
        self.assertNotIn("Off plays the retail game", text)
        self.assertIn("Off unloads this mod alone", text)

    def test_the_dread_controls_have_labels_of_their_own(self):
        # A jump opens only the section its row names, and counts rows by label on the page: a
        # Dread control labelled as Prime's would be looked for in the closed Prime section.
        mods = MODS.read_text()
        dread_labels = ("Pick the Dread keys...", "Pick the Dread image...", "Browse the Dread keys...",
                        "Browse the Dread image...", "Import the Dread Samus models")
        for label in dread_labels:
            self.assertIn(f'"{label}', mods)
        rows = index_rows(INDEX.read_text())
        dread_rows = [row for row in rows if row["section"].startswith("Built-in mods > Metroid Dread")]
        prime_rows = [row for row in rows if row["section"].startswith("Built-in mods > Prime Remastered")]
        self.assertTrue(dread_rows)
        shared = {row["label"] for row in dread_rows} & {row["label"] for row in prime_rows}
        self.assertFalse(shared, f"labels the two sections share: {shared}")
        for label in dread_labels:
            self.assertEqual(len([row for row in dread_rows if row["label"] == label]), 1, label)
        # No row of either section is left counting on a widget of the other.
        self.assertTrue(all(row["page"] == "Mods" for row in dread_rows + prime_rows))
        # An image picker sits inside its section's own import header, which a jump must open;
        # the keys row is outside it, so it names the section only.
        pickers = ("Pick the .nsp/.xci...", "Browse...", "Pick the Dread image...",
                   "Browse the Dread image...")
        keys = ("Pick the keys...", "Pick the Dread keys...", "Browse the Dread keys...")
        for row in prime_rows + dread_rows:
            if row["label"] not in pickers and row["label"] not in keys:
                continue
            # Prime's desktop rows share the label Browse...: the keywords say which is which.
            if "keys" in row["keywords"]:
                self.assertNotIn(" models", row["section"], row)
            else:
                self.assertIn(" models", row["section"], row)

    def test_the_search_reaches_both_built_in_mods_on_the_mods_page(self):
        rows = index_rows(INDEX.read_text())
        for label, keywords in (("Use the Remastered models", "remastered"),
                                ("Use the Dread Suit models", "dread")):
            found = [row for row in rows if row["label"] == label]
            self.assertEqual(len(found), 1, label)
            self.assertEqual(found[0]["page"], "Mods")
            self.assertTrue(found[0]["section"].startswith("Built-in mods"), found[0])
            self.assertIn(keywords, found[0]["keywords"])
        # The Dread import's controls, on the same page.
        self.assertTrue([row for row in rows if row["section"].startswith("Built-in mods > Metroid Dread > Import")])
        # The index names no page that the overlay no longer has, and no empty conditional.
        self.assertFalse([row for row in rows if row["page"] == "Remastered"])
        self.assertNotIn("#if defined(__ANDROID__)\n#else\n#endif", INDEX.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1)
