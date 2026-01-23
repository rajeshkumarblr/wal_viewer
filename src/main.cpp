#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <stdio.h>
#include <string>
#include <vector>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

#include "imgui_hex.h"  // Include the hex editor header
#include "wal_parser.h" // Include WAL parser
#include <libpq-fe.h>   // PostgreSQL LibPQ
#include <map>
#include <sstream> // Added for string building

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

// Simple .env parser to get DB settings
static std::map<std::string, std::string> LoadEnvConfig() {
  std::map<std::string, std::string> config;
  std::ifstream file(".env");
  if (!file.is_open())
    return config;

  std::string line;
  while (std::getline(file, line)) {
    // Trim whitespace
    line.erase(0, line.find_first_not_of(" \t\r\n"));
    line.erase(line.find_last_not_of(" \t\r\n") + 1);

    // Skip comments and empty lines
    if (line.empty() || line[0] == '#')
      continue;

    size_t delimiterPos = line.find('=');
    if (delimiterPos != std::string::npos) {
      std::string key = line.substr(0, delimiterPos);
      std::string value = line.substr(delimiterPos + 1);
      config[key] = value;
    }
  }
  return config;
}

// --- Global State ---

// Hex Editor State
static ImGuiHexEditorState hex_state;

// File loading state
static std::vector<uint8_t> file_data;
static char file_path[256] = "";
static char error_msg[256] = "";
static std::vector<std::string> files;
static int current_file_idx = -1;
static bool files_loaded = false;
static const std::string wal_dir_path = "/var/lib/postgresql/16/main/pg_wal";
static bool should_scroll_to_bottom = false;
static uint64_t current_file_base_lsn =
    0; // The starting LSN of the current file
static const uint64_t WAL_SEGMENT_SIZE = 16 * 1024 * 1024;

// WAL State
static WalParser wal_parser;
static std::vector<WalRecordInfo> wal_records;

// DB State
static char db_conn_str[512] = "host=localhost dbname=postgres";
static std::map<uint32_t, std::string> rel_names;
static std::map<uint32_t, std::string> rel_names_oid;
static std::map<uint32_t, std::string> db_names;
static char db_status[128] = "Disconnected";

// Active DB State
static std::string active_wal_filename;
static uint64_t active_wal_lsn = 0;

static uint64_t ParseWalFilename(const std::string &filename) {
  if (filename.length() != 24)
    return 0;
  // Format: TLI(8) Log(8) Seg(8)
  std::string logStr = filename.substr(8, 8);
  std::string segStr = filename.substr(16, 8);

  uint32_t logId = 0;
  uint32_t segId = 0;

  try {
    logId = std::stoul(logStr, nullptr, 16);
    segId = std::stoul(segStr, nullptr, 16);
    return ((uint64_t)logId << 32) | ((uint64_t)segId * WAL_SEGMENT_SIZE);
  } catch (...) {
    return 0;
  }
}

// --- Helper Functions ---

static void LoadCurrentFile() {
  if (current_file_idx >= 0 && current_file_idx < files.size()) {
    std::string full_path = wal_dir_path + "/" + files[current_file_idx];
    strncpy(file_path, full_path.c_str(), sizeof(file_path) - 1);
    printf("DEBUG: Loading file: %s\n", file_path);

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (file) {
      std::streamsize size = file.tellg();
      file.seekg(0, std::ios::beg);
      file_data.resize(size);
      if (file.read((char *)file_data.data(), size)) {
        hex_state.Bytes = file_data.data();
        hex_state.MaxBytes = size;
        error_msg[0] = 0;
        wal_records.clear();
        wal_parser.Parse(file_data.data(), size, wal_records);
        should_scroll_to_bottom = true;

        // Update Base LSN
        std::string fname = files[current_file_idx];
        current_file_base_lsn = ParseWalFilename(fname);
      }
    }
  }
}

