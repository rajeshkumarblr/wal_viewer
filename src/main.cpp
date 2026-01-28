#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string>
#include <vector>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

namespace fs = std::filesystem;

#include "imgui_hex.h"  // Include the hex editor header
#include "wal_parser.h" // Include WAL parser
#include <libpq-fe.h>   // PostgreSQL LibPQ
#include <map>
#include <nlohmann/json.hpp>
#include <sstream> // Added for string building

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using json = nlohmann::json;

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
  return config;
}

// Simple JSON parser to get DB settings
static std::map<std::string, std::string> LoadJsonConfig() {
  std::map<std::string, std::string> config;
  std::ifstream file("connection.json");
  if (!file.is_open())
    return config;

  try {
    json j;
    file >> j;

    if (j.contains("host"))
      config["DB_HOST"] = j["host"];
    if (j.contains("port"))
      config["DB_PORT"] = j["port"];
    if (j.contains("user"))
      config["DB_USER"] = j["user"];
    if (j.contains("password"))
      config["DB_PASSWORD"] = j["password"];
    if (j.contains("dbname"))
      config["DB_NAME"] = j["dbname"];
  } catch (const std::exception &e) {
    printf("Error parsing connection.json: %s\n", e.what());
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
static bool show_hexdump = false; // Default hidden

#ifdef _WIN32
static std::string wal_dir_path =
    "C:\\Program Files\\PostgreSQL\\16\\data\\pg_wal";
#else
// static std::string wal_dir_path = "/var/lib/postgresql/16/main/pg_wal";
static std::string wal_dir_path = "/home/rajesh/proj/wal_viewer/build/pg_wal";
#endif

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

// Table Filter Globals
struct RelItem {
  uint32_t id;
  std::string name;
};
static std::vector<RelItem> table_filter_items;
static int selected_table_idx = -1; // -1 for All

// Namespace Filter Globals
static std::map<uint32_t, std::string> namespace_names; // OID -> Name
static std::map<uint32_t, uint32_t>
    rel_namespace; // RelNode -> Namespace OID (via OID map logic? No,
                   // OID->NamespaceOID)
// Actually we need RelOID -> NamespaceOID.
// But WAL record has RelNode. We map RelNode -> RelOID in ConnectToDB if
// needed? In ConnectToDB: rel_names maps RelNode -> Name. rel_names_oid maps
// OID -> Name. We should map RelNode -> NamespaceOID
static std::map<uint32_t, uint32_t> relnode_to_namespace_oid;

struct NamespaceItem {
  uint32_t id;
  std::string name;
};
static std::vector<NamespaceItem> namespace_filter_items;
static int selected_namespace_idx = -1; // -1 for All

// Active DB State
static std::string active_wal_filename;
static uint64_t active_wal_lsn = 0;
static uint32_t highlighted_xid = 0; // 0 means no specific XID selected

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

// Global UI State for Offset
static uint64_t search_lsn = 0;

// --- Helper Functions ---

static void LoadCurrentFile() {
  if (current_file_idx >= 0 && current_file_idx < files.size()) {
    // fs::path handles separators correctly
    fs::path full_path = fs::path(wal_dir_path) / files[current_file_idx];
    std::string full_path_str = full_path.string();
    strncpy(file_path, full_path_str.c_str(), sizeof(file_path) - 1);
    printf("DEBUG: Loading file: %s\n", file_path);

    std::ifstream file(full_path_str, std::ios::binary | std::ios::ate);
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

        // Auto-update search LSN to file base
        search_lsn = current_file_base_lsn;

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

static bool HexAddressCallback(ImGuiHexEditorState *state, int offset,
                               char *buf, int size) {
  snprintf(buf, size, "%lX", current_file_base_lsn + offset);
  return true;
}

// function moved

static void ConnectToDB() {
  PGconn *conn = PQconnectdb(db_conn_str);
  if (PQstatus(conn) == CONNECTION_OK) {
    snprintf(db_status, sizeof(db_status), "Connected!");

    // Fetch relations
    // Fetch namespaces
    PGresult *res_nsp = PQexec(conn, "SELECT oid, nspname FROM pg_namespace");
    if (PQresultStatus(res_nsp) == PGRES_TUPLES_OK) {
      namespace_names.clear();
      namespace_filter_items.clear();
      int rows = PQntuples(res_nsp);
      for (int i = 0; i < rows; i++) {
        uint32_t oid =
            (uint32_t)strtoul(PQgetvalue(res_nsp, i, 0), nullptr, 10);
        std::string name = PQgetvalue(res_nsp, i, 1);
        namespace_names[oid] = name;
        namespace_filter_items.push_back({oid, name});
      }
      std::sort(namespace_filter_items.begin(), namespace_filter_items.end(),
                [](const NamespaceItem &a, const NamespaceItem &b) {
                  return a.name < b.name;
                });
    }
    PQclear(res_nsp);

    // Fetch relations with namespace info
    PGresult *res = PQexec(
        conn, "SELECT relfilenode, oid, relname, relnamespace FROM pg_class");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      rel_names.clear();
      rel_names_oid.clear();
      relnode_to_namespace_oid.clear();

      int rows = PQntuples(res);
      for (int i = 0; i < rows; i++) {
        uint32_t rnode = (uint32_t)strtoul(PQgetvalue(res, i, 0), nullptr, 10);
        uint32_t oid = (uint32_t)strtoul(PQgetvalue(res, i, 1), nullptr, 10);
        std::string name = PQgetvalue(res, i, 2);
        uint32_t nsp = (uint32_t)strtoul(PQgetvalue(res, i, 3), nullptr, 10);

        rel_names[rnode] = name;
        rel_names_oid[oid] = name;
        relnode_to_namespace_oid[rnode] = nsp;
      }

      // Populate dropdown items
      table_filter_items.clear();
      for (const auto &kv : rel_names) {
        table_filter_items.push_back({kv.first, kv.second});
      }
      std::sort(
          table_filter_items.begin(), table_filter_items.end(),
          [](const RelItem &a, const RelItem &b) { return a.name < b.name; });

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
  // glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE); // Removed to allow manual
  // positioning first

  // Create window with graphics context
  GLFWwindow *window = glfwCreateWindow(1280, 720, "WAL viewer", NULL, NULL);
  if (window == NULL)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // Enable vsync

  // Set Window Icon
  {
    int width, height, channels;
    unsigned char *pixels =
        stbi_load("icon.png", &width, &height, &channels, 4); // Force RGBA
    if (pixels) {
      GLFWimage images[1];
      images[0].width = width;
      images[0].height = height;
      images[0].pixels = pixels;
      glfwSetWindowIcon(window, 1, images);
      stbi_image_free(pixels);
    } else {
      fprintf(stderr, "Failed to load icon.png\n");
    }
  }

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  // Load Fonts
  // - If no fonts are loaded, dear imgui will use the default font. You can
  // also load multiple fonts and use ImGui::PushFont()/PopFont() to select
  // them.
  // - AddFontFromFileTTF() will return the ImFont* so you can store it if you
  // need to select the font among multiple.
  // - If the file cannot be loaded, the function will return NULL. Please
  // handle those errors in your application (e.g. use an assertion, or display
  // an error and quit).
  // - The fonts will be rasterized at a given size (w/ oversampling) and stored
  // into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which
  // ImGui_ImplXXXX_NewFrame below will call.
  // - Read 'docs/FONTS.md' for more and more details.
  // - Butterwick: load a larger font for the user
  // - Butterwick: load a larger font for the user
  const char *linux_font_path =
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  const char *windows_font_path = "C:\\Windows\\Fonts\\arial.ttf";

  if (fs::exists(linux_font_path)) {
    io.Fonts->AddFontFromFileTTF(linux_font_path, 24.0f);
  } else if (fs::exists(windows_font_path)) {
    io.Fonts->AddFontFromFileTTF(windows_font_path, 24.0f);
  } else {
    // Fallback: Scale default font
    io.FontGlobalScale = 1.5f;
  }

  // Scale style to match large font (approx 2.0x for 32px vs 13px default)
  ImGui::GetStyle().ScaleAllSizes(1.5f);

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  // Maximize the window on the primary monitor
  int monitor_count;
  GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
  GLFWmonitor *primary = glfwGetPrimaryMonitor();
  if (primary) {
    int x, y;
    glfwGetMonitorPos(primary, &x, &y);
    glfwSetWindowPos(window, x, y);
  }
  glfwMaximizeWindow(window);

  // Our state
  bool show_demo_window = true;
  bool show_another_window = false;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  // Initialize DB connection string from .env and auto-connect
  static char ui_host[64] = "localhost";
  static char ui_port[16] = "5432";
  static char ui_user[64] = "postgres";
  static char ui_db[64] = "postgres";
  static char ui_pass[64] = "";

  static bool db_conn_init = false;
  if (!db_conn_init) {
    auto env_config = LoadEnvConfig();
    auto json_config = LoadJsonConfig();

    // Merge configs: JSON overrides .env
    for (const auto &[key, value] : json_config) {
      env_config[key] = value;
    }

    if (env_config.count("DB_HOST"))
      strncpy(ui_host, env_config["DB_HOST"].c_str(), sizeof(ui_host) - 1);
    if (env_config.count("DB_PORT"))
      strncpy(ui_port, env_config["DB_PORT"].c_str(), sizeof(ui_port) - 1);
    if (env_config.count("DB_USER"))
      strncpy(ui_user, env_config["DB_USER"].c_str(), sizeof(ui_user) - 1);
    if (env_config.count("DB_NAME"))
      strncpy(ui_db, env_config["DB_NAME"].c_str(), sizeof(ui_db) - 1);
    if (env_config.count("DB_PASSWORD"))
      strncpy(ui_pass, env_config["DB_PASSWORD"].c_str(), sizeof(ui_pass) - 1);

    // Initial Connect
    // Initial Connect
    // Format: postgresql://user:password@host:port/dbname
    std::string conn_str = "postgresql://" + std::string(ui_user) + ":" +
                           std::string(ui_pass) + "@" + std::string(ui_host) +
                           ":" + std::string(ui_port) + "/" +
                           std::string(ui_db);
    strncpy(db_conn_str, conn_str.c_str(), sizeof(db_conn_str) - 1);
    ConnectToDB();

    db_conn_init = true;
  }

  // Configure Hex State Callback
  hex_state.GetAddressNameCallback = HexAddressCallback;

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

        try {
          if (fs::exists(wal_dir_path) && fs::is_directory(wal_dir_path)) {
            for (const auto &entry : fs::directory_iterator(wal_dir_path)) {
              if (entry.is_regular_file()) {
                files.push_back(entry.path().filename().string());
              }
            }
          } else {
            // Try current directory as fallback or just log
            // files.push_back("No files found or invalid dir");
          }
        } catch (const fs::filesystem_error &e) {
          fprintf(stderr, "Filesystem error: %s\n", e.what());
        }

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

    // --- DB Connection ---
    ImGui::Text("Connection:");
    ImGui::SameLine();

    // Calculate available width for the input text
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
    ImGui::InputText("##db_conn_str", db_conn_str, sizeof(db_conn_str));

    ImGui::SameLine();
    if (ImGui::Button("Connect")) {
      ConnectToDB();
    }
    ImGui::SameLine();
    ImGui::Text("%s", db_status);

    ImGui::Separator();

    // --- File Selector ---
    if (!files_loaded) {
      // ... (File loading logic managed by files_loaded flag, effectively
      // skipped if already loaded) Check lines 439-460 in original code for
      // re-scan logic if needed, but here we just display the combo.
    }

    if (files.empty()) {
      ImGui::TextColored(ImVec4(1, 0, 0, 1), "No files found in %s",
                         wal_dir_path.c_str());
    } else {
      ImGui::AlignTextToFramePadding();
      ImGui::Text("WAL File:");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(400);
      if (ImGui::BeginCombo("##walfile", current_file_idx >= 0
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

    // Moved controls: Show Raw Ids, Start LSN, Go
    ImGui::SameLine();

    ImGui::Text("Show Raw Ids?");
    ImGui::SameLine();
    static bool show_raw_ids = false;
    ImGui::Checkbox("##show_raw_ids", &show_raw_ids);

    ImGui::SameLine();

    ImGui::Text("Start LSN:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputScalar("##search_lsn", ImGuiDataType_U64, &search_lsn, NULL,
                       NULL, "%lX", ImGuiInputTextFlags_CharsHexadecimal);

    ImGui::SameLine();
    if (ImGui::Button("Go")) {
      if (!file_data.empty()) {
        wal_records.clear();

        // Calculate offset from LSN
        // Offset = SearchLSN - BaseLSN
        size_t start_offset_calc = 0;
        if (search_lsn > current_file_base_lsn) {
          uint64_t diff = search_lsn - current_file_base_lsn;
          if (diff < file_data.size()) {
            start_offset_calc = (size_t)diff;
          }
        }

        if (start_offset_calc < file_data.size()) {
          wal_parser.Parse(file_data.data() + start_offset_calc,
                           file_data.size() - start_offset_calc, wal_records);
          // Fixup global offsets
          for (auto &r : wal_records) {
            r.Offset += start_offset_calc;
          }
        }
      }
    }

    // Filter Variables
    static bool rmid_filter_states[24];
    static bool rmid_filters_initialized = false;

    ImGui::Separator();

    // --- Filters & Navigation ---

    // Namespace Filter
    ImGui::Text("Filter Namespace:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    const char *current_nsp_name =
        (selected_namespace_idx >= 0 &&
         selected_namespace_idx < namespace_filter_items.size())
            ? namespace_filter_items[selected_namespace_idx].name.c_str()
            : "All";
    if (ImGui::BeginCombo("##nsp_filter", current_nsp_name)) {
      if (ImGui::Selectable("All", selected_namespace_idx == -1)) {
        selected_namespace_idx = -1;
      }
      for (int i = 0; i < namespace_filter_items.size(); i++) {
        bool is_selected = (selected_namespace_idx == i);
        if (ImGui::Selectable(namespace_filter_items[i].name.c_str(),
                              is_selected)) {
          selected_namespace_idx = i;
        }
        if (is_selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();

    // Table Filter (Search Filter)
    ImGui::Text("Filter Table:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    const char *current_table_name =
        (selected_table_idx >= 0 &&
         selected_table_idx < table_filter_items.size())
            ? table_filter_items[selected_table_idx].name.c_str()
            : "All Tables";
    if (ImGui::BeginCombo("##table_filter", current_table_name)) {
      if (ImGui::Selectable("All Tables", selected_table_idx == -1)) {
        selected_table_idx = -1;
      }
      for (int i = 0; i < table_filter_items.size(); i++) {
        bool is_selected = (selected_table_idx == i);
        if (ImGui::Selectable(table_filter_items[i].name.c_str(),
                              is_selected)) {
          selected_table_idx = i;
        }
        if (is_selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();

    // RMID Filter Logic (already initialized above)
    // Multi-select Dropdown
    const char *rmid_names[] = {
        "XLOG",       "Transaction", "Storage", "CLOG",     "Database",
        "Tablespace", "MultiXact",   "RelMap",  "Standby",  "Heap2",
        "Heap",       "Btree",       "Hash",    "Gin",      "Gist",
        "Seq",        "SPGist",      "BRIN",    "CommitTS", "ReplOrigin",
        "Generic",    "LogicalMsg",  "Unknown"};
    int rmid_count = IM_ARRAYSIZE(rmid_names);

    if (!rmid_filters_initialized) {
      // Defaults: Heap(10), Heap2(9), Transaction(1)
      for (int i = 0; i < 24; ++i)
        rmid_filter_states[i] = false;
      if (1 < 24)
        rmid_filter_states[1] = true; // Transaction
      if (9 < 24)
        rmid_filter_states[9] = true; // Heap2
      if (10 < 24)
        rmid_filter_states[10] = true; // Heap
      rmid_filters_initialized = true;
    }

    int selected_count = 0;
    for (int i = 0; i < 24; ++i)
      if (rmid_filter_states[i])
        selected_count++;
    std::string preview_text =
        "(" + std::to_string(selected_count) + " Selected)";

    ImGui::Text("Filter RMID:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo("##rmid_filter_combo", preview_text.c_str())) {
      if (ImGui::Button("All")) {
        for (int i = 0; i < rmid_count; ++i)
          rmid_filter_states[i] = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("None")) {
        for (int i = 0; i < rmid_count; ++i)
          rmid_filter_states[i] = false;
      }
      ImGui::Separator();

      for (int i = 0; i < rmid_count; i++) {
        ImGui::Checkbox(rmid_names[i], &rmid_filter_states[i]);
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Text(" | Records: %zu", wal_records.size());

    ImGui::Separator();

    if (hex_state.Bytes) {
      if (!wal_records.empty()) {

        // Dynamic resizing: Hex Editor gets ~30%, Table gets rest
        float total_h = ImGui::GetContentRegionAvail().y;
        float hex_h = 0.0f;
        float table_h = total_h;

        if (show_hexdump) {
          hex_h = total_h * 0.3f; // Give it a bit more space when visible
          if (hex_h < 150.0f)
            hex_h = 150.0f;
          table_h = total_h - hex_h -
                    ImGui::GetStyle().ItemSpacing.y; // Header height approx
          // Just an estimation, CollapsingHeader takes space too.
          table_h -= 30.0f;
        }

        if (table_h < 100.0f)
          table_h = 100.0f;

        if (ImGui::BeginTable("WalRecords", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable,
                              ImVec2(0, table_h))) {
          ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
          // Increased widths by ~50-80%
          ImGui::TableSetupColumn("LSN", ImGuiTableColumnFlags_WidthFixed,
                                  150.0f);
          ImGui::TableSetupColumn("RMID", ImGuiTableColumnFlags_WidthFixed,
                                  160.0f);
          ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed,
                                  80.0f);
          ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed,
                                  80.0f);
          ImGui::TableSetupColumn("RelNode", ImGuiTableColumnFlags_WidthFixed,
                                  350.0f);
          ImGui::TableSetupColumn("Description",
                                  ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          for (const auto &rec : wal_records) {
            // Apply Filters
            // Search Text removed as per user request
            // if (filter_text[0] != 0) { ... }

            // RMID Filter
            // Check usage of rmid_filter_states
            // Handle Out Of Bounds just in case
            if (rec.RMID >= 0 && rec.RMID < 24) {
              if (!rmid_filter_states[rec.RMID])
                continue;
            } else {
              // Unknown RMID, maybe show it? or hide?
              // Let's hide if not explicitly enabled (but we don't have a
              // checkbox for > 23 usually) Actually our names array goes up
              // to 23.
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

            // Table Filter
            if (selected_table_idx != -1) {
              uint32_t target_node = table_filter_items[selected_table_idx].id;
              bool match = false;
              for (const auto &node : rec.RelFileNodes) {
                if (node.relNode == target_node) {
                  match = true;
                  break;
                }
              }
              if (!match)
                continue;
            }

            ImGui::TableNextRow();

            // Highlight XID logic
            if (highlighted_xid != 0 && rec.XID == highlighted_xid) {
              ImGui::TableSetBgColor(
                  ImGuiTableBgTarget_RowBg0,
                  ImGui::GetColorU32(
                      ImVec4(0.3f, 0.3f, 0.2f, 0.6f))); // Yellow-ish tint
            }

            // Make row selectable
            ImGui::TableNextColumn();
            char lsnBuf[32];
            snprintf(lsnBuf, 32, "%lX", rec.LSN);

            // We use a unique ID for selectable to allow multiple items with
            // same XID to be handled separately if needed, but here we just
            // need to detect click. Note: Selectable returns true on click.
            bool is_selected = (rec.XID != 0 && rec.XID == highlighted_xid);
            if (ImGui::Selectable(lsnBuf, is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
              // Highlight in hex editor
              hex_state.SelectStartByte = rec.Offset;
              hex_state.SelectEndByte = rec.Offset + rec.Length - 1;

              // Update highlighted XID
              highlighted_xid = rec.XID;
            }

            // Context Menu
            if (ImGui::BeginPopupContextItem()) {
              if (ImGui::MenuItem("Show Hexdump")) {
                show_hexdump = true;
                hex_state.SelectStartByte = rec.Offset;
                hex_state.SelectEndByte = rec.Offset + rec.Length - 1;
                highlighted_xid = rec.XID;
                // Note: CollapsingHeader state is managed by show_hexdump
                // variable if we use it with &
              }
              ImGui::EndPopup();
            }

            // Offset column removed

            ImGui::TableNextColumn();
            // Show Name only
            ImGui::Text("%s", wal_parser.GetRmidName(rec.RMID).c_str());

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
                  } else if (rel_names_oid.count(rec.RelFileNodes[i].relNode)) {
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
                  std::string rel = std::to_string(rec.RelFileNodes[i].relNode);
                  if (rel_names.count(rec.RelFileNodes[i].relNode)) {
                    rel = rel_names[rec.RelFileNodes[i].relNode];
                  } else if (rel_names_oid.count(rec.RelFileNodes[i].relNode)) {
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
      ImGui::SetNextItemOpen(show_hexdump);
      if (ImGui::CollapsingHeader("Hex Dump", &show_hexdump)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::BeginHexEditor("##HexEditor", &hex_state, avail);
        ImGui::EndHexEditor();
      }
    } else {
      ImGui::Text("No file loaded.");
    }

    ImGui::End();
    // Removed brace here to keep Block 389 open

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
