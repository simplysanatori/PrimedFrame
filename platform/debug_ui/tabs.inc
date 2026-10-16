// platform/debug_ui.cpp, in part: The pages assembled from their sections: Game, Controls, Video, Mods, System.
// Included by debug_ui.cpp at this point in the file, not compiled on its
// own: it shares that file's state and namespaces.

// The game's own volumes, the same as the pause menu's two sliders.
void DrawGameAudio() {
  ImGui::SeparatorText("Audio");
  ImGui::BeginDisabled(gpGameState == nullptr);
  int music = gpGameState != nullptr ? gpGameState->GameOptions().GetMusicVolume() : 0;
  int musicPct = (music * 100 + 63) / 127;
  if (ImGui::SliderInt("Music volume", &musicPct, 0, 100, "%d%%") && gpGameState != nullptr) {
    gpGameState->GameOptions().SetMusicVolume((musicPct * 127 + 50) / 100, true);
  }
  int sfx = gpGameState != nullptr ? gpGameState->GameOptions().GetSfxVolume() : 0;
  int sfxPct = (sfx * 100 + 63) / 127;
  if (ImGui::SliderInt("Sound effects volume", &sfxPct, 0, 100, "%d%%") && gpGameState != nullptr) {
    gpGameState->GameOptions().SetSfxVolume((sfxPct * 127 + 50) / 100, true);
  }
  // Sound mode: Mono / Stereo / Dolby Surround
  int soundMode = gpGameState != nullptr ? static_cast<int>(gpGameState->GameOptions().GetSurroundMode()) : CAudioSys::kSM_Stereo;
  constexpr const char* kSoundModeLabels[] = {"Mono", "Stereo", "Dolby Surround"};
  if (ImGui::Combo("Sound mode", &soundMode, kSoundModeLabels, IM_ARRAYSIZE(kSoundModeLabels)) && gpGameState != nullptr) {
    gpGameState->GameOptions().SetSurroundMode(static_cast<CAudioSys::ESurroundModes>(soundMode), true);
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("These audio options are saved globally and persist across restarts.");
}

void DrawOriginalSection() {
  bool original = sOriginalExperience;
  if (ImGui::Checkbox("Original experience", &original)) {
    SetOriginalExperience(original);
  }
  ItemHelp("Plays the game as on the GameCube: 640x480 at 4:3, 60 Hz with no interpolation, no "
           "mods, the retail HUD, font, button icons, FOV and cutscenes, and none of the gameplay extras (spring ball, "
           "fast morph, charge and lock-on options, turbo fire, unlocks). The other settings keep "
           "their values and come back when it's turned off. Controls, cheats, save states, the "
           "randomizer, Archipelago and the timer still work.");
}

void DrawGameTab() {
  DrawOriginalSection();
  DrawGameSection();
  DrawCutscenesSection();
  DrawUnlocksSection();
  DrawGameAudio();
  DrawLanguageSection();
  DrawSpeedrunSection();
}

void DrawControlsTab() {
  if (!ImGui::BeginTabBar("controlsPages")) {
    return;
  }
  if (SubTab("Controls", "Options")) {
    DrawControlsOptions();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Mouse settings")) {
    DrawControlsKeyboardMouse();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Controller settings")) {
    DrawControlsController();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Touch & gyro")) {
    DrawControlsTouchGyro();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Remap")) {
    PortInputRemap::Draw();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void DrawVideoTab() {
  if (!ImGui::BeginTabBar("videoPages")) {
    return;
  }
  if (SubTab("Video", "Display")) {
    DrawVideoDisplay();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Quality")) {
    DrawVideoQuality();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Frame rate")) {
    DrawPerformanceTab();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Compatibility")) {
    DrawVideoCompatibility();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void DrawModsTab() {
  DrawMods();
  DrawTexturesSection();
}

void DrawSystemTab() {
  DrawOverlaySection();
  DrawSettingsSection();
  DrawMemoryCard();
#if defined(__ANDROID__)
  PortDataFolder::DrawPanel();
#endif
  DrawLogSection();
  DrawDiscordSection();
  DrawUpdateSection();
  ImGui::SeparatorText("About");
  ImGui::TextDisabled("Version %s, build %s", MP_BUILD_VERSION, MP_BUILD_REVISION);
}

struct DebugPage {
  const char* name;
  void (*draw)();
};
