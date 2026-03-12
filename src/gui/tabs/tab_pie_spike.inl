if (ImGui::BeginTabItem("Pie Spike")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    ImGui::SeparatorText("Pie Chart Spike Detection");

    if (ImGui::Checkbox("Enable spike detection", &g_config.pieSpike.enabled)) {
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Automatically detects when the F3 pie chart shows a silverfish spawner spike (portal room).");

    if (!g_config.pieSpike.enabled) { ImGui::BeginDisabled(); }

    ImGui::Spacing();
    ImGui::SeparatorText("Detection Settings");

    if (ImGui::SliderFloat("Orange ratio target", &g_config.pieSpike.orangeRatioTarget, 0.0f, 1.0f, "%.3f")) {
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Target ratio of orange (block entities) to total colored pixels. Typical spawner spike: 0.10-0.20.");

    if (ImGui::SliderFloat("Tolerance", &g_config.pieSpike.tolerance, 0.001f, 0.2f, "%.3f")) {
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("How far the measured ratio can deviate from the target and still trigger.");

    if (ImGui::SliderFloat("Color match threshold", &g_config.pieSpike.colorThreshold, 0.01f, 0.5f, "%.3f")) {
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Maximum Euclidean distance (0-1 per channel) for a pixel to be classified as orange or green.");

    ImGui::Spacing();
    ImGui::SeparatorText("Reference Colors");

    float orangeCol[3] = { g_config.pieSpike.orangeReference.r, g_config.pieSpike.orangeReference.g, g_config.pieSpike.orangeReference.b };
    if (ImGui::ColorEdit3("Orange reference", orangeCol)) {
        g_config.pieSpike.orangeReference.r = orangeCol[0];
        g_config.pieSpike.orangeReference.g = orangeCol[1];
        g_config.pieSpike.orangeReference.b = orangeCol[2];
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("The pie chart color for block entities (orange slice). Default: RGB(233, 109, 77).");

    float greenCol[3] = { g_config.pieSpike.greenReference.r, g_config.pieSpike.greenReference.g, g_config.pieSpike.greenReference.b };
    if (ImGui::ColorEdit3("Green reference", greenCol)) {
        g_config.pieSpike.greenReference.r = greenCol[0];
        g_config.pieSpike.greenReference.g = greenCol[1];
        g_config.pieSpike.greenReference.b = greenCol[2];
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("The pie chart color for unspecified entities (green slice). Default: RGB(69, 204, 101).");

    ImGui::Spacing();
    ImGui::SeparatorText("Timing");

    if (ImGui::InputInt("Sample rate (ms)", &g_config.pieSpike.sampleRateMs, 50, 200)) {
        if (g_config.pieSpike.sampleRateMs < 50) g_config.pieSpike.sampleRateMs = 50;
        if (g_config.pieSpike.sampleRateMs > 5000) g_config.pieSpike.sampleRateMs = 5000;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("How often (in ms) to sample the pie chart. Lower = more responsive, higher = less GPU usage.");

    if (ImGui::InputInt("Cooldown (ms)", &g_config.pieSpike.cooldownMs, 500, 1000)) {
        if (g_config.pieSpike.cooldownMs < 500) g_config.pieSpike.cooldownMs = 500;
        if (g_config.pieSpike.cooldownMs > 30000) g_config.pieSpike.cooldownMs = 30000;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Minimum time between alerts to prevent spam.");

    if (ImGui::InputInt("Capture size (px)", &g_config.pieSpike.captureSize, 16, 64)) {
        if (g_config.pieSpike.captureSize < 32) g_config.pieSpike.captureSize = 32;
        if (g_config.pieSpike.captureSize > 512) g_config.pieSpike.captureSize = 512;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Size of the square region captured around the pie chart center.");

    ImGui::Spacing();
    ImGui::SeparatorText("Alerts");

    if (ImGui::Checkbox("Visual alert (screen flash)", &g_config.pieSpike.visualAlert)) {
        g_configIsDirty = true;
    }

    if (ImGui::Checkbox("Sound alert", &g_config.pieSpike.soundAlert)) {
        g_configIsDirty = true;
    }

    {
        char soundBuf[512];
        strncpy(soundBuf, g_config.pieSpike.soundPath.c_str(), sizeof(soundBuf) - 1);
        soundBuf[sizeof(soundBuf) - 1] = '\0';
        if (ImGui::InputText("Custom sound path (.wav)", soundBuf, sizeof(soundBuf))) {
            g_config.pieSpike.soundPath = soundBuf;
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to a .wav file for custom alert sound. Leave empty for system beep.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Live Status");

    float currentRatio = g_pieSpikeLastOrangeRatio.load(std::memory_order_relaxed);
    ImGui::Text("Current orange ratio: %.4f", currentRatio);

    // Visual bar showing ratio vs target
    float target = g_config.pieSpike.orangeRatioTarget;
    float tol = g_config.pieSpike.tolerance;
    ImGui::ProgressBar(currentRatio, ImVec2(-1, 0), "");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetContentRegionAvail().x - ImGui::CalcItemWidth());

    bool alertActive = g_pieSpikeAlertActive.load(std::memory_order_relaxed);
    if (alertActive) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "SPIKE DETECTED");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No spike");
    }

    ImGui::Text("Target range: %.3f - %.3f", target - tol, target + tol);

    if (!g_config.pieSpike.enabled) { ImGui::EndDisabled(); }

    ImGui::EndTabItem();
}
