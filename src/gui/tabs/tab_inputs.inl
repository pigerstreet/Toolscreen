if (ImGui::BeginTabItem(trc("tabs.inputs"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    if (ImGui::BeginTabBar("InputsSubTabs")) {
        if (ImGui::BeginTabItem(trc("inputs.mouse"))) {
            SliderCtrlClickTip();

            ImGui::SeparatorText(trc("inputs.mouse_settings"));

            RawInputSensitivityNote();
            ImGui::Text(trc("label.mouse_sensitivity"));
            ImGui::SetNextItemWidth(600);
            if (ImGui::SliderFloat("##mouseSensitivity", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("tooltip.mouse_sensitivity"));

            ImGui::Text(trc("label.mouse_movement_polling_rate"));
            ImGui::SetNextItemWidth(600);
            int mouseMovementPollingRate = g_config.mouseMovementPollingRate;
            if (ImGui::SliderInt("##mouseMovementPollingRate", &mouseMovementPollingRate, 0,
                                 ConfigDefaults::CONFIG_MOUSE_MOVEMENT_POLLING_RATE_MAX,
                                 mouseMovementPollingRate == 0 ? trc("label.disabled") : "%d events/s")) {
                const int interval = ConfigDefaults::CONFIG_MOUSE_MOVEMENT_POLLING_RATE_INTERVAL;
                if (mouseMovementPollingRate > 0) {
                    mouseMovementPollingRate =
                        static_cast<int>(std::lround(static_cast<double>(mouseMovementPollingRate) / static_cast<double>(interval))) * interval;
                    mouseMovementPollingRate = std::clamp(mouseMovementPollingRate, interval, ConfigDefaults::CONFIG_MOUSE_MOVEMENT_POLLING_RATE_MAX);
                }
                g_config.mouseMovementPollingRate = mouseMovementPollingRate;
                g_configIsDirty = true;
                ResetMouseMovementThrottleState();
            }
            ImGui::SameLine();
            HelpMarker(trc("tooltip.mouse_movement_polling_rate"));

            ImGui::Text(trc("label.windows_mouse_speed"));
            ImGui::SetNextItemWidth(600);
            int windowsSpeedValue = g_config.windowsMouseSpeed;
            if (ImGui::SliderInt("##windowsMouseSpeed", &windowsSpeedValue, 0, 20, windowsSpeedValue == 0 ? trc("label.disabled") : "%d")) {
                g_config.windowsMouseSpeed = windowsSpeedValue;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("tooltip.windows_mouse_speed"));

            if (g_gameVersion < GameVersion(1, 13, 0)) {
                if (ImGui::Checkbox(trc("label.let_cursor_escape_window"), &g_config.allowCursorEscape)) { g_configIsDirty = true; }
                ImGui::SameLine();
                HelpMarker(trc("tooptip.let_cursor_escape_window"));
            }

            ImGui::Spacing();
            ImGui::SeparatorText(trc("inputs.cursor_configuration"));

            if (ImGui::Checkbox(trc("inputs.enable_custom_cursors"), &g_config.cursors.enabled)) {
                g_configIsDirty = true;
                g_cursorsNeedReload = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("tooltip.cursor_change"));

            if (ImGui::Button(trc("button.open_cursor_folder"))) {
                if (g_toolscreenPath.empty()) {
                    Log("ERROR: Unable to open custom cursor folder because toolscreen path is empty.");
                } else {
                    std::wstring cursorsPath = g_toolscreenPath + L"\\cursors";
                    std::error_code ec;
                    if (!std::filesystem::exists(cursorsPath, ec)) { std::filesystem::create_directories(cursorsPath, ec); }

                    HINSTANCE shellResult = ShellExecuteW(NULL, L"open", cursorsPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    if ((INT_PTR)shellResult <= 32) { Log("ERROR: Failed to open custom cursor folder."); }
                }
            }
            ImGui::SameLine();
            HelpMarker(trc("tooltip.open_cursor_folder"));

            ImGui::Spacing();

            if (g_config.cursors.enabled) {
                ImGui::Text(trc("inputs.configure_cursors_for_different_game_states"));
                ImGui::Spacing();

                struct CursorOption {
                    std::string key;
                    std::string name;
                    std::string description;
                };

                static std::vector<CursorOption> availableCursors;
                static bool cursorListInitialized = false;
                static auto lastCursorListRefreshTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

                auto now = std::chrono::steady_clock::now();
                if (!cursorListInitialized || now - lastCursorListRefreshTime >= std::chrono::seconds(2)) {
                    CursorTextures::RefreshCursorDefinitions();

                    availableCursors.clear();

                    auto cursorNames = CursorTextures::GetAvailableCursorNames();
                    availableCursors.reserve(cursorNames.size());

                    for (const auto& cursorName : cursorNames) {
                        std::string displayName = cursorName;

                        if (!displayName.empty()) {
                            displayName[0] = std::toupper(displayName[0]);
                            for (auto& c : displayName) {
                                if (c == '_' || c == '-') c = ' ';
                            }
                        }

                        std::string description;
                        if (cursorName.find("Cross") != std::string::npos) {
                            description = tr("inputs.crosshair_cursor");
                        } else if (cursorName.find("Arrow") != std::string::npos) {
                            description = tr("inputs.arrow_pointer_cursor");
                        } else {
                            description = tr("inputs.custom_cursor");
                        }

                        availableCursors.push_back({ cursorName, displayName, description });
                    }

                    cursorListInitialized = true;
                    lastCursorListRefreshTime = now;
                }

                struct CursorConfigUI {
                    const char* name;
                    CursorConfig* config;
                };

                CursorConfigUI cursors[] = { { trc("game_state.title"), &g_config.cursors.title },
                                             { trc("game_state.wall"), &g_config.cursors.wall },
                                             { trc("game_state.inworld_free"), &g_config.cursors.ingame } };

                for (int i = 0; i < 3; ++i) {
                    auto& cursorUI = cursors[i];
                    auto& cursorConfig = *cursorUI.config;
                    ImGui::PushID(i);

                    ImGui::SeparatorText(cursorUI.name);

                    const char* currentCursorName = cursorConfig.cursorName.c_str();
                    std::string currentDescription = "";
                    for (const auto& option : availableCursors) {
                        if (cursorConfig.cursorName == option.key) {
                            currentCursorName = option.name.c_str();
                            currentDescription = option.description;
                            break;
                        }
                    }

                    ImGui::Text(trc("inputs.cursor"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
                    if (ImGui::BeginCombo("##cursor", currentCursorName)) {
                        for (const auto& option : availableCursors) {
                            ImGui::PushID(option.key.c_str());

                            bool is_selected = (cursorConfig.cursorName == option.key);

                            if (ImGui::Selectable(option.name.c_str(), is_selected)) {
                                cursorConfig.cursorName = option.key;
                                g_configIsDirty = true;
                                // Schedule cursor reload (will happen outside GUI rendering to avoid deadlock)
                                g_cursorsNeedReload = true;

                                std::wstring cursorPath;
                                UINT loadType = IMAGE_CURSOR;
                                CursorTextures::GetCursorPathByName(option.key, cursorPath, loadType);

                                const CursorTextures::CursorData* cursorData =
                                    CursorTextures::LoadOrFindCursor(cursorPath, loadType, cursorConfig.cursorSize);
                                if (cursorData && cursorData->hCursor) { SetCursor(cursorData->hCursor); }
                            }

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%s", option.name.c_str());
                                ImGui::Separator();
                                ImGui::TextUnformatted(option.description.c_str());
                                ImGui::EndTooltip();
                            }

                            if (is_selected) { ImGui::SetItemDefaultFocus(); }

                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }

                    if (!currentDescription.empty() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(currentDescription.c_str());
                        ImGui::EndTooltip();
                    }

                    ImGui::SameLine();
                    ImGui::Spacing();
                    ImGui::SameLine();
                    ImGui::Text(trc("inputs.cursor_size"));
                    ImGui::SameLine();

                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
                    int sliderValue = cursorConfig.cursorSize;
                    if (ImGui::SliderInt("##cursorSize", &sliderValue, ConfigDefaults::CURSOR_MIN_SIZE,
                                         ConfigDefaults::CURSOR_MAX_SIZE, "%d px", ImGuiSliderFlags_AlwaysClamp)) {
                        int newSize = sliderValue;
                        if (newSize != cursorConfig.cursorSize) {
                            cursorConfig.cursorSize = newSize;
                            g_configIsDirty = true;

                            std::wstring cursorPath;
                            UINT loadType = IMAGE_CURSOR;
                            CursorTextures::GetCursorPathByName(cursorConfig.cursorName, cursorPath, loadType);

                            const CursorTextures::CursorData* cursorData = CursorTextures::LoadOrFindCursor(cursorPath, loadType, newSize);
                            if (cursorData && cursorData->hCursor) { SetCursor(cursorData->hCursor); }
                        }
                    }

                    ImGui::PopID();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button((tr("button.reset_defaults") + "##cursors").c_str())) { ImGui::OpenPopup(trc("inputs.reset_cursors_to_defaults")); }

                if (ImGui::BeginPopupModal(trc("inputs.reset_cursors_to_defaults"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
                    ImGui::Text(trc("inputs.reset_cursors_to_defaults_tip"));
                    ImGui::Text(trc("label.action_cannot_be_undone"));
                    ImGui::Separator();
                    if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
                        g_config.cursors = GetDefaultCursors();
                        g_configIsDirty = true;
                        g_cursorsNeedReload = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetItemDefaultFocus();
                    ImGui::SameLine();
                    if (ImGui::Button(trc("label.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                    ImGui::EndPopup();
                }
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(trc("inputs.keyboard"))) {
            SliderCtrlClickTip();

            ImGui::SeparatorText(trc("inputs.key_repeat_rate"));

            ImGui::Text(trc("inputs.key_repeat_start_delay"));
            ImGui::SetNextItemWidth(600);
            int startDelayValue = g_config.keyRepeatStartDelay;
            if (ImGui::SliderInt("##keyRepeatStartDelay", &startDelayValue, -1, 500,
                                 startDelayValue < 0 ? trc("label.default") : "%d ms")) {
                if (startDelayValue >= 0 && startDelayValue < 50) startDelayValue = 50;
                g_config.keyRepeatStartDelay = startDelayValue;
                g_configIsDirty = true;
                ApplyKeyRepeatSettings();
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.key_repeat_start_delay"));

            ImGui::Text(trc("inputs.key_repeat_delay"));
            ImGui::SetNextItemWidth(600);
            int repeatDelayValue = g_config.keyRepeatDelay;
            if (ImGui::SliderInt("##keyRepeatDelay", &repeatDelayValue, -1, 500,
                                 repeatDelayValue < 0 ? trc("label.default") : "%d ms")) {
                g_config.keyRepeatDelay = repeatDelayValue;
                g_configIsDirty = true;
                ApplyKeyRepeatSettings();
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.key_repeat_delay"));

            if (ImGui::Checkbox(trc("inputs.key_repeat_resume_previous"), &g_config.keyRepeatResumePreviousHeldKey)) {
                g_configIsDirty = true;
                ApplyKeyRepeatSettings();
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.key_repeat_resume_previous"));

            ImGui::Spacing();

            ImGui::SeparatorText(trc("inputs.key_rebinding"));
            ImGui::TextWrapped(trc("inputs.tooltip.key_rebinding"));
            ImGui::Spacing();

            if (ImGui::Checkbox(trc("inputs.enable_key_rebinding"), &g_config.keyRebinds.enabled)) {
                if (!g_config.keyRebinds.enabled) {
                    ReleaseActiveLowLevelRebindKeys(g_minecraftHwnd.load(std::memory_order_relaxed));
                }
                g_configIsDirty = true;
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.enable_key_rebinding"));

            if (ImGui::Checkbox(trc("inputs.resolve_rebind_targets_for_hotkeys"), &g_config.keyRebinds.resolveRebindTargetsForHotkeys)) {
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.resolve_rebind_targets"));

            const ImVec4 rebindActiveGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
            const ImVec4 rebindDisabledRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);
            ImGui::TextDisabled(trc("label.status"));
            ImGui::SameLine();
            ImGui::TextColored(g_config.keyRebinds.enabled ? rebindActiveGreen : rebindDisabledRed, "%s",
                               g_config.keyRebinds.enabled ? trc("label.enabled") : trc("label.disabled"));

            if (g_config.keyRebinds.enabled) {
                ImGui::Spacing();
                ImGui::SeparatorText(trc("inputs.rebind_toggle_hotkey"));
                std::string rebindToggleHotkeyStr = GetKeyComboString(g_config.keyRebinds.toggleHotkey);
                const bool isBindingRebindToggle = (s_mainHotkeyToBind == -995);
                const char* rebindToggleHotkeyButtonLabel =
                    isBindingRebindToggle ? trc("hotkeys.press_keys")
                                          : (rebindToggleHotkeyStr.empty() ? trc("hotkeys.click_to_bind") : rebindToggleHotkeyStr.c_str());
                if (ImGui::Button(rebindToggleHotkeyButtonLabel, ImVec2(150, 0))) {
                    s_mainHotkeyToBind = -995;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                    MarkHotkeyBindingActive();
                }
                ImGui::SameLine();
                HelpMarker(trc("inputs.tooltip.rebind_toggle_hotkey"));

                ImGui::Separator();
                ImGui::Spacing();

                auto getScanCodeWithExtendedFlag = [](DWORD vk) -> DWORD {
                    DWORD scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC_EX);
                    if (scan == 0) { scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC); }

                    if ((scan & 0xFF00) == 0) {
                        switch (vk) {
                        case VK_LEFT:
                        case VK_RIGHT:
                        case VK_UP:
                        case VK_DOWN:
                        case VK_INSERT:
                        case VK_DELETE:
                        case VK_HOME:
                        case VK_END:
                        case VK_PRIOR:
                        case VK_NEXT:
                        case VK_RCONTROL:
                        case VK_RMENU:
                        case VK_DIVIDE:
                        case VK_NUMLOCK:
                        case VK_SNAPSHOT:
                            if ((scan & 0xFF) != 0) { scan |= 0xE000; }
                            break;
                        default:
                            break;
                        }
                    }

                    return scan;
                };

                static int s_rebindFromKeyToBind = -1;
                static int s_rebindOutputVKToBind = -1;
                static int s_rebindTextOverrideVKToBind = -1;
                static int s_rebindOutputScanToBind = -1;

                enum class LayoutBindTarget {
                    None,
                    FullOutputVk,
                    TypesVk,
                    TypesVkShift,
                    TriggersVk,
                };
                static LayoutBindTarget s_layoutBindTarget = LayoutBindTarget::None;
                static int s_layoutBindIndex = -1;
                static uint64_t s_layoutBindLastSeq = 0;

                enum class LayoutUnicodeEditTarget {
                    None,
                    TypesBase,
                    TypesShift,
                };

                static int s_layoutUnicodeEditIndex = -1;
                static std::string s_layoutUnicodeEditText;
                static LayoutUnicodeEditTarget s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;

                static std::vector<std::string> s_rebindUnicodeTextEdit;

                auto isValidUnicodeScalar = [](uint32_t cp) -> bool {
                    if (cp == 0) return false;
                    if (cp > 0x10FFFFu) return false;
                    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
                    return true;
                };

                auto codepointToUtf8 = [&](uint32_t cp) -> std::string {
                    if (!isValidUnicodeScalar(cp)) return std::string();
                    std::wstring w;
                    if (cp <= 0xFFFFu) {
                        w.push_back((wchar_t)cp);
                    } else {
                        uint32_t v = cp - 0x10000u;
                        wchar_t high = (wchar_t)(0xD800u + (v >> 10));
                        wchar_t low = (wchar_t)(0xDC00u + (v & 0x3FFu));
                        w.push_back(high);
                        w.push_back(low);
                    }
                    return WideToUtf8(w);
                };

                auto formatCodepointUPlus = [&](uint32_t cp) -> std::string {
                    char buf[32] = {};
                    if (cp <= 0xFFFFu) {
                        sprintf_s(buf, "U+%04X", (unsigned)cp);
                    } else {
                        sprintf_s(buf, "U+%06X", (unsigned)cp);
                    }
                    return std::string(buf);
                };

                auto codepointToDisplay = [&](uint32_t cp) -> std::string {
                    if (!isValidUnicodeScalar(cp)) return std::string("[None]");
                    if (cp < 0x20u || cp == 0x7Fu) return formatCodepointUPlus(cp);
                    std::string s = codepointToUtf8(cp);
                    if (s.empty()) return formatCodepointUPlus(cp);
                    return s;
                };

                auto tryParseUnicodeInput = [&](const std::string& in, uint32_t& outCp) -> bool {
                    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
                    std::string s = in;
                    while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
                    while (!s.empty() && isSpace((unsigned char)s.back())) s.pop_back();
                    if (s.empty()) return false;

                    auto startsWithI = [&](const char* pfx) {
                        size_t n = std::char_traits<char>::length(pfx);
                        if (s.size() < n) return false;
                        for (size_t i = 0; i < n; ++i) {
                            char a = s[i];
                            char b = pfx[i];
                            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
                            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
                            if (a != b) return false;
                        }
                        return true;
                    };

                    std::string hex;
                    if (startsWithI("U+")) hex = s.substr(2);
                    else if (startsWithI("\\\\u") || startsWithI("\\\\U")) hex = s.substr(2);
                    else if (startsWithI("0X")) hex = s.substr(2);

                    if (!hex.empty()) {
                        if (!hex.empty() && hex.front() == '{' && hex.back() == '}') hex = hex.substr(1, hex.size() - 2);
                        try {
                            size_t idx = 0;
                            unsigned long v = std::stoul(hex, &idx, 16);
                            if (idx == 0) return false;
                            if (!isValidUnicodeScalar((uint32_t)v)) return false;
                            outCp = (uint32_t)v;
                            return true;
                        } catch (...) {
                            return false;
                        }
                    }

                    std::wstring w = Utf8ToWide(s);
                    if (!w.empty()) {
                        uint32_t cp = 0;
                        if (w.size() >= 2 && w[0] >= 0xD800 && w[0] <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
                            cp = 0x10000u + (((uint32_t)w[0] - 0xD800u) << 10) + ((uint32_t)w[1] - 0xDC00u);
                        } else {
                            cp = (uint32_t)w[0];
                        }
                        if (isValidUnicodeScalar(cp)) {
                            outCp = cp;
                            return true;
                        }
                    }

                    try {
                        size_t idx = 0;
                        unsigned long v = std::stoul(s, &idx, 16);
                        if (idx == 0) return false;
                        if (!isValidUnicodeScalar((uint32_t)v)) return false;
                        outCp = (uint32_t)v;
                        return true;
                    } catch (...) {
                        return false;
                    }
                };

                auto syncUnicodeEditBuffers = [&]() {
                    if (s_rebindUnicodeTextEdit.size() != g_config.keyRebinds.rebinds.size()) {
                        s_rebindUnicodeTextEdit.resize(g_config.keyRebinds.rebinds.size());
                    }
                };

                static bool s_keyboardLayoutOpen = false;
                static float s_keyboardLayoutScale = 1.45f;
                static bool s_layoutEscapeRequiresRelease = false;
                static bool s_layoutContextSplitMode = false;
                static bool s_layoutShiftUppercaseDefault = true;
                static bool s_layoutContextPopupWasOpenLastFrame = false;
                static bool s_keyboardLayoutWasOpenLastFrame = false;
                static ImVec2 s_lastKeyboardLayoutWindowSize = ImVec2(-1.0f, -1.0f);
                static float s_lastKeyboardLayoutGuiScaleFactor = -1.0f;
                if (s_layoutEscapeRequiresRelease && !ImGui::IsKeyDown(ImGuiKey_Escape)) {
                    s_layoutEscapeRequiresRelease = false;
                }

                if (ImGui::Button(trc("inputs.open_keyboard_layout"))) { s_keyboardLayoutOpen = true; }
                ImGui::SameLine();
                HelpMarker(trc("inputs.tooltip.open_keyboard_layout"));

                if (s_keyboardLayoutOpen) {
                    const ImGuiViewport* vp = ImGui::GetMainViewport();
                    ImVec2 target = vp ? vp->WorkSize : ImVec2(1400.0f, 800.0f);
                    target.x *= 0.70f;
                    target.y *= 0.62f;
                    if (target.x < 920.0f) target.x = 920.0f;
                    if (target.y < 560.0f) target.y = 560.0f;
                    ImGui::SetNextWindowSize(target, ImGuiCond_Appearing);
                    const ImVec2 center = vp ? ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f)
                                              : ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
                    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                }

                auto scanCodeToDisplayName = [&](DWORD scan, DWORD fallbackVk) -> std::string {
                        if (scan == 0) {
                            if (fallbackVk == VK_LBUTTON || fallbackVk == VK_RBUTTON || fallbackVk == VK_MBUTTON || fallbackVk == VK_XBUTTON1 ||
                                fallbackVk == VK_XBUTTON2) {
                                return VkToString(fallbackVk);
                            }
                            return std::string(tr("inputs.keyboard_layout_unbound"));
                        }

                    const DWORD scanLow = (scan & 0xFF);
                    if (scanLow == 0x45) {
                        if (fallbackVk == VK_NUMLOCK) return VkToString(VK_NUMLOCK);
                        if (fallbackVk == VK_PAUSE) return VkToString(VK_PAUSE);
                    }

                    DWORD scanDisplayVK = MapVirtualKey(scan, MAPVK_VSC_TO_VK_EX);
                    if (scanDisplayVK != 0) {
                        if (scanLow == 0x45 && (fallbackVk == VK_NUMLOCK || fallbackVk == VK_PAUSE) &&
                            (scanDisplayVK == VK_NUMLOCK || scanDisplayVK == VK_PAUSE) && scanDisplayVK != fallbackVk) {
                            scanDisplayVK = fallbackVk;
                        }
                        return VkToString(scanDisplayVK);
                    }

                    LONG keyNameLParam = static_cast<LONG>((scan & 0xFF) << 16);
                    if ((scan & 0xFF00) != 0) { keyNameLParam |= (1 << 24); }
                    char keyName[64] = {};
                    if (GetKeyNameTextA(keyNameLParam, keyName, sizeof(keyName)) > 0) { return std::string(keyName); }
                    return std::string(tr("inputs.keyboard_layout_unknown"));
                };

                ImGui::SetNextWindowBgAlpha(1.0f);
                if (s_keyboardLayoutOpen) {
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 19, 22, 255));
                    const bool keyboardLayoutWindowVisible =
                        ImGui::Begin(trc("inputs.keyboard_layout"), &s_keyboardLayoutOpen,
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoCollapse);
                    if (keyboardLayoutWindowVisible) {
                    MarkRebindBindingActive();
                    const bool keyboardLayoutOpenedThisFrame = ImGui::IsWindowAppearing() || !s_keyboardLayoutWasOpenLastFrame;
                    const ImVec2 keyboardLayoutWindowSize = ImGui::GetWindowSize();
                    const float keyboardLayoutGuiScaleFactor = ComputeGuiScaleFactorFromCachedWindowSize();
                    const bool keyboardLayoutWindowResized = fabsf(keyboardLayoutWindowSize.x - s_lastKeyboardLayoutWindowSize.x) > 0.5f ||
                                                           fabsf(keyboardLayoutWindowSize.y - s_lastKeyboardLayoutWindowSize.y) > 0.5f;
                    const bool keyboardLayoutGuiScaleChanged = s_lastKeyboardLayoutGuiScaleFactor < 0.0f ||
                                                               fabsf(keyboardLayoutGuiScaleFactor - s_lastKeyboardLayoutGuiScaleFactor) > 0.001f;
                    bool keyboardLayoutScaleChanged = false;
                    const bool escapePressedThisFrame = ImGui::IsKeyPressed(ImGuiKey_Escape);
                    bool layoutEscapeConsumedThisFrame = false;

                    const bool anyPopupOpenNow = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
                    const bool contextPopupOpenNow = ImGui::IsPopupOpen(trc("inputs.rebind_config")) || ImGui::IsPopupOpen(trc("inputs.triggers_custom")) ||
                                                     ImGui::IsPopupOpen(trc("inputs.custom_unicode"));
                    bool blockLayoutEscapeThisFrame = false;
                    if (escapePressedThisFrame && (anyPopupOpenNow || contextPopupOpenNow || s_layoutContextPopupWasOpenLastFrame)) {
                        s_layoutEscapeRequiresRelease = true;
                        blockLayoutEscapeThisFrame = true;
                        layoutEscapeConsumedThisFrame = true;
                    }

                    if (s_layoutEscapeRequiresRelease) {
                        blockLayoutEscapeThisFrame = true;
                        layoutEscapeConsumedThisFrame = true;
                    }

                    {
                        float scalePct = s_keyboardLayoutScale * 100.0f;
                        ImGui::TextUnformatted(trc("inputs.keyboard_layout_scale"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(220.0f);
                        if (ImGui::SliderFloat("##keyboardLayoutScalePct", &scalePct, 60.0f, 300.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                            s_keyboardLayoutScale = scalePct / 100.0f;
                            keyboardLayoutScaleChanged = true;
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("inputs.tooltip.keyboard_layout_scale"));

                        ImGui::TextDisabled(trc("inputs.keyboard_layout_tip"));
                        ImGui::TextDisabled(trc("label.not_all_rebinds_supported"));

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                    }

                    // Make the keyboard region scrollable (both axes) to fit on smaller windows.
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(14, 15, 18, 255));
                    ImGui::BeginChild("##keyboardLayoutChild", ImVec2(0, 0), true,
                                     ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);

                    struct KeyCell {
                        DWORD vk;
                        const char* labelOverride;
                        float w;
                    };

                    auto Spacer = [](float wUnits) -> KeyCell { return KeyCell{ 0, nullptr, wUnits }; };
                    auto Key = [](DWORD vk, float wUnits = 1.0f, const char* overrideLabel = nullptr) -> KeyCell {
                        return KeyCell{ vk, overrideLabel, wUnits };
                    };

                    const std::vector<std::vector<KeyCell>> rows = {
                                                { Key(VK_ESCAPE), Spacer(1.0f), Key(VK_F1), Key(VK_F2), Key(VK_F3), Key(VK_F4), Spacer(0.5f), Key(VK_F5),
                          Key(VK_F6), Key(VK_F7), Key(VK_F8), Spacer(0.5f), Key(VK_F9), Key(VK_F10), Key(VK_F11), Key(VK_F12),
                                                    Spacer(0.5f), Key(VK_SNAPSHOT, 1.25f, "PRTSC"), Key(VK_SCROLL, 1.25f, "SCRLK"), Key(VK_PAUSE, 1.25f, "PAUSE") },

                        { Key(VK_OEM_3, 1.0f, "`") , Key('1'), Key('2'), Key('3'), Key('4'), Key('5'), Key('6'), Key('7'), Key('8'), Key('9'),
                          Key('0'), Key(VK_OEM_MINUS, 1.0f, "-"), Key(VK_OEM_PLUS, 1.0f, "="), Key(VK_BACK, 2.0f, "BACK"), Spacer(0.5f),
                                                    Key(VK_INSERT, 1.25f, "INS"), Key(VK_HOME, 1.25f, "HOME"), Key(VK_PRIOR, 1.25f, "PGUP"), Spacer(0.5f),
                                                    Key(VK_NUMLOCK, 1.25f, "NUMLK"), Key(VK_DIVIDE, 1.25f, "/"), Key(VK_MULTIPLY, 1.25f, "*"), Key(VK_SUBTRACT, 1.25f, "-") },

                        { Key(VK_TAB, 1.5f, "TAB"), Key('Q'), Key('W'), Key('E'), Key('R'), Key('T'), Key('Y'), Key('U'), Key('I'), Key('O'), Key('P'),
                          Key(VK_OEM_4, 1.0f, "["), Key(VK_OEM_6, 1.0f, "]"), Key(VK_OEM_5, 1.5f, "\\"), Spacer(0.5f),
                                                    Key(VK_DELETE, 1.25f, "DEL"), Key(VK_END, 1.25f, "END"), Key(VK_NEXT, 1.25f, "PGDN"), Spacer(0.5f),
                                                    Key(VK_NUMPAD7, 1.25f, "NUM7"), Key(VK_NUMPAD8, 1.25f, "NUM8"), Key(VK_NUMPAD9, 1.25f, "NUM9"), Key(VK_ADD, 1.25f, "+") },

                        { Key(VK_CAPITAL, 1.75f, "CAPS"), Key('A'), Key('S'), Key('D'), Key('F'), Key('G'), Key('H'), Key('J'), Key('K'), Key('L'),
                          Key(VK_OEM_1, 1.0f, ";"), Key(VK_OEM_7, 1.0f, "'"), Key(VK_RETURN, 2.25f, "ENTER"), Spacer(0.5f),
                                                                                                        Spacer(1.25f), Spacer(1.25f), Spacer(1.25f), Spacer(0.5f),
                                                                                                        Key(VK_NUMPAD4, 1.25f, "NUM4"), Key(VK_NUMPAD5, 1.25f, "NUM5"), Key(VK_NUMPAD6, 1.25f, "NUM6"),
                                                    Spacer(1.25f) },

                        { Key(VK_LSHIFT, 2.25f, "LSHIFT"), Key('Z'), Key('X'), Key('C'), Key('V'), Key('B'), Key('N'), Key('M'),
                          Key(VK_OEM_COMMA, 1.0f, ","), Key(VK_OEM_PERIOD, 1.0f, "."), Key(VK_OEM_2, 1.0f, "/"), Key(VK_RSHIFT, 2.75f, "RSHIFT"),
                                                    Spacer(0.5f), Spacer(1.25f), Key(VK_UP, 1.25f, "UP"), Spacer(1.25f), Spacer(0.5f),
                                                    Key(VK_NUMPAD1, 1.25f, "NUM1"), Key(VK_NUMPAD2, 1.25f, "NUM2"), Key(VK_NUMPAD3, 1.25f, "NUM3"), Key(VK_RETURN, 1.25f, "ENTER") },

                                                { Key(VK_LCONTROL, 1.25f, "LCTRL"), Key(VK_LWIN, 1.25f, "LWIN"), Key(VK_LMENU, 1.25f, "LALT"),
                                                    Key(VK_SPACE, 6.25f, "SPACE"), Key(VK_RMENU, 1.25f, "RALT"), Key(VK_RWIN, 1.25f, "RWIN"), Key(VK_APPS, 1.25f, "APPS"),
                                                    Key(VK_RCONTROL, 1.25f, "RCTRL"), Spacer(0.5f), Key(VK_LEFT, 1.25f, "LEFT"), Key(VK_DOWN, 1.25f, "DOWN"), Key(VK_RIGHT, 1.25f, "RIGHT"),
                                                    Spacer(0.5f), Key(VK_NUMPAD0, 2.5f, "NUM0"), Key(VK_DECIMAL, 1.25f, "NUM."), Spacer(1.25f) },
                    };

                    auto findRebindForKey = [&](DWORD fromVk) -> const KeyRebind* {
                        const KeyRebind* first = nullptr;
                        const KeyRebind* enabledAny = nullptr;
                        const KeyRebind* enabledConfigured = nullptr;
                        const KeyRebind* configuredAny = nullptr;

                        for (const auto& r : g_config.keyRebinds.rebinds) {
                            if (r.fromKey != fromVk) continue;
                            if (!first) first = &r;

                            const bool configured = (r.fromKey != 0 && r.toKey != 0);
                            if (configured && !configuredAny) configuredAny = &r;
                            if (r.enabled && !enabledAny) enabledAny = &r;
                            if (r.enabled && configured) {
                                enabledConfigured = &r;
                                break;
                            }
                        }

                        if (enabledConfigured) return enabledConfigured;
                        if (configuredAny) return configuredAny;
                        if (enabledAny) return enabledAny;
                        return first;
                    };

                    auto roundPx = [](float v) -> float { return (float)(int)(v + 0.5f); };

                    constexpr float kKeyboardScaleMult = 1.0f;
                    constexpr float kKeyHeightMul = 1.68f;
                    constexpr float kKeyUnitMul = 0.96f;
                    constexpr float kKeyGapMul = 1.08f;
                    constexpr float kKeyCapInsetXMul = 0.55f;
                    constexpr float kKeyCapInsetYMul = 0.45f;
                    constexpr float kKeyRoundingPx = 5.0f;

                    const float keyboardScale = s_keyboardLayoutScale * kKeyboardScaleMult;

                    const float keyH = roundPx(ImGui::GetFrameHeight() * kKeyHeightMul * keyboardScale);
                    float unit = roundPx(keyH * kKeyUnitMul);
                    unit = (float)(((int)(unit + 2.0f) / 4) * 4);
                    if (unit < 20.0f) unit = 20.0f;
                    float gap = roundPx(ImGui::GetStyle().ItemInnerSpacing.x * keyboardScale * kKeyGapMul);
                    if (gap < 1.0f) gap = 1.0f;
                    if (keyboardLayoutOpenedThisFrame || keyboardLayoutWindowResized || keyboardLayoutScaleChanged ||
                        keyboardLayoutGuiScaleChanged) {
                        RequestKeyboardLayoutFontRefresh(keyboardLayoutWindowSize, keyH, keyboardScale,
                                                         keyboardLayoutOpenedThisFrame || keyboardLayoutScaleChanged);
                        s_lastKeyboardLayoutWindowSize = keyboardLayoutWindowSize;
                    }
                    s_lastKeyboardLayoutGuiScaleFactor = keyboardLayoutGuiScaleFactor;
                    const float rounding = roundPx(kKeyRoundingPx * keyboardScale);
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    const float pitchX = unit;
                    const float keyPadX = roundPx(gap * kKeyCapInsetXMul);
                    const float keyPadY = roundPx(gap * kKeyCapInsetYMul);

                    float keyboardMaxRowW = 0.0f;
                    for (const auto& row : rows) {
                        float w = 0.0f;
                        for (size_t c = 0; c < row.size(); ++c) {
                            w += row[c].w * pitchX;
                        }
                        if (w > keyboardMaxRowW) keyboardMaxRowW = w;
                    }

                    const ImVec2 layoutStart = ImGui::GetCursorPos();
                    const ImVec2 layoutStartScreen = ImGui::GetCursorScreenPos();
                    const float keyboardTotalH = (float)rows.size() * (keyH + gap);

                    {
                        const float platePad = 10.0f * keyboardScale;
                        const float plateRound = 10.0f * keyboardScale;
                        const ImVec2 plateMin = ImVec2(layoutStartScreen.x - platePad, layoutStartScreen.y - platePad);
                        const ImVec2 plateMax =
                            ImVec2(layoutStartScreen.x + keyboardMaxRowW + platePad, layoutStartScreen.y + keyboardTotalH - gap + platePad);

                        dl->AddRectFilled(ImVec2(plateMin.x + 5, plateMin.y + 6), ImVec2(plateMax.x + 5, plateMax.y + 6),
                                          IM_COL32(0, 0, 0, 130), plateRound);

                        const ImU32 plateTop = IM_COL32(35, 38, 46, 255);
                        const ImU32 plateBot = IM_COL32(18, 20, 26, 255);
                        dl->AddRectFilled(plateMin, plateMax, plateBot, plateRound);
                        const float plateSplit = 0.55f;
                        const ImVec2 plateMid = ImVec2(plateMax.x, plateMin.y + (plateMax.y - plateMin.y) * plateSplit);
                        dl->AddRectFilled(plateMin, plateMid, plateTop, plateRound, ImDrawFlags_RoundCornersTop);
                        dl->AddRect(plateMin, plateMax, IM_COL32(10, 10, 12, 255), plateRound);
                        dl->AddLine(ImVec2(plateMin.x + 6, plateMin.y + 6), ImVec2(plateMax.x - 6, plateMin.y + 6), IM_COL32(255, 255, 255, 25),
                                    1.0f);
                    }

                    static DWORD s_layoutContextVk = 0;
                    static int s_layoutContextPreferredIndex = -1;

                    const ImGuiID rebindPopupId = ImGui::GetID(trc("inputs.rebind_config"));
                    auto openRebindContextFor = [&](DWORD vk) {
                        s_layoutContextVk = vk;
                        s_layoutContextPreferredIndex = -1;
                        ImGui::OpenPopup(rebindPopupId);
                    };

                    auto normalizeMouseButtonLabel = [](std::string s) -> std::string {
                        if (s == "MOUSE1") return "MB1";
                        if (s == "MOUSE2") return "MB2";
                        if (s == "MOUSE3") return "MB3";
                        if (s == "MOUSE4") return "MB4";
                        if (s == "MOUSE5") return "MB5";
                        return s;
                    };

                    auto isModifierVk = [](DWORD vk) -> bool {
                        return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT ||
                               vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
                    };

                    auto isMouseButtonVk = [](DWORD vk) -> bool {
                        return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
                    };

                    auto hasShiftLayerOverride = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                        if (!rb || !rb->shiftLayerEnabled) return false;
                        if (rb->shiftLayerOutputUnicode != 0) return true;
                        if (rb->shiftLayerOutputVK == 0) return false;
                        if (rb->useCustomOutput && rb->customOutputUnicode != 0) return true;

                        DWORD baseTextVk = (rb->useCustomOutput && rb->customOutputVK != 0) ? rb->customOutputVK : originalVk;
                        if (baseTextVk == 0) baseTextVk = originalVk;
                        if (rb->shiftLayerOutputVK != baseTextVk) return true;
                        // Keep explicit Shift-layer settings visible/configured even when VK matches base output.
                        return true;
                    };

                    auto resolveTypesVkFor = [&](const KeyRebind* rb, DWORD originalVk, bool useShiftLayer) -> DWORD {
                        DWORD textVk = (rb && rb->useCustomOutput && rb->customOutputVK != 0) ? rb->customOutputVK : originalVk;
                        if (rb && useShiftLayer && rb->shiftLayerEnabled && rb->shiftLayerOutputVK != 0) {
                            textVk = rb->shiftLayerOutputVK;
                        }
                        if (textVk == 0) textVk = originalVk;
                        return textVk;
                    };

                    auto resolveTriggerVkFor = [&](const KeyRebind* rb, DWORD originalVk) -> DWORD {
                        DWORD triggerVk = rb ? rb->toKey : originalVk;
                        if (triggerVk == 0) triggerVk = originalVk;
                        return triggerVk;
                    };

                    auto resolveTriggerScanFor = [&](const KeyRebind* rb, DWORD originalVk) -> DWORD {
                        const DWORD triggerVk = resolveTriggerVkFor(rb, originalVk);
                        DWORD displayScan = (rb && rb->useCustomOutput && rb->customOutputScanCode != 0) ? rb->customOutputScanCode
                                                                                                            : getScanCodeWithExtendedFlag(triggerVk);
                        if (displayScan != 0 && (displayScan & 0xFF00) == 0) {
                            DWORD derived = getScanCodeWithExtendedFlag(triggerVk);
                            if ((derived & 0xFF00) != 0 && ((derived & 0xFF) == (displayScan & 0xFF))) { displayScan = derived; }
                        }
                        return displayScan;
                    };

                    auto isModifierTriggerScan = [&](DWORD triggerScan, DWORD triggerVk) -> bool {
                        if ((triggerScan & 0xFF) == 0) return false;

                        DWORD scanVk = MapVirtualKey(triggerScan, MAPVK_VSC_TO_VK_EX);
                        if (scanVk == 0 && (triggerScan & 0xFF00) != 0) {
                            scanVk = MapVirtualKey((triggerScan & 0xFF), MAPVK_VSC_TO_VK_EX);
                        }
                        if (scanVk == 0) scanVk = triggerVk;

                        return isModifierVk(scanVk);
                    };

                    auto cannotTypeFor = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                        if (!rb) return false;
                        const DWORD triggerVk = resolveTriggerVkFor(rb, originalVk);
                        const DWORD triggerScan = resolveTriggerScanFor(rb, originalVk);
                        if (isModifierTriggerScan(triggerScan, triggerVk)) return true;
                        if (isMouseButtonVk(triggerVk)) return true;
                        if (triggerVk == VK_BACK || triggerVk == VK_CAPITAL) return true;
                        if (triggerVk == VK_DELETE || triggerVk == VK_HOME || triggerVk == VK_INSERT ||
                            triggerVk == VK_END || triggerVk == VK_PRIOR || triggerVk == VK_NEXT) return true;
                        return false;
                    };

                    auto typesValueForDisplay = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                        if (!rb) return VkToString(originalVk);
                        if (cannotTypeFor(rb, originalVk)) return tr("inputs.cannot_type");
                        if (rb->useCustomOutput && rb->customOutputUnicode != 0) return codepointToDisplay((uint32_t)rb->customOutputUnicode);

                        DWORD textVk = resolveTypesVkFor(rb, originalVk, false);
                        return normalizeMouseButtonLabel(VkToString(textVk));
                    };

                    auto typesShiftValueForDisplay = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                        if (!rb) return VkToString(originalVk);
                        if (!hasShiftLayerOverride(rb, originalVk)) return typesValueForDisplay(rb, originalVk);
                        if (rb->shiftLayerEnabled && rb->shiftLayerOutputUnicode != 0) {
                            return codepointToDisplay((uint32_t)rb->shiftLayerOutputUnicode);
                        }

                        DWORD textVk = resolveTypesVkFor(rb, originalVk, true);
                        return normalizeMouseButtonLabel(VkToString(textVk));
                    };

                    auto drawKeyCell = [&](DWORD vk, const char* label, const ImVec2& pMin, const ImVec2& pMax, const KeyRebind* rb) {
                        const bool hovered = ImGui::IsItemHovered();
                        const bool active = ImGui::IsItemActive();

                        struct KeyTheme {
                            ImU32 top;
                            ImU32 bottom;
                            ImU32 border;
                            ImU32 text;
                        };

                        auto clamp255 = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                        auto adjust = [&](ImU32 c, int add) -> ImU32 {
                            int r = (int)(c & 0xFF);
                            int g = (int)((c >> 8) & 0xFF);
                            int b = (int)((c >> 16) & 0xFF);
                            int a = (int)((c >> 24) & 0xFF);
                            r = clamp255(r + add);
                            g = clamp255(g + add);
                            b = clamp255(b + add);
                            return (ImU32)(r | (g << 8) | (b << 16) | (a << 24));
                        };

                        auto themeForVk = [&](DWORD v) -> KeyTheme {
                            const bool isMouse = (v == VK_LBUTTON || v == VK_RBUTTON || v == VK_MBUTTON || v == VK_XBUTTON1 || v == VK_XBUTTON2);
                            const bool isFn = (v == VK_ESCAPE || (v >= VK_F1 && v <= VK_F24) || v == VK_SNAPSHOT || v == VK_SCROLL || v == VK_PAUSE);
                            const bool isMod = (v == VK_SHIFT || v == VK_LSHIFT || v == VK_RSHIFT || v == VK_CONTROL || v == VK_LCONTROL ||
                                                v == VK_RCONTROL || v == VK_MENU || v == VK_LMENU || v == VK_RMENU || v == VK_LWIN || v == VK_RWIN ||
                                                v == VK_TAB || v == VK_CAPITAL || v == VK_BACK || v == VK_RETURN || v == VK_SPACE || v == VK_APPS);
                            const bool isNav = (v == VK_INSERT || v == VK_DELETE || v == VK_HOME || v == VK_END || v == VK_PRIOR || v == VK_NEXT ||
                                                v == VK_UP || v == VK_DOWN || v == VK_LEFT || v == VK_RIGHT);
                            const bool isNum = (v >= VK_NUMPAD0 && v <= VK_NUMPAD9) || v == VK_ADD || v == VK_SUBTRACT || v == VK_MULTIPLY ||
                                                v == VK_DIVIDE || v == VK_DECIMAL || v == VK_NUMLOCK;

                            KeyTheme t{ IM_COL32(80, 86, 98, 255), IM_COL32(45, 49, 60, 255), IM_COL32(18, 18, 20, 255),
                                        IM_COL32(245, 245, 245, 255) };
                            if (isMod) {
                                t.top = IM_COL32(72, 78, 92, 255);
                                t.bottom = IM_COL32(38, 42, 52, 255);
                            }
                            if (isFn) {
                                t.top = IM_COL32(92, 80, 74, 255);
                                t.bottom = IM_COL32(52, 42, 38, 255);
                            }
                            if (isNav) {
                                t.top = IM_COL32(78, 86, 104, 255);
                                t.bottom = IM_COL32(40, 45, 56, 255);
                            }
                            if (isNum) {
                                t.top = IM_COL32(72, 88, 90, 255);
                                t.bottom = IM_COL32(36, 46, 48, 255);
                            }
                            if (isMouse) {
                                t.top = IM_COL32(88, 90, 108, 255);
                                t.bottom = IM_COL32(44, 46, 60, 255);
                            }
                            return t;
                        };

                        KeyTheme theme = themeForVk(vk);
                        theme.bottom = adjust(theme.bottom, 10);
                        if (hovered) {
                            theme.top = adjust(theme.top, 12);
                            theme.bottom = adjust(theme.bottom, 10);
                        }
                        if (active) {
                            theme.top = adjust(theme.top, -8);
                            theme.bottom = adjust(theme.bottom, -16);
                        }

                        const ImVec2 size = ImVec2(pMax.x - pMin.x, pMax.y - pMin.y);
                        const float shadow = 2.0f * keyboardScale;
                        const ImVec2 pressOff = active ? ImVec2(0.0f, 1.2f * keyboardScale) : ImVec2(0, 0);

                        dl->AddRectFilled(ImVec2(pMin.x + shadow, pMin.y + shadow), ImVec2(pMax.x + shadow, pMax.y + shadow),
                                          IM_COL32(0, 0, 0, 90), rounding);

                        const ImVec2 kMin = ImVec2(pMin.x + pressOff.x, pMin.y + pressOff.y);
                        const ImVec2 kMax = ImVec2(pMax.x + pressOff.x, pMax.y + pressOff.y);

                        dl->AddRectFilled(kMin, kMax, theme.bottom, rounding);
                        const float split = 0.70f;
                        const ImVec2 kMid = ImVec2(kMax.x, kMin.y + (kMax.y - kMin.y) * split);
                        dl->AddRectFilled(kMin, kMid, theme.top, rounding, ImDrawFlags_RoundCornersTop);

                        dl->AddLine(ImVec2(kMin.x + 2, kMin.y + 1), ImVec2(kMax.x - 2, kMin.y + 1), IM_COL32(255, 255, 255, 18), 1.0f);

                        dl->AddRect(kMin, kMax, theme.border, rounding, 0, 1.0f);

                        if (rb && rb->fromKey != 0 && rb->toKey != 0) {
                            const ImU32 outline = rb->enabled ? IM_COL32(0, 220, 110, 255) : IM_COL32(255, 170, 0, 255);
                            dl->AddRect(kMin, kMax, outline, rounding, 0, 3.0f);
                            const ImU32 tint = rb->enabled ? IM_COL32(0, 190, 95, 30) : IM_COL32(255, 165, 0, 35);
                            dl->AddRectFilled(kMin, kMax, tint, rounding);
                        }

                        const float padX = 6.0f * keyboardScale;
                        const float padY = 4.0f * keyboardScale;

                        const bool hasConfigured = (rb && rb->fromKey != 0 && rb->toKey != 0);
                        const bool isNoOp = [&]() -> bool {
                            if (!hasConfigured) return false;
                            if (rb->toKey != vk) return false;
                            if (rb->customOutputVK != 0 && rb->customOutputVK != rb->toKey) return false;
                            if (rb->customOutputUnicode != 0) return false;
                            if (rb->customOutputScanCode != 0) return false;
                            if (rb->baseOutputShifted) return false;
                            if (hasShiftLayerOverride(rb, vk)) return false;
                            return true;
                        }();
                        const bool showRebindInfo = hasConfigured && !isNoOp;

                        DWORD triggerVK = showRebindInfo ? resolveTriggerVkFor(rb, vk) : vk;
                        DWORD outScan = showRebindInfo ? resolveTriggerScanFor(rb, vk) : 0;

                        const std::string primaryText = showRebindInfo ? typesValueForDisplay(rb, vk) : std::string(label);
                        const std::string secondaryText = showRebindInfo ? normalizeMouseButtonLabel(scanCodeToDisplayName(outScan, triggerVK))
                                                                          : std::string();
                        const bool showShiftLayerText = showRebindInfo && hasShiftLayerOverride(rb, vk);
                        const std::string shiftLayerText =
                            showShiftLayerText ? normalizeMouseButtonLabel(typesShiftValueForDisplay(rb, vk)) : std::string();
                        const bool showSecondaryText = showRebindInfo && !secondaryText.empty();

                        ImFont* fLabel = g_keyboardLayoutPrimaryFont ? g_keyboardLayoutPrimaryFont : ImGui::GetFont();
                        ImFont* fSecondary = g_keyboardLayoutSecondaryFont ? g_keyboardLayoutSecondaryFont : fLabel;
                        auto snapPxText = [](float v) -> float { return floorf(v + 0.5f); };
                        auto snapFontSize = [](float v) -> float {
                            float s = floorf(v + 0.5f);
                            if (s < 8.0f) s = 8.0f;
                            return s;
                        };
                        auto fitTextToWidth = [&](ImFont* font, float desiredFontSize, const std::string& text, float availWidth,
                                                  float minFontSize) {
                            float fontSize = desiredFontSize;
                            ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());

                            if (availWidth > 1.0f && textSize.x > availWidth) {
                                const float widthScale = availWidth / (textSize.x + 0.001f);
                                fontSize = desiredFontSize * widthScale;
                                if (fontSize < minFontSize) fontSize = minFontSize;
                                textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());

                                if (textSize.x > availWidth && fontSize > minFontSize) {
                                    const float exactScale = availWidth / (textSize.x + 0.001f);
                                    float refinedFontSize = fontSize * exactScale;
                                    if (refinedFontSize < minFontSize) refinedFontSize = minFontSize;
                                    if (refinedFontSize < fontSize) {
                                        fontSize = refinedFontSize;
                                        textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
                                    }
                                }
                            }

                            return std::pair<float, ImVec2>(fontSize, textSize);
                        };
                        auto centerTextXWithin = [&](float minX, float maxX, float textWidth) {
                            const float availWidth = maxX - minX;
                            float x = snapPxText(minX + (availWidth - textWidth) * 0.5f);
                            const float maxStartX = maxX - textWidth;
                            if (maxStartX >= minX) {
                                if (x < minX) x = snapPxText(minX);
                                if (x > maxStartX) x = snapPxText(maxStartX);
                            }
                            return x;
                        };

                        const float textAvailW = size.x - padX * 2.0f;
                        const float textAvailH = size.y - padY * 2.0f;
                        const float textMinX = kMin.x + padX;
                        const float textMaxX = kMax.x - padX;
                        float layoutTextBoost = 0.85f + 0.35f * s_keyboardLayoutScale;
                        if (layoutTextBoost < 0.85f) layoutTextBoost = 0.85f;
                        if (layoutTextBoost > 1.85f) layoutTextBoost = 1.85f;

                        float shiftLayerFs = 0.0f;
                        ImVec2 shiftLayerSz = ImVec2(0.0f, 0.0f);
                        float shiftLayerBodyOffset = 0.0f;
                        float shiftLayerBodyMinX = textMinX;
                        if (showShiftLayerText) {
                            shiftLayerFs = snapFontSize(fSecondary->LegacySize * (0.70f + 0.10f * s_keyboardLayoutScale));
                            shiftLayerSz = fSecondary->CalcTextSizeA(shiftLayerFs, FLT_MAX, 0.0f, shiftLayerText.c_str());
                            const float shiftAvailW = textAvailW * (showSecondaryText ? 0.42f : 0.50f);
                            if (shiftAvailW > 6.0f) {
                                const auto fittedShift = fitTextToWidth(fSecondary, shiftLayerFs, shiftLayerText, shiftAvailW, 6.0f);
                                shiftLayerFs = fittedShift.first;
                                shiftLayerSz = fittedShift.second;
                            }

                            const float bodyOffsetCap = textAvailH * (showSecondaryText ? 0.10f : 0.06f);
                            if (bodyOffsetCap > 0.0f && shiftLayerSz.y > 0.0f) {
                                shiftLayerBodyOffset = snapPxText(shiftLayerSz.y * (showSecondaryText ? 0.28f : 0.18f));
                                if (shiftLayerBodyOffset > bodyOffsetCap) shiftLayerBodyOffset = snapPxText(bodyOffsetCap);
                            }

                            const float shiftTextGapX = snapPxText((showSecondaryText ? 4.0f : 5.0f) * keyboardScale);
                            const float candidateBodyMinX = snapPxText(textMinX + shiftLayerSz.x + shiftTextGapX);
                            const float maxBodyMinX = textMaxX - 6.0f;
                            if (candidateBodyMinX < maxBodyMinX) shiftLayerBodyMinX = candidateBodyMinX;
                        }

                        const float bodyTextAvailH = (shiftLayerBodyOffset < textAvailH) ? (textAvailH - shiftLayerBodyOffset) : textAvailH;
                        const float bodyTextTopY = kMin.y + padY + ((shiftLayerBodyOffset < textAvailH) ? shiftLayerBodyOffset : 0.0f);
                        const float bodyTextMinX = showShiftLayerText ? shiftLayerBodyMinX : textMinX;
                        const float bodyTextAvailW = textMaxX - bodyTextMinX;

                        float labelFontSize = fLabel->LegacySize * layoutTextBoost;
                        ImVec2 labelSz = fLabel->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, primaryText.c_str());

                        if (bodyTextAvailW > 8.0f) {
                            const auto fittedPrimary = fitTextToWidth(fLabel, snapFontSize(labelFontSize), primaryText, bodyTextAvailW, 6.0f);
                            labelFontSize = fittedPrimary.first;
                            labelSz = fittedPrimary.second;
                        }

                        const ImU32 shiftCol = (rb && !rb->enabled) ? IM_COL32(255, 220, 170, 215) : IM_COL32(220, 238, 255, 220);
                        const ImU32 infoCol = (rb && !rb->enabled) ? IM_COL32(255, 220, 170, 235) : IM_COL32(245, 245, 245, 235);

                        if (showShiftLayerText && showSecondaryText) {
                            ImFont* fTopRow = fLabel;
                            const float topRowBoost = 1.12f;
                            const float pairTopPadY = snapPxText((padY * 0.40f) - (0.5f * keyboardScale));
                            const float pairBottomPadY = snapPxText(padY * 0.55f);
                            const float pairTextAvailH = size.y - pairTopPadY - pairBottomPadY;

                            float topRowFs = snapFontSize(fTopRow->LegacySize * layoutTextBoost * topRowBoost);
                            ImVec2 shiftSz = ImVec2(0.0f, 0.0f);
                            ImVec2 primarySz = ImVec2(0.0f, 0.0f);
                            auto recalcTopPair = [&]() {
                                shiftSz = fTopRow->CalcTextSizeA(topRowFs, FLT_MAX, 0.0f, shiftLayerText.c_str());
                                primarySz = fTopRow->CalcTextSizeA(topRowFs, FLT_MAX, 0.0f, primaryText.c_str());
                            };
                            recalcTopPair();

                            float secondaryFs = snapFontSize(fSecondary->LegacySize * layoutTextBoost);
                            ImVec2 secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                            if (textAvailW > 8.0f) {
                                const auto fittedSecondary =
                                    fitTextToWidth(fSecondary, secondaryFs, secondaryText, textAvailW, 6.0f);
                                secondaryFs = fittedSecondary.first;
                                secondarySz = fittedSecondary.second;
                            }

                            float topGapX = snapPxText(2.0f * keyboardScale);
                            if (topGapX < 1.0f) topGapX = 1.0f;

                            auto fitTopPair = [&]() {
                                float combinedW = shiftSz.x + topGapX + primarySz.x;
                                if (textAvailW > 8.0f && combinedW > textAvailW) {
                                    float fit = textAvailW / (combinedW + 0.001f);
                                    if (fit < 1.0f) {
                                        topRowFs = (topRowFs * fit < 6.0f) ? 6.0f : topRowFs * fit;
                                        recalcTopPair();
                                        combinedW = shiftSz.x + topGapX + primarySz.x;
                                    }

                                    if (combinedW > textAvailW) {
                                        const float widestTopLabel = (shiftSz.x > primarySz.x) ? shiftSz.x : primarySz.x;
                                        if (widestTopLabel > 0.0f) {
                                            const float maxWidthFit = (textAvailW - topGapX) / (widestTopLabel + 0.001f);
                                            if (maxWidthFit < 1.0f) {
                                                topRowFs = (topRowFs * maxWidthFit < 6.0f) ? 6.0f : topRowFs * maxWidthFit;
                                                recalcTopPair();
                                            }
                                        }
                                    }
                                }
                            };

                            fitTopPair();

                            float lineGap = snapPxText(1.0f * keyboardScale);
                            if (lineGap < 1.0f) lineGap = 1.0f;

                            float topRowH = (shiftSz.y > primarySz.y) ? shiftSz.y : primarySz.y;
                            float totalH = topRowH + lineGap + secondarySz.y;
                            if (pairTextAvailH > 0.0f && totalH > pairTextAvailH) {
                                const float fit = pairTextAvailH / (totalH + 0.001f);
                                if (fit < 1.0f) {
                                    topRowFs = (topRowFs * fit < 6.0f) ? 6.0f : topRowFs * fit;
                                    secondaryFs = (secondaryFs * fit < 6.0f) ? 6.0f : secondaryFs * fit;

                                    recalcTopPair();
                                    if (textAvailW > 8.0f) {
                                        const auto fittedSecondary =
                                            fitTextToWidth(fSecondary, secondaryFs, secondaryText, textAvailW, 6.0f);
                                        secondaryFs = fittedSecondary.first;
                                        secondarySz = fittedSecondary.second;
                                    } else {
                                        secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                                    }

                                    fitTopPair();
                                    topRowH = (shiftSz.y > primarySz.y) ? shiftSz.y : primarySz.y;
                                    totalH = topRowH + lineGap + secondarySz.y;
                                }
                            }

                            const float combinedTopW = shiftSz.x + topGapX + primarySz.x;
                            float topY = snapPxText(kMin.y + pairTopPadY);
                            float secondaryY = snapPxText(kMax.y - pairBottomPadY - secondarySz.y);
                            if (secondaryY < topY + topRowH + lineGap) {
                                float startY = snapPxText(kMin.y + pairTopPadY + (pairTextAvailH - totalH) * 0.5f);
                                if (startY < kMin.y + pairTopPadY) startY = snapPxText(kMin.y + pairTopPadY);
                                topY = startY;
                                secondaryY = snapPxText(topY + topRowH + lineGap);
                            }

                            const float pairStartX = centerTextXWithin(textMinX, textMaxX, combinedTopW);
                            const float shiftX = pairStartX;
                            const float primaryX = snapPxText(shiftX + shiftSz.x + topGapX);
                            const float shiftY = snapPxText(topY + (topRowH - shiftSz.y) * 0.5f);
                            const float primaryY = snapPxText(topY + (topRowH - primarySz.y) * 0.5f);
                            const float secondaryX = centerTextXWithin(textMinX, textMaxX, secondarySz.x);

                            dl->AddText(fTopRow, topRowFs, ImVec2(shiftX, shiftY), shiftCol, shiftLayerText.c_str());
                            dl->AddText(fTopRow, topRowFs, ImVec2(primaryX, primaryY), theme.text, primaryText.c_str());
                            dl->AddText(fSecondary, secondaryFs, ImVec2(secondaryX, secondaryY), infoCol, secondaryText.c_str());
                        } else if (!showRebindInfo || !showSecondaryText) {
                            if (showShiftLayerText) {
                                float shiftX = snapPxText(textMinX);
                                const float shiftMaxX = textMaxX - shiftLayerSz.x;
                                if (shiftMaxX >= textMinX && shiftX > shiftMaxX) shiftX = snapPxText(shiftMaxX);

                                float shiftY = snapPxText(kMin.y + padY - 1.0f * keyboardScale);
                                const float shiftMaxY = kMax.y - padY - shiftLayerSz.y;
                                if (shiftMaxY >= kMin.y + padY && shiftY > shiftMaxY) shiftY = snapPxText(shiftMaxY);

                                dl->AddText(fSecondary, shiftLayerFs, ImVec2(shiftX, shiftY), shiftCol, shiftLayerText.c_str());
                            }

                            const float maxByHeight = bodyTextAvailH * 0.98f;
                            if (maxByHeight > 0.0f && labelFontSize > maxByHeight) {
                                labelFontSize = maxByHeight;
                                labelSz = fLabel->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, primaryText.c_str());
                                if (bodyTextAvailW > 8.0f) {
                                    const auto fittedPrimary = fitTextToWidth(fLabel, labelFontSize, primaryText, bodyTextAvailW, 6.0f);
                                    labelFontSize = fittedPrimary.first;
                                    labelSz = fittedPrimary.second;
                                }
                            }

                            float x = centerTextXWithin(bodyTextMinX, textMaxX, labelSz.x);
                            float y = snapPxText(bodyTextTopY + (bodyTextAvailH - labelSz.y) * 0.5f);
                            if (showShiftLayerText) {
                                const float shiftAlignedY = snapPxText(kMin.y + padY - 1.0f * keyboardScale + (shiftLayerSz.y - labelSz.y) * 0.5f);
                                const float shiftMaxY = kMax.y - padY - labelSz.y;
                                y = shiftAlignedY;
                                if (shiftMaxY >= bodyTextTopY && y > shiftMaxY) y = snapPxText(shiftMaxY);
                            }
                            if (y < bodyTextTopY) y = snapPxText(bodyTextTopY);
                            dl->AddText(fLabel, labelFontSize, ImVec2(x, y), theme.text, primaryText.c_str());
                        } else {
                            ImFont* f = fLabel;

                            float primaryFs = labelFontSize;
                            ImVec2 primarySz = f->CalcTextSizeA(primaryFs, FLT_MAX, 0.0f, primaryText.c_str());

                            float secondaryFs = fSecondary->LegacySize * layoutTextBoost;
                            ImVec2 secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                            if (bodyTextAvailW > 8.0f) {
                                const auto fittedSecondary =
                                    fitTextToWidth(fSecondary, snapFontSize(secondaryFs), secondaryText, bodyTextAvailW, 6.0f);
                                secondaryFs = fittedSecondary.first;
                                secondarySz = fittedSecondary.second;
                            }

                            float lineGap = snapPxText(1.0f * keyboardScale);
                            if (lineGap < 1.0f) lineGap = 1.0f;

                            float totalH = primarySz.y + lineGap + secondarySz.y;
                            if (bodyTextAvailH > 0.0f && totalH > bodyTextAvailH) {
                                float fit = bodyTextAvailH / (totalH + 0.001f);
                                if (fit < 1.0f) {
                                    primaryFs *= fit;
                                    secondaryFs *= fit;
                                    if (bodyTextAvailW > 8.0f) {
                                        const auto fittedPrimary = fitTextToWidth(f, primaryFs, primaryText, bodyTextAvailW, 6.0f);
                                        primaryFs = fittedPrimary.first;
                                        primarySz = fittedPrimary.second;

                                        const auto fittedSecondary = fitTextToWidth(fSecondary, secondaryFs, secondaryText, bodyTextAvailW, 6.0f);
                                        secondaryFs = fittedSecondary.first;
                                        secondarySz = fittedSecondary.second;
                                    } else {
                                        primarySz = f->CalcTextSizeA(primaryFs, FLT_MAX, 0.0f, primaryText.c_str());
                                        secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                                    }
                                    totalH = primarySz.y + lineGap + secondarySz.y;
                                }
                            }

                            float startY = snapPxText(bodyTextTopY + (bodyTextAvailH - totalH) * 0.5f);
                            if (startY < bodyTextTopY) startY = snapPxText(bodyTextTopY);

                            float x1 = centerTextXWithin(bodyTextMinX, textMaxX, primarySz.x);
                            dl->AddText(f, primaryFs, ImVec2(x1, startY), theme.text, primaryText.c_str());

                            float y2 = snapPxText(startY + primarySz.y + lineGap);
                            float x2 = centerTextXWithin(bodyTextMinX, textMaxX, secondarySz.x);

                            dl->AddText(fSecondary, secondaryFs, ImVec2(x2, y2), infoCol, secondaryText.c_str());
                        }

                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            openRebindContextFor(vk);
                        }
                    };

                    bool haveNumpadPlusAnchor = false;
                    ImVec2 numpadPlusAnchorMin = ImVec2(0, 0);
                    ImVec2 numpadPlusAnchorMax = ImVec2(0, 0);
                    bool haveNumpadEnterAnchor = false;
                    ImVec2 numpadEnterAnchorMin = ImVec2(0, 0);
                    ImVec2 numpadEnterAnchorMax = ImVec2(0, 0);

                    auto snapPx = [](float v) -> float { return (float)(int)(v + 0.5f); };

                    for (size_t rowIdx = 0; rowIdx < rows.size(); ++rowIdx) {
                        float xCursor = layoutStart.x;
                        float yCursor = layoutStart.y + (float)rowIdx * (keyH + gap);

                        for (size_t colIdx = 0; colIdx < rows[rowIdx].size(); ++colIdx) {
                            const KeyCell& kc = rows[rowIdx][colIdx];
                            const float keyW = kc.w * pitchX;

                            ImGui::SetCursorPos(ImVec2(snapPx(xCursor), snapPx(yCursor)));
                            if (kc.vk == 0) {
                                ImGui::Dummy(ImVec2(keyW, keyH));
                                xCursor += keyW;
                                xCursor = snapPx(xCursor);
                                continue;
                            }

                            if (kc.vk == VK_ADD && rowIdx == 2) {
                                const ImVec2 aMin = ImGui::GetCursorScreenPos();
                                ImGui::Dummy(ImVec2(keyW, keyH));
                                const ImVec2 aMax = ImVec2(aMin.x + keyW, aMin.y + keyH);
                                haveNumpadPlusAnchor = true;
                                numpadPlusAnchorMin = aMin;
                                numpadPlusAnchorMax = aMax;
                                xCursor += keyW;
                                xCursor = snapPx(xCursor);
                                continue;
                            }

                            if (kc.vk == VK_RETURN && rowIdx == 4 && kc.w < 1.6f) {
                                const ImVec2 aMin = ImGui::GetCursorScreenPos();
                                ImGui::Dummy(ImVec2(keyW, keyH));
                                const ImVec2 aMax = ImVec2(aMin.x + keyW, aMin.y + keyH);
                                haveNumpadEnterAnchor = true;
                                numpadEnterAnchorMin = aMin;
                                numpadEnterAnchorMax = aMax;
                                xCursor += keyW;
                                xCursor = snapPx(xCursor);
                                continue;
                            }

                            ImGui::PushID((int)(rowIdx * 1000 + colIdx));
                            ImGui::InvisibleButton("##key", ImVec2(keyW, keyH),
                                                  ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

                            const ImVec2 pMin = ImGui::GetItemRectMin();
                            const ImVec2 pMax = ImGui::GetItemRectMax();

                            const KeyRebind* rb = findRebindForKey(kc.vk);

                            std::string keyName = kc.labelOverride ? std::string(kc.labelOverride) : VkToString(kc.vk);
                            ImVec2 capMin = ImVec2(pMin.x + keyPadX, pMin.y + keyPadY);
                            ImVec2 capMax = ImVec2(pMax.x - keyPadX, pMax.y - keyPadY);
                            if (capMax.x <= capMin.x + 2.0f) { capMin.x = pMin.x; capMax.x = pMax.x; }
                            if (capMax.y <= capMin.y + 2.0f) { capMin.y = pMin.y; capMax.y = pMax.y; }
                            drawKeyCell(kc.vk, keyName.c_str(), capMin, capMax, rb);

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                if (rb && rb->fromKey != 0 && rb->toKey != 0) {
                                    DWORD triggerVkTip = resolveTriggerVkFor(rb, kc.vk);
                                    DWORD triggerScanTip = resolveTriggerScanFor(rb, kc.vk);
                                    const std::string typesTip = typesValueForDisplay(rb, kc.vk);
                                    const std::string triggersTip =
                                        normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                    ImGui::Text(tr("inputs.types_format", typesTip.c_str()).c_str());
                                    if (hasShiftLayerOverride(rb, kc.vk)) {
                                        const std::string typesShiftTip = typesShiftValueForDisplay(rb, kc.vk);
                                        ImGui::Text(tr("inputs.types_shift_format", typesShiftTip.c_str()).c_str());
                                    }
                                    ImGui::Text(tr("inputs.triggers_format", triggersTip.c_str()).c_str());
                                } else {
                                    ImGui::Text("%s (%u)", VkToString(kc.vk).c_str(), (unsigned)kc.vk);
                                    ImGui::TextUnformatted(trc("inputs.tooltip.right_click_to_configure"));
                                }
                                ImGui::EndTooltip();
                            }

                            ImGui::PopID();
                            xCursor += keyW;
                            xCursor = snapPx(xCursor);
                        }

                    }

                    auto drawTallKey = [&](DWORD vk, const char* label, const ImVec2& anchorMin, const ImVec2& anchorMax) {
                        const float w = anchorMax.x - anchorMin.x;
                        const float h = keyH * 2.0f + gap;

                        ImGui::PushID((int)vk);
                        ImGui::SetCursorScreenPos(anchorMin);
                        ImGui::InvisibleButton("##tall", ImVec2(w, h),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        const ImVec2 pMin = ImGui::GetItemRectMin();
                        const ImVec2 pMax = ImVec2(pMin.x + w, pMin.y + h);

                        const KeyRebind* rb = findRebindForKey(vk);
                        ImVec2 capMin = ImVec2(pMin.x + keyPadX, pMin.y + keyPadY);
                        ImVec2 capMax = ImVec2(pMax.x - keyPadX, pMax.y - keyPadY);
                        if (capMax.x <= capMin.x + 2.0f) { capMin.x = pMin.x; capMax.x = pMax.x; }
                        if (capMax.y <= capMin.y + 2.0f) { capMin.y = pMin.y; capMax.y = pMax.y; }
                        drawKeyCell(vk, label, capMin, capMax, rb);

                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            if (rb && rb->fromKey != 0 && rb->toKey != 0) {
                                DWORD triggerVkTip = resolveTriggerVkFor(rb, vk);
                                DWORD triggerScanTip = resolveTriggerScanFor(rb, vk);
                                const std::string typesTip = typesValueForDisplay(rb, vk);
                                const std::string triggersTip = normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                ImGui::Text(trc("inputs.types_format"), typesTip.c_str());
                                if (hasShiftLayerOverride(rb, vk)) {
                                    const std::string typesShiftTip = typesShiftValueForDisplay(rb, vk);
                                    ImGui::Text(trc("inputs.types_shift_format"), typesShiftTip.c_str());
                                }
                                ImGui::Text(trc("inputs.triggers_format"), triggersTip.c_str());
                            } else {
                                ImGui::Text("%s (%u)", VkToString(vk).c_str(), (unsigned)vk);
                                ImGui::TextUnformatted(trc("inputs.tooltip.right_click_to_configure"));
                            }
                            ImGui::EndTooltip();
                        }

                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            openRebindContextFor(vk);
                        }

                        ImGui::PopID();
                    };

                    if (haveNumpadPlusAnchor) {
                        drawTallKey(VK_ADD, "+", numpadPlusAnchorMin, numpadPlusAnchorMax);
                    }
                    if (haveNumpadEnterAnchor) {
                        drawTallKey(VK_RETURN, "ENTER", numpadEnterAnchorMin, numpadEnterAnchorMax);
                    }

                    const float mousePanelX = layoutStart.x + keyboardMaxRowW + unit * 0.9f;
                    const float mousePanelY = layoutStart.y;
                    ImGui::SetCursorPos(ImVec2(mousePanelX, mousePanelY));

                    const float mouseHeaderH = 0.0f;

                    float mouseDiagramTotalH = mouseHeaderH;

                    {
                        const float mouseW = unit * 3.6f;
                        const float mouseH = (keyboardTotalH - mouseHeaderH - gap);
                        mouseDiagramTotalH = mouseHeaderH + mouseH;
                        const float pad = 6.0f * keyboardScale;

                        ImGui::SetCursorPos(ImVec2(mousePanelX, mousePanelY + mouseHeaderH));
                        ImGui::Dummy(ImVec2(mouseW, mouseH));
                        const ImVec2 bodyMin = ImGui::GetItemRectMin();
                        const ImVec2 bodyMax = ImGui::GetItemRectMax();

                        const float bodyR = (mouseW < mouseH ? mouseW : mouseH) * 0.45f;
                        dl->AddRectFilled(bodyMin, bodyMax, IM_COL32(24, 26, 33, 255), bodyR);
                        dl->AddRect(bodyMin, bodyMax, IM_COL32(10, 10, 12, 255), bodyR, 0, 1.5f);

                        const ImVec2 innerMin = ImVec2(bodyMin.x + pad, bodyMin.y + pad);
                        const ImVec2 innerMax = ImVec2(bodyMax.x - pad, bodyMax.y - pad);
                        const float midX = (innerMin.x + innerMax.x) * 0.5f;
                        const float topH = (innerMax.y - innerMin.y) * 0.52f;
                        const float splitY = innerMin.y + topH;

                        auto drawMouseSegment = [&](DWORD vk, const char* label, const ImVec2& segMin, const ImVec2& segMax, float segR, ImDrawFlags segFlags) {
                            const bool hovered = ImGui::IsItemHovered();
                            const bool active = ImGui::IsItemActive();

                            struct KeyTheme {
                                ImU32 top;
                                ImU32 bottom;
                                ImU32 border;
                                ImU32 text;
                            };
                            auto clamp255 = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                            auto adjust = [&](ImU32 c, int add) -> ImU32 {
                                int r = (int)(c & 0xFF);
                                int g = (int)((c >> 8) & 0xFF);
                                int b = (int)((c >> 16) & 0xFF);
                                int a = (int)((c >> 24) & 0xFF);
                                r = clamp255(r + add);
                                g = clamp255(g + add);
                                b = clamp255(b + add);
                                return (ImU32)(r | (g << 8) | (b << 16) | (a << 24));
                            };
                            KeyTheme theme{ IM_COL32(88, 90, 108, 255), IM_COL32(44, 46, 60, 255), IM_COL32(18, 18, 20, 255),
                                            IM_COL32(245, 245, 245, 255) };
                            theme.bottom = adjust(theme.bottom, 10);
                            if (hovered) {
                                theme.top = adjust(theme.top, 12);
                                theme.bottom = adjust(theme.bottom, 10);
                            }
                            if (active) {
                                theme.top = adjust(theme.top, -8);
                                theme.bottom = adjust(theme.bottom, -16);
                            }

                            dl->AddRectFilled(segMin, segMax, theme.bottom, segR, segFlags);
                            const ImVec2 segMid = ImVec2(segMax.x, segMin.y + (segMax.y - segMin.y) * 0.72f);
                            ImDrawFlags topFlags = segFlags;
                            if (segFlags == ImDrawFlags_RoundCornersAll) {
                                topFlags = ImDrawFlags_RoundCornersTop;
                            } else {
                                topFlags &= (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight);
                            }
                            dl->AddRectFilled(segMin, segMid, theme.top, segR, topFlags);
                            dl->AddRect(segMin, segMax, theme.border, segR, segFlags, 1.0f);

                            const KeyRebind* rb = findRebindForKey(vk);
                            if (rb && rb->fromKey != 0 && rb->toKey != 0) {
                                const ImU32 outline = rb->enabled ? IM_COL32(0, 220, 110, 255) : IM_COL32(255, 170, 0, 255);
                                dl->AddRect(segMin, segMax, outline, segR, segFlags, 3.0f);
                            }

                            ImFont* f = ImGui::GetFont();
                            const float fs = ImGui::GetFontSize() * 0.85f;
                            const ImVec2 tsz = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
                            const ImVec2 tpos = ImVec2(segMin.x + (segMax.x - segMin.x - tsz.x) * 0.5f, segMin.y + (segMax.y - segMin.y - tsz.y) * 0.5f);
                            dl->AddText(f, fs, tpos, theme.text, label);

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                if (rb && rb->fromKey != 0 && rb->toKey != 0) {
                                    DWORD triggerVkTip = resolveTriggerVkFor(rb, vk);
                                    DWORD triggerScanTip = resolveTriggerScanFor(rb, vk);
                                    const std::string typesTip = typesValueForDisplay(rb, vk);
                                    const std::string triggersTip =
                                        normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                    ImGui::Text("Types: %s", typesTip.c_str());
                                    ImGui::Text("Triggers: %s", triggersTip.c_str());
                                } else {
                                    ImGui::Text("Input: %s (%u)", VkToString(vk).c_str(), (unsigned)vk);
                                    ImGui::TextUnformatted("Right-click to configure rebinds.");
                                }
                                ImGui::EndTooltip();
                            }
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                                openRebindContextFor(vk);
                            }
                        };

                        const float wheelW = (innerMax.x - innerMin.x) * 0.16f;
                        const float wheelH = topH * 0.55f;
                        const ImVec2 wheelMin = ImVec2(midX - wheelW * 0.5f, innerMin.y + topH * 0.18f);
                        const ImVec2 wheelMax = ImVec2(midX + wheelW * 0.5f, wheelMin.y + wheelH);
                        const float wheelSideGap = 2.0f * keyboardScale;

                        const ImVec2 leftMin = innerMin;
                        const ImVec2 leftMax = ImVec2(wheelMin.x - wheelSideGap, splitY);
                        const ImVec2 rightMin = ImVec2(wheelMax.x + wheelSideGap, innerMin.y);
                        const ImVec2 rightMax = ImVec2(innerMax.x, splitY);

                        const float sideW = (innerMax.x - innerMin.x) * 0.32f;
                        const float sideH = (innerMax.y - innerMin.y) * 0.12f;
                        const float sideX0 = innerMin.x + (innerMax.x - innerMin.x) * 0.07f;
                        const float sideY0 = innerMin.y + topH + (innerMax.y - innerMin.y - topH) * 0.26f;
                        const float sideGap = sideH * 0.35f;
                        const ImVec2 side1Min = ImVec2(sideX0, sideY0);
                        const ImVec2 side1Max = ImVec2(sideX0 + sideW, sideY0 + sideH);
                        const ImVec2 side2Min = ImVec2(sideX0, sideY0 + sideH + sideGap);
                        const ImVec2 side2Max = ImVec2(sideX0 + sideW, sideY0 + sideH + sideGap + sideH);

                        dl->AddLine(ImVec2(midX, innerMin.y + 2), ImVec2(midX, splitY - 2), IM_COL32(10, 10, 12, 255), 1.0f);
                        dl->AddLine(ImVec2(innerMin.x + 2, splitY), ImVec2(innerMax.x - 2, splitY), IM_COL32(10, 10, 12, 255), 1.0f);

                        ImGui::SetCursorScreenPos(leftMin);
                        ImGui::InvisibleButton("##mouse_left", ImVec2(leftMax.x - leftMin.x, leftMax.y - leftMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        drawMouseSegment(VK_LBUTTON, "MB1", leftMin, leftMax, bodyR * 0.75f, ImDrawFlags_RoundCornersTopLeft);

                        ImGui::SetCursorScreenPos(rightMin);
                        ImGui::InvisibleButton("##mouse_right", ImVec2(rightMax.x - rightMin.x, rightMax.y - rightMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        drawMouseSegment(VK_RBUTTON, "MB2", rightMin, rightMax, bodyR * 0.75f, ImDrawFlags_RoundCornersTopRight);

                        ImGui::SetCursorScreenPos(wheelMin);
                        ImGui::InvisibleButton("##mouse_mid", ImVec2(wheelMax.x - wheelMin.x, wheelMax.y - wheelMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        drawMouseSegment(VK_MBUTTON, "MB3", wheelMin, wheelMax, 6.0f * keyboardScale, ImDrawFlags_RoundCornersAll);

                        ImGui::SetCursorScreenPos(side1Min);
                        ImGui::InvisibleButton("##mouse_x1", ImVec2(side1Max.x - side1Min.x, side1Max.y - side1Min.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        drawMouseSegment(VK_XBUTTON2, "MB5", side1Min, side1Max, 6.0f * keyboardScale, ImDrawFlags_RoundCornersAll);

                        ImGui::SetCursorScreenPos(side2Min);
                        ImGui::InvisibleButton("##mouse_x2", ImVec2(side2Max.x - side2Min.x, side2Max.y - side2Min.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        drawMouseSegment(VK_XBUTTON1, "MB4", side2Min, side2Max, 6.0f * keyboardScale, ImDrawFlags_RoundCornersAll);
                    }

                    ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
                    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(640.0f, FLT_MAX));
                    if (ImGui::BeginPopup(trc("inputs.rebind_config"))) {
                        // Also block global ESC-to-close-GUI while editing inside this popup.
                        MarkRebindBindingActive();
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f, 6.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 3.0f));

                        const bool nestedLayoutPopupOpen = ImGui::IsPopupOpen(trc("inputs.triggers_custom")) || ImGui::IsPopupOpen(trc("inputs.custom_unicode"));
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !nestedLayoutPopupOpen) {
                            s_layoutEscapeRequiresRelease = true;
                            layoutEscapeConsumedThisFrame = true;
                            s_layoutBindTarget = LayoutBindTarget::None;
                            s_layoutBindIndex = -1;
                            s_layoutUnicodeEditIndex = -1;
                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                            s_layoutUnicodeEditText.clear();
                            ImGui::CloseCurrentPopup();
                        }

                        syncUnicodeEditBuffers();

                        auto isNoOpRebindForKey = [&](const KeyRebind& r, DWORD originalVk) -> bool {
                            if (r.fromKey != originalVk) return false;
                            if (r.toKey != originalVk) return false;
                            if (r.customOutputVK != 0 && r.customOutputVK != r.toKey) return false;
                            if (r.customOutputUnicode != 0) return false;
                            if (r.customOutputScanCode != 0) return false;
                            if (r.baseOutputShifted) return false;
                            if (hasShiftLayerOverride(&r, originalVk)) return false;
                            return true;
                        };

                        auto eraseRebindIndex = [&](int eraseIdx) {
                            if (eraseIdx < 0 || eraseIdx >= (int)g_config.keyRebinds.rebinds.size()) return;
                            g_config.keyRebinds.rebinds.erase(g_config.keyRebinds.rebinds.begin() + eraseIdx);
                            g_configIsDirty = true;
                            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                            RebuildHotkeyMainKeys_Internal();

                            auto decIfAfter = [&](int& v) {
                                if (v == -1) return;
                                if (v == eraseIdx) {
                                    v = -1;
                                } else if (v > eraseIdx) {
                                    v -= 1;
                                }
                            };
                            decIfAfter(s_rebindFromKeyToBind);
                            decIfAfter(s_rebindOutputVKToBind);
                            decIfAfter(s_rebindTextOverrideVKToBind);
                            decIfAfter(s_rebindOutputScanToBind);
                            decIfAfter(s_layoutBindIndex);
                            decIfAfter(s_layoutContextPreferredIndex);
                            decIfAfter(s_layoutUnicodeEditIndex);

                            syncUnicodeEditBuffers();
                        };

                        auto findBestRebindIndexForKey = [&](DWORD fromVk) -> int {
                            int first = -1;
                            int enabledAny = -1;
                            int enabledConfigured = -1;
                            int configuredAny = -1;

                            for (int ri = 0; ri < (int)g_config.keyRebinds.rebinds.size(); ++ri) {
                                const auto& r = g_config.keyRebinds.rebinds[ri];
                                if (r.fromKey != fromVk) continue;
                                if (first == -1) first = ri;

                                const bool configured = (r.fromKey != 0 && r.toKey != 0);
                                if (configured && configuredAny == -1) configuredAny = ri;
                                if (r.enabled && enabledAny == -1) enabledAny = ri;
                                if (r.enabled && configured) {
                                    enabledConfigured = ri;
                                    break;
                                }
                            }

                            if (enabledConfigured != -1) return enabledConfigured;
                            if (configuredAny != -1) return configuredAny;
                            if (enabledAny != -1) return enabledAny;
                            return first;
                        };

                        // Do not create a rebind on right-click.
                        int idx = s_layoutContextPreferredIndex;
                        if (idx < 0 || idx >= (int)g_config.keyRebinds.rebinds.size() || g_config.keyRebinds.rebinds[idx].fromKey != s_layoutContextVk) {
                            idx = findBestRebindIndexForKey(s_layoutContextVk);
                            s_layoutContextPreferredIndex = idx;
                        }

                        auto createRebindForKeyIfMissing = [&](DWORD fromVk) -> int {
                            int e = findBestRebindIndexForKey(fromVk);
                            if (e >= 0) return e;
                            KeyRebind r;
                            r.fromKey = fromVk;
                            r.toKey = fromVk;
                            r.enabled = true;
                            g_config.keyRebinds.rebinds.push_back(r);
                            g_configIsDirty = true;
                            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                            RebuildHotkeyMainKeys_Internal();
                            syncUnicodeEditBuffers();
                            return (int)g_config.keyRebinds.rebinds.size() - 1;
                        };

                        auto typesValueFor = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                            return typesValueForDisplay(rb, originalVk);
                        };

                        auto typesShiftValueFor = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                            return typesShiftValueForDisplay(rb, originalVk);
                        };

                        auto triggersValueFor = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                            DWORD triggerVk = resolveTriggerVkFor(rb, originalVk);
                            DWORD displayScan = resolveTriggerScanFor(rb, originalVk);
                            return scanCodeToDisplayName(displayScan, triggerVk);
                        };

                        auto syncCustomOutputState = [&](KeyRebind& r) {
                            r.useCustomOutput = (r.customOutputVK != 0) || (r.customOutputUnicode != 0) || (r.customOutputScanCode != 0);
                        };

                        auto clearBaseTypesOverrideIfDefault = [&](KeyRebind& r, DWORD originalVk) {
                            if (r.customOutputUnicode == 0 && r.customOutputVK == originalVk && !r.baseOutputShifted) {
                                r.customOutputVK = 0;
                            }
                            syncCustomOutputState(r);
                        };

                        auto clearShiftLayerOverrideIfDefault = [&](KeyRebind& r, DWORD originalVk) {
                            if (!r.shiftLayerEnabled) {
                                r.shiftLayerOutputVK = 0;
                                r.shiftLayerOutputUnicode = 0;
                                r.shiftLayerOutputShifted = false;
                                return;
                            }

                            if (r.shiftLayerOutputUnicode != 0) {
                                if (r.shiftLayerOutputVK == 0) {
                                    r.shiftLayerOutputVK = resolveTypesVkFor(&r, originalVk, false);
                                }
                                return;
                            }

                            const DWORD baseTypesVk = resolveTypesVkFor(&r, originalVk, false);
                            if (r.shiftLayerOutputVK == 0) {
                                r.shiftLayerOutputVK = baseTypesVk;
                            }
                        };

                        auto setFullRebindState = [&](KeyRebind& r, DWORD originalVk) {
                            DWORD unifiedVk = resolveTriggerVkFor(&r, originalVk);
                            if (unifiedVk == 0) unifiedVk = originalVk;

                            r.toKey = unifiedVk;
                            r.customOutputUnicode = 0;
                            r.baseOutputShifted = false;
                            r.shiftLayerEnabled = false;
                            r.shiftLayerOutputVK = 0;
                            r.shiftLayerOutputUnicode = 0;
                            r.shiftLayerOutputShifted = false;
                            r.customOutputVK = unifiedVk;
                            clearBaseTypesOverrideIfDefault(r, originalVk);
                        };

                        auto isSplitRebindUiMode = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                            if (!rb) return false;
                            if (rb->customOutputUnicode != 0) return true;
                            if (rb->shiftLayerOutputUnicode != 0) return true;
                            if (rb->baseOutputShifted) return true;
                            if (hasShiftLayerOverride(rb, originalVk)) return true;
                            return resolveTypesVkFor(rb, originalVk, false) != resolveTriggerVkFor(rb, originalVk);
                        };

                        static std::vector<std::pair<DWORD, std::string>> s_knownScanCodes;
                        static bool s_knownScanCodesBuilt = false;
                        if (!s_knownScanCodesBuilt) {
                            struct ScanMenuEntry {
                                DWORD scan = 0;
                                std::string name;
                                std::string sortName;
                                int group = 0;
                                int order = 0;
                            };

                            s_knownScanCodes.clear();
                            s_knownScanCodes.reserve(0x1FE);
                            std::vector<ScanMenuEntry> entries;
                            entries.reserve(0x1FE);

                            auto toUpperAscii = [](std::string s) -> std::string {
                                for (char& ch : s) {
                                    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                                }
                                return s;
                            };

                            auto navOrderForVk = [](DWORD vk) -> int {
                                switch (vk) {
                                case VK_INSERT: return 0;
                                case VK_HOME: return 1;
                                case VK_PRIOR: return 2;
                                case VK_DELETE: return 3;
                                case VK_END: return 4;
                                case VK_NEXT: return 5;
                                case VK_UP: return 6;
                                case VK_LEFT: return 7;
                                case VK_DOWN: return 8;
                                case VK_RIGHT: return 9;
                                case VK_SNAPSHOT: return 10;
                                case VK_SCROLL: return 11;
                                case VK_PAUSE: return 12;
                                default: return 1000 + (int)vk;
                                }
                            };

                            auto numpadOrderFor = [](DWORD vk, DWORD scan) -> int {
                                switch (vk) {
                                case VK_NUMLOCK: return 0;
                                case VK_DIVIDE: return 1;
                                case VK_MULTIPLY: return 2;
                                case VK_SUBTRACT: return 3;
                                case VK_NUMPAD7: return 4;
                                case VK_NUMPAD8: return 5;
                                case VK_NUMPAD9: return 6;
                                case VK_ADD: return 7;
                                case VK_NUMPAD4: return 8;
                                case VK_NUMPAD5: return 9;
                                case VK_NUMPAD6: return 10;
                                case VK_NUMPAD1: return 11;
                                case VK_NUMPAD2: return 12;
                                case VK_NUMPAD3: return 13;
                                case VK_RETURN: return ((scan & 0xFF00) != 0) ? 14 : 1000;
                                case VK_NUMPAD0: return 15;
                                case VK_DECIMAL: return 16;
                                default: return 1000 + (int)vk;
                                }
                            };

                            auto modifierOrderForVk = [](DWORD vk) -> int {
                                switch (vk) {
                                case VK_ESCAPE: return 0;
                                case VK_TAB: return 1;
                                case VK_CAPITAL: return 2;
                                case VK_SHIFT:
                                case VK_LSHIFT:
                                case VK_RSHIFT:
                                    return 3;
                                case VK_CONTROL:
                                case VK_LCONTROL:
                                case VK_RCONTROL:
                                    return 4;
                                case VK_MENU:
                                case VK_LMENU:
                                case VK_RMENU:
                                    return 5;
                                case VK_LWIN:
                                case VK_RWIN:
                                    return 6;
                                case VK_SPACE: return 7;
                                case VK_RETURN: return 8;
                                case VK_BACK: return 9;
                                case VK_APPS: return 10;
                                default: return 1000 + (int)vk;
                                }
                            };

                            auto classifyGroupOrder = [&](DWORD scan, DWORD vk) -> std::pair<int, int> {
                                if (vk >= 'A' && vk <= 'Z') {
                                    return { 0, (int)(vk - 'A') };
                                }
                                if (vk >= '0' && vk <= '9') {
                                    return { 1, (int)(vk - '0') };
                                }
                                if (vk >= VK_F1 && vk <= VK_F24) {
                                    return { 2, (int)(vk - VK_F1) };
                                }

                                switch (vk) {
                                case VK_INSERT:
                                case VK_HOME:
                                case VK_PRIOR:
                                case VK_DELETE:
                                case VK_END:
                                case VK_NEXT:
                                case VK_UP:
                                case VK_LEFT:
                                case VK_DOWN:
                                case VK_RIGHT:
                                case VK_SNAPSHOT:
                                case VK_SCROLL:
                                case VK_PAUSE:
                                    return { 3, navOrderForVk(vk) };
                                default:
                                    break;
                                }

                                const bool isNumpad =
                                    (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_NUMLOCK || vk == VK_DIVIDE ||
                                    vk == VK_MULTIPLY || vk == VK_SUBTRACT || vk == VK_ADD || vk == VK_DECIMAL ||
                                    (vk == VK_RETURN && ((scan & 0xFF00) != 0));
                                if (isNumpad) {
                                    return { 4, numpadOrderFor(vk, scan) };
                                }

                                switch (vk) {
                                case VK_ESCAPE:
                                case VK_TAB:
                                case VK_CAPITAL:
                                case VK_SHIFT:
                                case VK_LSHIFT:
                                case VK_RSHIFT:
                                case VK_CONTROL:
                                case VK_LCONTROL:
                                case VK_RCONTROL:
                                case VK_MENU:
                                case VK_LMENU:
                                case VK_RMENU:
                                case VK_LWIN:
                                case VK_RWIN:
                                case VK_SPACE:
                                case VK_RETURN:
                                case VK_BACK:
                                case VK_APPS:
                                    return { 5, modifierOrderForVk(vk) };
                                default:
                                    break;
                                }

                                if (vk == 0) {
                                    return { 7, (int)scan };
                                }

                                return { 6, (int)vk };
                            };

                            auto appendScan = [&](DWORD scan) {
                                DWORD vkFromScan = MapVirtualKey(scan, MAPVK_VSC_TO_VK_EX);
                                std::string name = scanCodeToDisplayName(scan, vkFromScan);
                                const auto [group, order] = classifyGroupOrder(scan, vkFromScan);
                                ScanMenuEntry e;
                                e.scan = scan;
                                e.name = name;
                                e.sortName = toUpperAscii(name);
                                e.group = group;
                                e.order = order;
                                entries.push_back(std::move(e));
                            };

                            for (DWORD low = 1; low <= 0xFF; ++low) {
                                appendScan(low);
                            }

                            for (DWORD low = 1; low <= 0xFF; ++low) {
                                appendScan(0xE000 | low);
                            }

                            std::sort(entries.begin(), entries.end(), [](const ScanMenuEntry& a, const ScanMenuEntry& b) {
                                if (a.group != b.group) return a.group < b.group;
                                if (a.order != b.order) return a.order < b.order;
                                if (a.sortName != b.sortName) return a.sortName < b.sortName;
                                return a.scan < b.scan;
                            });

                            for (const auto& e : entries) {
                                s_knownScanCodes.emplace_back(e.scan, e.name);
                            }

                            s_knownScanCodesBuilt = true;
                        }

                        auto formatScanHex = [](DWORD scan) -> std::string {
                            auto hex2 = [](unsigned v) -> std::string {
                                static const char* kHex = "0123456789ABCDEF";
                                std::string s;
                                s.push_back(kHex[(v >> 4) & 0xF]);
                                s.push_back(kHex[v & 0xF]);
                                return s;
                            };

                            const unsigned low = (unsigned)(scan & 0xFF);
                            if ((scan & 0xFF00) != 0) {
                                return std::string("E0 ") + hex2(low);
                            }
                            return hex2(low);
                        };

                        if (s_layoutBindTarget != LayoutBindTarget::None && s_layoutBindIndex >= 0 &&
                            s_layoutBindIndex < (int)g_config.keyRebinds.rebinds.size()) {
                            MarkRebindBindingActive();

                            DWORD capturedVk = 0;
                            LPARAM capturedLParam = 0;
                            bool capturedIsMouse = false;
                            if (ConsumeBindingInputEventSince(s_layoutBindLastSeq, capturedVk, capturedLParam, capturedIsMouse)) {
                                if (capturedVk == VK_ESCAPE) {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutBindIndex];
                                    int maybeErase = isNoOpRebindForKey(r, r.fromKey) ? s_layoutBindIndex : -1;
                                    s_layoutBindTarget = LayoutBindTarget::None;
                                    s_layoutBindIndex = -1;
                                    if (maybeErase != -1) {
                                        eraseRebindIndex(maybeErase);
                                        if (s_layoutContextPreferredIndex == maybeErase) s_layoutContextPreferredIndex = -1;
                                    }
                                } else {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutBindIndex];
                                    if (s_layoutBindTarget == LayoutBindTarget::FullOutputVk) {
                                        r.toKey = capturedVk;
                                        r.customOutputUnicode = 0;
                                        r.customOutputScanCode = 0;
                                        r.baseOutputShifted = false;
                                        r.shiftLayerEnabled = false;
                                        r.shiftLayerOutputVK = 0;
                                        r.shiftLayerOutputUnicode = 0;
                                        r.shiftLayerOutputShifted = false;
                                        r.customOutputVK = capturedVk;
                                        if (s_layoutBindIndex >= 0 && s_layoutBindIndex < (int)s_rebindUnicodeTextEdit.size()) {
                                            s_rebindUnicodeTextEdit[s_layoutBindIndex].clear();
                                        }

                                        clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                        g_configIsDirty = true;
                                    } else if (s_layoutBindTarget == LayoutBindTarget::TypesVk) {
                                        r.useCustomOutput = true;
                                        r.customOutputVK = capturedVk;
                                        r.customOutputUnicode = 0;
                                        if (s_layoutBindIndex >= 0 && s_layoutBindIndex < (int)s_rebindUnicodeTextEdit.size()) {
                                            s_rebindUnicodeTextEdit[s_layoutBindIndex].clear();
                                        }

                                        clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                        g_configIsDirty = true;
                                    } else if (s_layoutBindTarget == LayoutBindTarget::TypesVkShift) {
                                        r.shiftLayerEnabled = true;
                                        if (r.shiftLayerOutputVK == 0 && r.shiftLayerOutputUnicode == 0) {
                                            r.shiftLayerOutputShifted = s_layoutShiftUppercaseDefault;
                                        }
                                        r.shiftLayerOutputVK = capturedVk;
                                        r.shiftLayerOutputUnicode = 0;

                                        clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                        g_configIsDirty = true;
                                    } else if (s_layoutBindTarget == LayoutBindTarget::TriggersVk) {
                                        r.toKey = capturedVk;

                                        if (!s_layoutContextSplitMode) {
                                            r.customOutputUnicode = 0;
                                            r.customOutputScanCode = 0;
                                            r.baseOutputShifted = false;
                                            r.shiftLayerEnabled = false;
                                            r.shiftLayerOutputVK = 0;
                                            r.shiftLayerOutputUnicode = 0;
                                            r.shiftLayerOutputShifted = false;
                                            r.customOutputVK = capturedVk;
                                            clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                        }
                                        g_configIsDirty = true;
                                    }

                                    if (isNoOpRebindForKey(r, r.fromKey)) {
                                        int eraseIdx = s_layoutBindIndex;
                                        s_layoutBindTarget = LayoutBindTarget::None;
                                        s_layoutBindIndex = -1;
                                        eraseRebindIndex(eraseIdx);
                                    } else {
                                        s_layoutBindTarget = LayoutBindTarget::None;
                                        s_layoutBindIndex = -1;
                                        (void)capturedLParam;
                                        (void)capturedIsMouse;
                                    }
                                }
                            }
                        }

                        if (s_layoutUnicodeEditIndex != -1) {
                            MarkRebindBindingActive();
                            ImGui::OpenPopup(trc("inputs.custom_unicode"));
                        }

                        if (ImGui::BeginPopupModal(trc("inputs.custom_unicode"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                            MarkRebindBindingActive();
                            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                s_layoutEscapeRequiresRelease = true;
                                layoutEscapeConsumedThisFrame = true;
                                s_layoutUnicodeEditIndex = -1;
                                s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                s_layoutUnicodeEditText.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::TextUnformatted(trc("inputs.tooltip.enter_unicode_or_codepoint"));
                            ImGui::TextDisabled(trc("inputs.tooltip.unicode_examples"));
                            ImGui::Separator();
                            ImGui::SetNextItemWidth(260.0f);
                            ImGui::InputTextWithHint("##unicode", trc("inputs.tooltip.unicode_hint"), &s_layoutUnicodeEditText);
                            ImGui::Spacing();

                            const bool canApply = true;
                            if (ImGui::Button(trc("button.apply"), ImVec2(120, 0)) && canApply) {
                                if (s_layoutUnicodeEditIndex >= 0 && s_layoutUnicodeEditIndex < (int)g_config.keyRebinds.rebinds.size()) {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutUnicodeEditIndex];
                                    if (s_layoutUnicodeEditTarget == LayoutUnicodeEditTarget::TypesShift) {
                                        const bool hadShiftOutput = (r.shiftLayerOutputVK != 0) || (r.shiftLayerOutputUnicode != 0);
                                        r.shiftLayerEnabled = true;
                                        if (r.shiftLayerOutputVK == 0) {
                                            r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                        }
                                        if (!hadShiftOutput) {
                                            r.shiftLayerOutputShifted = s_layoutShiftUppercaseDefault;
                                        }
                                        if (s_layoutUnicodeEditText.empty()) {
                                            r.shiftLayerOutputUnicode = 0;
                                            clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;
                                        } else {
                                            uint32_t cp = 0;
                                            if (tryParseUnicodeInput(s_layoutUnicodeEditText, cp)) {
                                                r.shiftLayerOutputUnicode = (DWORD)cp;
                                                g_configIsDirty = true;
                                            }
                                        }
                                    } else {
                                        if (s_layoutUnicodeEditText.empty()) {
                                            r.customOutputUnicode = 0;
                                            if (r.customOutputVK == 0 && r.customOutputScanCode == 0) r.useCustomOutput = false;
                                            g_configIsDirty = true;
                                        } else {
                                            uint32_t cp = 0;
                                            if (tryParseUnicodeInput(s_layoutUnicodeEditText, cp)) {
                                                r.useCustomOutput = true;
                                                r.customOutputUnicode = (DWORD)cp;
                                                g_configIsDirty = true;
                                            }
                                        }
                                    }

                                    if (isNoOpRebindForKey(r, r.fromKey)) {
                                        int eraseIdx = s_layoutUnicodeEditIndex;
                                        s_layoutUnicodeEditIndex = -1;
                                        s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                        s_layoutUnicodeEditText.clear();
                                        ImGui::CloseCurrentPopup();
                                        eraseRebindIndex(eraseIdx);
                                    }
                                }

                                s_layoutUnicodeEditIndex = -1;
                                s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                s_layoutUnicodeEditText.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(trc("label.cancel"), ImVec2(120, 0))) {
                                if (s_layoutUnicodeEditIndex >= 0 && s_layoutUnicodeEditIndex < (int)g_config.keyRebinds.rebinds.size()) {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutUnicodeEditIndex];
                                    int maybeErase = isNoOpRebindForKey(r, r.fromKey) ? s_layoutUnicodeEditIndex : -1;
                                    if (maybeErase != -1) {
                                        eraseRebindIndex(maybeErase);
                                        if (s_layoutContextPreferredIndex == maybeErase) s_layoutContextPreferredIndex = -1;
                                    }
                                }
                                s_layoutUnicodeEditIndex = -1;
                                s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                s_layoutUnicodeEditText.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                        KeyRebind* rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                        if (ImGui::IsWindowAppearing()) {
                            s_layoutContextSplitMode = isSplitRebindUiMode(rbPtr, s_layoutContextVk);
                        }

                        const float bindButtonW = 132.0f;
                        const float auxButtonW = 82.0f;
                        const float labelColumnW = 142.0f;
                        const float popupInlineGap = 6.0f;
                        bool openTriggersCustomPopup = false;
                        ImVec2 triggersCustomPopupAnchor = ImVec2(0.0f, 0.0f);
                        const std::string typesValue = typesValueFor(rbPtr, s_layoutContextVk);
                        const std::string typesShiftValue = typesShiftValueFor(rbPtr, s_layoutContextVk);
                        const std::string triggersValue = triggersValueFor(rbPtr, s_layoutContextVk);

                        if (ImGui::RadioButton(trc("inputs.full_rebind_label"), !s_layoutContextSplitMode)) {
                            s_layoutContextSplitMode = false;
                            if (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                setFullRebindState(r, r.fromKey);
                                g_configIsDirty = true;

                                if (isNoOpRebindForKey(r, r.fromKey)) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                    idx = -1;
                                }
                            }
                        }
                        if (ImGui::RadioButton(trc("inputs.split_rebind_label"), s_layoutContextSplitMode)) {
                            s_layoutContextSplitMode = true;
                        }

                        rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                        ImGui::Dummy(ImVec2(0.0f, 4.0f));
                        ImGui::Separator();

                        auto drawPopupRowLabel = [&](const char* tooltip, const char* label) {
                            HelpMarker(tooltip);
                            ImGui::SameLine(0.0f, popupInlineGap);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(label);
                        };

                        if (ImGui::BeginTable("##rebind_config_rows", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
                            ImGui::TableSetupColumn("##rebind_label", ImGuiTableColumnFlags_WidthFixed, labelColumnW);
                            ImGui::TableSetupColumn("##rebind_value", ImGuiTableColumnFlags_WidthStretch);

                            if (!s_layoutContextSplitMode) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_output"), trc("inputs.output_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::FullOutputVk)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : triggersValue;
                                    if (ImGui::Button((label + "##output_full").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            s_layoutBindTarget = LayoutBindTarget::FullOutputVk;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.custom") + "##output_scan_custom").c_str(), ImVec2(auxButtonW, 0))) {
                                        openTriggersCustomPopup = true;
                                        triggersCustomPopupAnchor =
                                            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0f);
                                    }
                                }
                            } else {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_types"), trc("inputs.types_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::TypesVk)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : typesValue;
                                    if (ImGui::Button((label + "##types").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            s_layoutBindTarget = LayoutBindTarget::TypesVk;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.custom") + "##types_custom").c_str(), ImVec2(auxButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            s_layoutUnicodeEditIndex = idx;
                                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::TypesBase;
                                            const auto& r = g_config.keyRebinds.rebinds[idx];
                                            s_layoutUnicodeEditText =
                                                (r.customOutputUnicode != 0) ? formatCodepointUPlus((uint32_t)r.customOutputUnicode) : std::string();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::AlignTextToFramePadding();
                                    bool baseOutputShifted = rbPtr ? rbPtr->baseOutputShifted : false;
                                    if (ImGui::Checkbox((std::string(trc("inputs.uppercase_label")) + "##types_uppercase").c_str(), &baseOutputShifted)) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.baseOutputShifted = baseOutputShifted;
                                            clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;

                                            if (isNoOpRebindForKey(r, r.fromKey)) {
                                                eraseRebindIndex(idx);
                                                s_layoutContextPreferredIndex = -1;
                                                idx = -1;
                                            }
                                        }
                                    }
                                }

                                rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_types_shift"), trc("inputs.types_shift_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::TypesVkShift)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : typesShiftValue;
                                    if (ImGui::Button((label + "##types_shift").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.shiftLayerEnabled = true;
                                            if (r.shiftLayerOutputVK == 0 && r.shiftLayerOutputUnicode == 0) {
                                                r.shiftLayerOutputShifted = s_layoutShiftUppercaseDefault;
                                            }
                                            s_layoutBindTarget = LayoutBindTarget::TypesVkShift;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.custom") + "##types_shift_custom").c_str(), ImVec2(auxButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            const auto& r = g_config.keyRebinds.rebinds[idx];
                                            s_layoutUnicodeEditIndex = idx;
                                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::TypesShift;
                                            s_layoutUnicodeEditText =
                                                (r.shiftLayerOutputUnicode != 0)
                                                    ? formatCodepointUPlus((uint32_t)r.shiftLayerOutputUnicode)
                                                    : std::string();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::AlignTextToFramePadding();
                                    bool shiftLayerOutputShifted = rbPtr ? rbPtr->shiftLayerOutputShifted : false;
                                    if (ImGui::Checkbox((std::string(trc("inputs.uppercase_label")) + "##types_shift_uppercase").c_str(),
                                                        &shiftLayerOutputShifted)) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.shiftLayerEnabled = true;
                                            if (r.shiftLayerOutputVK == 0) {
                                                r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                            }
                                            r.shiftLayerOutputShifted = shiftLayerOutputShifted;
                                            s_layoutShiftUppercaseDefault = shiftLayerOutputShifted;
                                            clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;

                                            if (isNoOpRebindForKey(r, r.fromKey)) {
                                                eraseRebindIndex(idx);
                                                s_layoutContextPreferredIndex = -1;
                                                idx = -1;
                                            }
                                        }
                                    }
                                }

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_triggers"), trc("inputs.triggers_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::TriggersVk)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : triggersValue;
                                    if (ImGui::Button((label + "##triggers").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            s_layoutBindTarget = LayoutBindTarget::TriggersVk;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.custom") + "##triggers_scan_custom").c_str(), ImVec2(auxButtonW, 0))) {
                                        openTriggersCustomPopup = true;
                                        triggersCustomPopupAnchor =
                                            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0f);
                                    }
                                }
                            }

                            ImGui::EndTable();
                        }

                        idx = (idx >= 0) ? idx : findBestRebindIndexForKey(s_layoutContextVk);
                        KeyRebind* r = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;

                        if (openTriggersCustomPopup) {
                            ImGui::SetNextWindowPos(triggersCustomPopupAnchor, ImGuiCond_Appearing);
                            ImGui::OpenPopup(trc("inputs.triggers_custom"));
                        }
                        ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 220.0f), ImVec2(520.0f, 460.0f));

                        if (ImGui::BeginPopup(trc("inputs.triggers_custom"))) {
                                MarkRebindBindingActive();
                                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                    s_layoutEscapeRequiresRelease = true;
                                    layoutEscapeConsumedThisFrame = true;
                                    ImGui::CloseCurrentPopup();
                                }

                                DWORD curTriggerVk = r ? r->toKey : s_layoutContextVk;
                                if (curTriggerVk == 0) curTriggerVk = s_layoutContextVk;
                                DWORD curScan = (r && r->useCustomOutput && r->customOutputScanCode != 0) ? r->customOutputScanCode
                                                                                                          : getScanCodeWithExtendedFlag(curTriggerVk);
                                std::string preview = scanCodeToDisplayName(curScan, curTriggerVk);

                                ImGui::Text(tr("inputs.current_format", preview.c_str()).c_str());
                                ImGui::Separator();

                                bool isDefault = !(r && r->useCustomOutput && r->customOutputScanCode != 0);
                                if (ImGui::Selectable(trc("label.default"), isDefault)) {
                                    idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                    s_layoutContextPreferredIndex = idx;
                                    if (idx >= 0) {
                                        auto& rr = g_config.keyRebinds.rebinds[idx];
                                        rr.customOutputScanCode = 0;
                                        if (rr.customOutputVK == 0 && rr.customOutputUnicode == 0) rr.useCustomOutput = false;
                                        g_configIsDirty = true;
                                    }
                                }
                                ImGui::Separator();

                                ImGui::BeginChild("##triggers_custom_list", ImVec2(0.0f, 260.0f), true);
                                for (const auto& it : s_knownScanCodes) {
                                    const DWORD scan = it.first;
                                    const std::string& name = it.second;
                                    const std::string itemLabel = name + "  (" + formatScanHex(scan) + ")##scan_" + std::to_string((unsigned)scan);

                                    const bool selected = (r && r->useCustomOutput && r->customOutputScanCode == scan);
                                    if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& rr = g_config.keyRebinds.rebinds[idx];
                                            rr.useCustomOutput = true;
                                            rr.customOutputScanCode = scan;
                                            g_configIsDirty = true;
                                        }
                                    }
                                }
                                ImGui::EndChild();

                                ImGui::EndPopup();
                        }

                        if (cannotTypeFor(rbPtr, s_layoutContextVk)) {
                            ImGui::TextDisabled(trc("inputs.cannot_type"));
                        }

                        ImGui::Spacing();

                        if (ImGui::Button((tr("label.reset") + "##layout_reset").c_str(), ImVec2(102, 0))) {
                            if (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                r.toKey = r.fromKey;
                                r.customOutputVK = 0;
                                r.customOutputUnicode = 0;
                                r.customOutputScanCode = 0;
                                r.baseOutputShifted = false;
                                r.shiftLayerEnabled = false;
                                r.shiftLayerOutputVK = 0;
                                r.shiftLayerOutputUnicode = 0;
                                r.shiftLayerOutputShifted = false;
                                r.useCustomOutput = false;
                                r.enabled = true;
                                g_configIsDirty = true;

                                if (idx >= 0 && idx < (int)s_rebindUnicodeTextEdit.size()) s_rebindUnicodeTextEdit[idx].clear();

                                if (isNoOpRebindForKey(r, r.fromKey)) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                } else {
                                    std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                                    RebuildHotkeyMainKeys_Internal();
                                }
                            }
                        }

                        ImGui::PopStyleVar(3);
                        ImGui::EndPopup();
                    }

                    const float totalH = (keyboardTotalH > mouseDiagramTotalH) ? keyboardTotalH : mouseDiagramTotalH;
                    ImGui::SetCursorPos(ImVec2(layoutStart.x, layoutStart.y + totalH + gap));

                    {
                        ImGui::Spacing();
                        ImGui::SeparatorText(trc("inputs.rebinds"));
                        bool anyShown = false;
                        auto isNoOp = [&](const KeyRebind& r) -> bool {
                            if (r.fromKey == 0 || r.toKey == 0) return true;
                            if (r.toKey != r.fromKey) return false;
                            if (r.customOutputVK != 0 && r.customOutputVK != r.toKey) return false;
                            if (r.customOutputUnicode != 0) return false;
                            if (r.customOutputScanCode != 0) return false;
                            if (r.baseOutputShifted) return false;
                            if (hasShiftLayerOverride(&r, r.fromKey)) return false;
                            return true;
                        };
                        for (const auto& r : g_config.keyRebinds.rebinds) {
                            if (r.fromKey == 0 || r.toKey == 0) continue;
                            if (isNoOp(r)) continue;

                            std::string fromStr = VkToString(r.fromKey);
                            std::string typesStr = typesValueForDisplay(&r, r.fromKey);
                            if (hasShiftLayerOverride(&r, r.fromKey)) {
                                const std::string shiftTypesStr = typesShiftValueForDisplay(&r, r.fromKey);
                                typesStr += "/" + shiftTypesStr;
                            }

                            DWORD triggerVk = resolveTriggerVkFor(&r, r.fromKey);
                            DWORD displayScan = resolveTriggerScanFor(&r, r.fromKey);
                            std::string triggersStr = scanCodeToDisplayName(displayScan, triggerVk);
                            ImGui::Text("%s -> %s & %s", fromStr.c_str(), typesStr.c_str(), triggersStr.c_str());
                            anyShown = true;
                        }
                        if (!anyShown) {
                            ImGui::TextDisabled(trc("inputs.no_active_rebinds"));
                        }
                    }

                    ImGui::EndChild();
                    ImGui::PopStyleColor();

                    const bool anyRebindBindUiActiveAfter = (s_rebindFromKeyToBind != -1) || (s_rebindOutputVKToBind != -1) ||
                                                            (s_rebindTextOverrideVKToBind != -1) || (s_rebindOutputScanToBind != -1) ||
                                                            (s_layoutBindTarget != LayoutBindTarget::None) || (s_layoutUnicodeEditIndex != -1) ||
                                                            ImGui::IsPopupOpen(trc("inputs.rebind_config")) || ImGui::IsPopupOpen(trc("inputs.triggers_custom")) ||
                                                            ImGui::IsPopupOpen(trc("inputs.custom_unicode"));
                    if (!blockLayoutEscapeThisFrame && !layoutEscapeConsumedThisFrame && escapePressedThisFrame && !anyRebindBindUiActiveAfter) {
                        s_keyboardLayoutOpen = false;
                    }

                    s_layoutContextPopupWasOpenLastFrame = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
                    s_keyboardLayoutWasOpenLastFrame = s_keyboardLayoutOpen;
                    } else {
                        s_layoutContextPopupWasOpenLastFrame = false;
                        s_keyboardLayoutWasOpenLastFrame = false;
                        s_lastKeyboardLayoutWindowSize = ImVec2(-1.0f, -1.0f);
                        s_lastKeyboardLayoutGuiScaleFactor = -1.0f;
                    }
                    ImGui::End();
                    ImGui::PopStyleColor();
                } else {
                    s_layoutContextPopupWasOpenLastFrame = false;
                    s_keyboardLayoutWasOpenLastFrame = false;
                    s_lastKeyboardLayoutWindowSize = ImVec2(-1.0f, -1.0f);
                    s_lastKeyboardLayoutGuiScaleFactor = -1.0f;
                }

                bool is_rebind_from_binding = (s_rebindFromKeyToBind != -1);
                if (is_rebind_from_binding) { MarkRebindBindingActive(); }
                if (is_rebind_from_binding) { ImGui::OpenPopup(trc("inputs.bind_from_key")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_from_key"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_from_key.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_from_key.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs1 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs1 = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs1, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindFromKeyToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindFromKeyToBind != -1 && s_rebindFromKeyToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                g_config.keyRebinds.rebinds[s_rebindFromKeyToBind].fromKey = capturedVk;
                                g_configIsDirty = true;
                                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                                RebuildHotkeyMainKeys_Internal();
                                (void)capturedLParam;
                                (void)capturedIsMouse;
                            }
                            s_rebindFromKeyToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

                bool is_vk_binding = (s_rebindOutputVKToBind != -1);
                if (is_vk_binding) { MarkRebindBindingActive(); }
                if (is_vk_binding) { ImGui::OpenPopup(trc("inputs.bind_output_vk")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_output_vk"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_output_vk.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_output_vk.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs2 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs2 = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs2, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindOutputVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindOutputVKToBind >= 0 && s_rebindOutputVKToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindOutputVKToBind];
                                rebind.toKey = capturedVk;
                                // Do not touch custom text override here.
                                g_configIsDirty = true;
                                (void)capturedLParam;
                                (void)capturedIsMouse;
                            }
                            s_rebindOutputVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

                bool is_text_vk_binding = (s_rebindTextOverrideVKToBind != -1);
                if (is_text_vk_binding) { MarkRebindBindingActive(); }
                if (is_text_vk_binding) { ImGui::OpenPopup(trc("inputs.bind_text_override_vk")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_text_override_vk"), NULL,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_text_override_vk.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_text_override_vk.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs2b = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs2b = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs2b, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindTextOverrideVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindTextOverrideVKToBind >= 0 &&
                                s_rebindTextOverrideVKToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindTextOverrideVKToBind];
                                rebind.useCustomOutput = true;
                                rebind.customOutputVK = capturedVk;
                                g_configIsDirty = true;
                                (void)capturedLParam;
                                (void)capturedIsMouse;
                            }
                            s_rebindTextOverrideVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

                bool is_scan_binding = (s_rebindOutputScanToBind != -1);
                if (is_scan_binding) { MarkRebindBindingActive(); }
                if (is_scan_binding) { ImGui::OpenPopup(trc("inputs.bind_output_scan")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_output_scan"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_output_scan.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_output_scan.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs3 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs3 = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs3, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindOutputScanToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindOutputScanToBind >= 0 && s_rebindOutputScanToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindOutputScanToBind];

                                if (capturedVk == VK_LBUTTON || capturedVk == VK_RBUTTON || capturedVk == VK_MBUTTON ||
                                    capturedVk == VK_XBUTTON1 || capturedVk == VK_XBUTTON2) {
                                    rebind.toKey = capturedVk;
                                    rebind.customOutputScanCode = 0;
                                } else {
                                    UINT scanCode = static_cast<UINT>((capturedLParam >> 16) & 0xFF);
                                    if ((capturedLParam & (1LL << 24)) != 0) { scanCode |= 0xE000; }
                                    if ((capturedLParam & (1LL << 24)) == 0 && scanCode == 0) {
                                        scanCode = getScanCodeWithExtendedFlag(capturedVk);
                                    }

                                    if ((scanCode & 0xFF00) == 0) { scanCode = getScanCodeWithExtendedFlag(capturedVk); }

                                    rebind.customOutputScanCode = scanCode;
                                    rebind.useCustomOutput = true;

                                    Log("[Rebind][GameKeybind] capturedVk=" + std::to_string(capturedVk) +
                                        " capturedLParam=" + std::to_string(static_cast<long long>(capturedLParam)) +
                                        " storedScan=" + std::to_string(scanCode) + " ext=" + std::string((scanCode & 0xFF00) ? "1" : "0"));
                                }

                                g_configIsDirty = true;
                                (void)capturedIsMouse;
                            }
                            s_rebindOutputScanToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

                ImGui::Spacing();
                ImGui::TextDisabled(trc("inputs.tooltip.configure_key_rebinds"));
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndTabItem();
}