static uint64_t ParseLSN(const char *lsnStr) {
  uint32_t hi, lo;
  if (sscanf(lsnStr, "%X/%X", &hi, &lo) == 2) {
    return ((uint64_t)hi << 32) | lo;
  }
  return 0;
}

// function moved

static void ConnectToDB() {
  PGconn *conn = PQconnectdb(db_conn_str);
  if (PQstatus(conn) == CONNECTION_OK) {
    snprintf(db_status, sizeof(db_status), "Connected!");

    // Fetch relations
    PGresult *res =
        PQexec(conn, "SELECT relfilenode, oid, relname FROM pg_class");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      rel_names.clear();
      rel_names_oid.clear();
      int rows = PQntuples(res);
      for (int i = 0; i < rows; i++) {
        uint32_t rnode = (uint32_t)strtoul(PQgetvalue(res, i, 0), nullptr, 10);
        uint32_t oid = (uint32_t)strtoul(PQgetvalue(res, i, 1), nullptr, 10);
        std::string name = PQgetvalue(res, i, 2);
        rel_names[rnode] = name;
        rel_names_oid[oid] = name;
      }
      snprintf(db_status, sizeof(db_status), "Fetched %zu relations",
               rel_names.size());

      // Fetch database names
      PGresult *res_db = PQexec(conn, "SELECT oid, datname FROM pg_database");
      if (PQresultStatus(res_db) == PGRES_TUPLES_OK) {
        db_names.clear();
        int rows_db = PQntuples(res_db);
        for (int i = 0; i < rows_db; i++) {
          uint32_t oid =
              (uint32_t)strtoul(PQgetvalue(res_db, i, 0), nullptr, 10);
          std::string name = PQgetvalue(res_db, i, 1);
          db_names[oid] = name;
        }
      }
      PQclear(res_db);

      // Fetch current WAL state
      PGresult *res_wal = PQexec(
          conn,
          "SELECT pg_walfile_name(pg_current_wal_lsn()), pg_current_wal_lsn()");
      if (PQresultStatus(res_wal) == PGRES_TUPLES_OK) {
        active_wal_filename = PQgetvalue(res_wal, 0, 0);
        active_wal_lsn = ParseLSN(PQgetvalue(res_wal, 0, 1));

        // Switch file if possible
        if (files_loaded && !files.empty()) {
          bool found = false;
          for (size_t i = 0; i < files.size(); ++i) {
            if (files[i] == active_wal_filename) {
              current_file_idx = (int)i;
              LoadCurrentFile();
              found = true;
              break;
            }
          }
          // If not found, we don't change
        }
      }
      PQclear(res_wal);

    } else {
      snprintf(db_status, sizeof(db_status), "Query Failed: %s",
               PQerrorMessage(conn));
    }
    PQclear(res);
  } else {
    snprintf(db_status, sizeof(db_status), "Conn Failed: %s",
             PQerrorMessage(conn));
  }
  PQfinish(conn);
}

