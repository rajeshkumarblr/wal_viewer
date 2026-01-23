# Application Flow & Debugging Report

## The Cause of the Freeze
The application became unresponsive because of how **ImGui** handles layout for large datasets when the window size isn't fixed.

1.  **ImGui Layout System**: When you create a window (e.g., "PostgreSQL WAL Viewer") without a fixed size, it attempts to **auto-resize** to fit its content.
2.  **The Hex Editor**: You loaded a **16MB file**. This corresponds to about **1 million lines** of hex data.
3.  **The Bug**: We passed `ImGui::GetContentRegionAvail()` to the Hex Editor. Since the parent window was auto-sizing, it reported "0 remaining space".
4.  **The Consequence**: When `BeginChild` (used by the hex editor) receives a height of `0`, it switches to **"Auto-resize to fit content"** mode. It then tried to calculate the vertical position of **1 million lines** immediately to determine the total height. This massive loop on the CPU caused the application to hang, making it impossible to close.

**The Fix**: We will force the "PostgreSQL WAL Viewer" window to fill the entire application screen. This gives it a fixed, non-zero height. The Hex Editor will then see (for example) "500px height" and only render the ~30 lines visible on screen, which is instantaneous.

---

## Code Flow Explanation (How "Dear ImGui" Works)
Dear ImGui uses an **Immediate Mode** paradigm, which is different from traditional UI toolkits (like Qt or WinForms).

### 1. The Main Loop (`main.cpp`)
Instead of setting up widgets once, the code runs a continuous loop that draws the entire UI from scratch every single frame (e.g., 60 times per second).

```cpp
// main.cpp
while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();           // 1. Read mouse/keyboard inputs
    ImGui_ImplOpenGL3_NewFrame(); // 2. Start a new UI frame
    ImGui::NewFrame();
    
    // 3. Define the UI
    ImGui::Begin("Window Name"); // Start a window
    if (ImGui::Button("Click Me")) { 
        // Logic runs immediately if button is clicked THIS frame
        LoadFile(); 
    }
    ImGui::End();

    // 4. Render
    ImGui::Render();            // Turn UI definitions into draw data
    glfwSwapBuffers(window);    // Show on screen
}
```

### 2. The Hex Editor (`imgui_hex.cpp`)
This component handles the visualization of the binary data.
*   **Clipping (`ImGuiListClipper`)**: This is the crucial optimization. Instead of drawing 1 million rows, it calculates: *"Window is 500px high, each row is 20px. I only need to draw rows 100 to 125."*
*   **Drawing**: It uses `ImDrawList` to issue low-level drawing commands (rectangles, text) for those specific visible rows.

By fixing the window text, we enable the **Clipper** to work correctly, making the app responsive again.
