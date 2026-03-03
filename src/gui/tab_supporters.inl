if (ImGui::BeginTabItem("Supporters")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    if (g_supporterTierTexturesDirty.exchange(false, std::memory_order_acq_rel)) { ClearSupporterTierTextureCache(); }

    ImGui::TextWrapped("Thanks to these people for supporting the development of Toolscreen!");
    ImGui::TextWrapped("If you'd like to support, please consider donating at:");
    ImGui::TextLinkOpenURL("https://patreon.com/jojoe77777");
    ImGui::Spacing();

    const bool loaded = g_supportersLoaded.load(std::memory_order_acquire);
    const bool failedBefore = g_supportersFetchEverFailed.load(std::memory_order_acquire);

    if (!loaded) {
        if (failedBefore) {
            ImGui::TextWrapped("Unable to load supporters.");
        } else {
            ImGui::TextWrapped("Loading supporters...");
        }
    } else {
        std::shared_lock<std::shared_mutex> readLock(g_supportersMutex);
        if (g_supporterRoles.empty()) {
            ImGui::TextDisabled("No supporters listed.");
        }

        for (const auto& role : g_supporterRoles) {
            GLuint tierTexture = 0;
            int tierTextureWidth = 0;
            int tierTextureHeight = 0;
            const bool hasTierTexture = EnsureSupporterTierTexture(role, tierTexture, tierTextureWidth, tierTextureHeight);

            const ImVec4 roleColor(role.color.r, role.color.g, role.color.b, role.color.a);
            if (hasTierTexture) {
                constexpr float kMaxIconSize = 22.0f;
                float iconScale = 1.0f;
                const int maxSide = (std::max)(tierTextureWidth, tierTextureHeight);
                if (maxSide > 0) { iconScale = kMaxIconSize / static_cast<float>(maxSide); }

                const ImVec2 iconSize((std::max)(1.0f, static_cast<float>(tierTextureWidth) * iconScale),
                                      (std::max)(1.0f, static_cast<float>(tierTextureHeight) * iconScale));
                ImGui::Image((ImTextureID)(intptr_t)tierTexture, iconSize);
                ImGui::SameLine(0.0f, 8.0f);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, roleColor);
            ImGui::TextUnformatted(role.name.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();

            if (role.members.empty()) {
                ImGui::TextDisabled("No members listed.");
            } else {
                for (const auto& member : role.members) {
                    ImGui::BulletText("%s", member.c_str());
                }
            }

            ImGui::Spacing();
        }
    }

    ImGui::EndTabItem();
}