int main(int, char **) {
  // Setup window
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // GL 3.0 + GLSL 130
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

  // Create window with graphics context
  GLFWwindow *window = glfwCreateWindow(1280, 720, "WAL viewer", NULL, NULL);
  if (window == NULL)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // Enable vsync

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  // Our state
  bool show_demo_window = true;
  bool show_another_window = false;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  // Initialize DB connection string from .env and auto-connect
  static bool db_conn_init = false;
  if (!db_conn_init) {
    auto env_config = LoadEnvConfig();
    std::string conn_str;

    if (env_config.count("DB_HOST"))
      conn_str += "host=" + env_config["DB_HOST"] + " ";
    if (env_config.count("DB_PORT"))
      conn_str += "port=" + env_config["DB_PORT"] + " ";
    if (env_config.count("DB_USER"))
      conn_str += "user=" + env_config["DB_USER"] + " ";
    if (env_config.count("DB_NAME"))
      conn_str += "dbname=" + env_config["DB_NAME"] + " ";
    if (env_config.count("DB_PASSWORD"))
      conn_str += "password=" + env_config["DB_PASSWORD"] + " ";

    if (conn_str.empty() && env_config.count("DB_URL")) {
      conn_str = env_config["DB_URL"];
    }

    if (!conn_str.empty()) {
      strncpy(db_conn_str, conn_str.c_str(), sizeof(db_conn_str) - 1);
      // Auto-connect
      ConnectToDB();
    }
    db_conn_init = true;
  }

  // Main loop
  while (!glfwWindowShouldClose(window)) {
    // Poll and handle events (inputs, window resize, etc.)
    glfwPollEvents();

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // 1. Show the Hex Editor
    {
      const ImGuiViewport *viewport = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(viewport->WorkPos);
      ImGui::SetNextWindowSize(viewport->WorkSize);
      ImGui::Begin("WAL viewer", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoSavedSettings);

      // --- File Selector ---
      if (!files_loaded) {
        files.clear();
        DIR *dir = opendir(wal_dir_path.c_str());
        if (dir) {
          struct dirent *ent;
          while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_REG) { // Regular files only
              files.push_back(ent->d_name);
            }
          }
          closedir(dir);
          std::sort(files.begin(), files.end());

          // Initial load logic
          if (!files.empty()) {
            if (active_wal_filename.empty()) {
              current_file_idx =
                  files.size() - 1; // Default to last if no active WAL known
              LoadCurrentFile();    // Simple default
            } else {
              // Try to find the active WAL
              bool found = false;
              for (size_t i = 0; i < files.size(); ++i) {
                if (files[i] == active_wal_filename) {
                  current_file_idx = (int)i;
                  LoadCurrentFile();
                  found = true;
                  break;
                }
              }
              if (!found) {
                current_file_idx = files.size() - 1; // Fallback
                LoadCurrentFile();
              }
            }
          }
        }
        files_loaded = true;
      }

      if (files.empty()) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "No files found in %s",
                           wal_dir_path.c_str());
      } else {
        if (ImGui::BeginCombo("WAL File", current_file_idx >= 0
                                              ? files[current_file_idx].c_str()
                                              : "Select File")) {
          for (int n = 0; n < files.size(); n++) {
            const bool is_selected = (current_file_idx == n);
            if (ImGui::Selectable(files[n].c_str(), is_selected)) {
              if (current_file_idx != n) {
                current_file_idx = n;
                LoadCurrentFile();
              }
            }
            if (is_selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      }

      ImGui::SameLine();
      if (ImGui::Button("Refresh Folder")) {
        files_loaded = false;
      }
      ImGui::SameLine();
      if (ImGui::Button("Refresh File")) {
        LoadCurrentFile();
      }

      // --- DB Connection ---
      ImGui::Separator();

      ImGui::InputText("DB Conn String", db_conn_str, sizeof(db_conn_str));
      ImGui::SameLine();
      if (ImGui::Button("Connect & Fetch Names")) {
        ConnectToDB();
      }
      ImGui::SameLine();
      ImGui::Text("%s", db_status);

      ImGui::Separator();

      // --- Filters & Navigation ---
      ImGui::Separator();
      static char filter_text[64] = "";
      static int filter_rmid = -1; // -1 = All
      ImGui::InputText("Search Filter", filter_text, sizeof(filter_text));
      ImGui::SameLine();

      // RMID Filter
      const char *rmid_names[] = {
          "All",        "XLOG",       "Transaction", "Storage", "CLOG",
          "Database",   "Tablespace", "MultiXact",   "RelMap",  "Standby",
          "Heap2",      "Heap",       "Btree",       "Hash",    "Gin",
          "Gist",       "Seq",        "SPGist",      "BRIN",    "CommitTS",
          "ReplOrigin", "Generic",    "LogicalMsg"};

      // Default set of interesting RMIDs
      static bool show_interesting_only = true;
      if (ImGui::Checkbox("Show Interesting Only (Heap/Heap2/Xact)",
                          &show_interesting_only)) {
        // Logic to be applied continuously
      }
      ImGui::SameLine();

      static bool show_raw_ids = false;
      ImGui::Checkbox("Show Raw IDs", &show_raw_ids);
      ImGui::SameLine();

      ImGui::Combo("RMID Type", &filter_rmid, rmid_names,
                   IM_ARRAYSIZE(rmid_names));

      ImGui::SameLine();
      static int search_offset = 0;
      ImGui::SetNextItemWidth(100);
      if (ImGui::InputInt("Start Offset", &search_offset, 0)) {
        if (search_offset < 0)
          search_offset = 0;
      }
      ImGui::SameLine();
      if (ImGui::Button("Go")) {
        if (!file_data.empty()) {
          wal_records.clear();
          // Offset logic: simple pointer arithmetic
          size_t start = (size_t)search_offset;
          if (start < file_data.size()) {
            wal_parser.Parse(file_data.data() + start, file_data.size() - start,
                             wal_records);
            // Fixup global offsets
            for (auto &r : wal_records) {
              r.Offset += start;
            }
          }
        }
      }

      ImGui::Separator();

      if (hex_state.Bytes) {
        // ImGui::Text("File Size: %zu bytes", (size_t)hex_state.MaxBytes);

        if (!wal_records.empty()) {

          // Dynamic resizing: Hex Editor gets ~20%, Table gets ~80%
          float total_h = ImGui::GetContentRegionAvail().y;
          float hex_h = total_h * 0.2f;
          if (hex_h < 100.0f)
            hex_h = 100.0f;
          float table_h = total_h - hex_h - ImGui::GetStyle().ItemSpacing.y;
          if (table_h < 100.0f)
            table_h = 100.0f;

          ImGui::Text("WAL Records: %zu", wal_records.size());

          if (ImGui::BeginTable(
                  "WalRecords", 7,
                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                  ImVec2(0, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
            ImGui::TableSetupColumn("LSN", ImGuiTableColumnFlags_WidthFixed,
                                    80.0f);
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed,
                                    80.0f);
            ImGui::TableSetupColumn("RMID", ImGuiTableColumnFlags_WidthFixed,
                                    100.0f);
            ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed,
                                    40.0f);
            ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed,
                                    60.0f);
            ImGui::TableSetupColumn("RelNode", ImGuiTableColumnFlags_WidthFixed,
                                    300.0f);
            ImGui::TableSetupColumn("Description",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto &rec : wal_records) {
              // Apply Filters
              if (filter_text[0] != 0) {
                // Simple substring search in Description
                if (rec.Description.find(filter_text) == std::string::npos)
                  continue;
              }
              if (filter_rmid > 0) {
                // Exact substring match for RMID name from the dropdown
                // filter_rmid 0 is "All", so we skip filtering.
                // otherwise, rmid_names[filter_rmid] must be present in
                // rec.Description. Since rec.Description starts with the RMID
                // Name (e.g. "Btree ..."), this works perfectly.
                if (rec.Description.find(rmid_names[filter_rmid]) ==
                    std::string::npos) {
                  continue;
                }
              }

              // Apply Interesting Only Filter if enabled and no specific RMID
              // filter is selected
              if (show_interesting_only && filter_rmid <= 0) {
                bool is_interesting =
                    (rec.RMID == RM_HEAP_ID || rec.RMID == RM_HEAP2_ID ||
                     rec.RMID == RM_XACT_ID);
                if (!is_interesting)
                  continue;
              }

              // Filter out future records (invalid tail)
              // User requirement: "display records upto the current WAL LSN
              // only. anything beyond that is invalid."
              if (active_wal_lsn > 0 && rec.LSN > active_wal_lsn) {
                continue;
              }

              // Filter out garbage from recycled files (LSN mismatch)
              // If the record's LSN is not within the file's segment range,
              // hide it.
              if (current_file_base_lsn > 0) {
                if (rec.LSN < current_file_base_lsn ||
                    rec.LSN >= current_file_base_lsn + WAL_SEGMENT_SIZE) {
                  continue;
                }
              }

              ImGui::TableNextRow();

              // Make row selectable
              ImGui::TableNextColumn();
              char lsnBuf[32];
              snprintf(lsnBuf, 32, "%lX", rec.LSN);
              if (ImGui::Selectable(lsnBuf, false,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                // Highlight in hex editor
                hex_state.SelectStartByte = rec.Offset;
                hex_state.SelectEndByte = rec.Offset + rec.Length - 1;
                // Need to scroll hex editor to this position?
                // ImGui::BeginHexEditor has no API to "Scroll To".
                // But we can set Selection.
              }

              ImGui::TableNextColumn();
              ImGui::Text("%zX", rec.Offset);

              ImGui::TableNextColumn();
              // Show Name + ID
              ImGui::Text("%s (%d)", wal_parser.GetRmidName(rec.RMID).c_str(),
                          rec.RMID);

              ImGui::TableNextColumn();
              ImGui::Text("%02X", rec.Info);

              ImGui::TableNextColumn();
              ImGui::Text("%u", rec.Length);

              ImGui::TableNextColumn();
              if (rec.RelFileNodes.empty()) {
                ImGui::Text("-");
              } else {
                std::string s;
                for (size_t i = 0; i < rec.RelFileNodes.size(); ++i) {
                  if (i > 0)
                    s += "\n"; // Use newlines for multiple relations

                  if (show_raw_ids) {
                    // spc
                    s += std::to_string(rec.RelFileNodes[i].spcNode) + "/";

                    // db (Name)
                    s += std::to_string(rec.RelFileNodes[i].dbNode);
                    if (db_names.count(rec.RelFileNodes[i].dbNode)) {
                      s += "(" + db_names[rec.RelFileNodes[i].dbNode] + ")";
                    }
                    s += "/";

                    // rel (Name)
                    s += std::to_string(rec.RelFileNodes[i].relNode);
                    if (rel_names.count(rec.RelFileNodes[i].relNode)) {
                      s += "(" + rel_names[rec.RelFileNodes[i].relNode] + ")";
                    } else if (rel_names_oid.count(
                                   rec.RelFileNodes[i].relNode)) {
                      s += "(" + rel_names_oid[rec.RelFileNodes[i].relNode] +
                           "*)"; // * indicates OID match
                    }
                  } else {
                    // Simplified view: db_name:rel_name
                    // DB Name
                    std::string db = std::to_string(rec.RelFileNodes[i].dbNode);
                    if (db_names.count(rec.RelFileNodes[i].dbNode)) {
                      db = db_names[rec.RelFileNodes[i].dbNode];
                    }

                    // Rel Name
                    std::string rel =
                        std::to_string(rec.RelFileNodes[i].relNode);
                    if (rel_names.count(rec.RelFileNodes[i].relNode)) {
                      rel = rel_names[rec.RelFileNodes[i].relNode];
                    } else if (rel_names_oid.count(
                                   rec.RelFileNodes[i].relNode)) {
                      rel = rel_names_oid[rec.RelFileNodes[i].relNode] + "*";
                    }

                    s += db + ":" + rel;
                  }
                }
                ImGui::Text("%s", s.c_str());
              }

              ImGui::TableNextColumn();
              ImGui::Text("%s", rec.Description.c_str());
              // Maybe append XID here?
              if (rec.XID != 0)
                ImGui::SameLine();
              ImGui::TextColored(ImVec4(0.7f, 0.7f, 1, 1), "XID: %u", rec.XID);
            }

            if (should_scroll_to_bottom) {
              ImGui::SetScrollHereY(1.0f);
              should_scroll_to_bottom = false;
            }

            ImGui::EndTable();
          }
        }

        // Call the Hex Editor
        // Calculate remaining space
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::BeginHexEditor("##HexEditor", &hex_state, avail);
        ImGui::EndHexEditor();
      } else {
        ImGui::Text("No file loaded.");
      }

      ImGui::End();
    }

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);

    glViewport(0, 0, display_w, display_h);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                 clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }

  // Cleanup
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
