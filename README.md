# PostgreSQL WAL Viewer

A specialized viewer for PostgreSQL Write-Ahead Log (WAL) files, built using Dear ImGui. This tool allows users to inspect WAL records, filter them, and understand the internal operations of their PostgreSQL database.

![WAL Viewer Screenshot](resources/screenshot.png)

## Features

### Core Analysis
- **Automatic Loading**: Scans `pg_wal` and automatically loads the active WAL file.
- **Detailed Parsing**: Decodes WAL records to show LSN, XID, Resource Manager (RMID), Length, and Description.
- **Hex Editor**: Integrated hex viewer highlights the raw bytes corresponding to the selected WAL record.

### Advanced Filtering
- **Resource Manager (RMID)**: Multi-select filter to show/hide specific record types (e.g., Heap, Btree, Transaction).
- **Table & Namespace**: Filter records by specific Tables or Schemas (Namespaces).
- **Smart LSN**: Automatically filters out "future" records beyond `pg_current_wal_lsn()` and garbage data from recycled WAL files.
- **Transaction Highlighting**: Click on any record to highlight all other records belonging to the same Transaction ID (XID).

### Metadata Resolution
- **Live Connection**: Connects to a local PostgreSQL instance to resolve internal OIDs to human-readable names:
    - Database Names
    - Schema (Namespace) Names
    - Table (Relation) Names

### Navigation & UI
- **Jump to LSN**: Quickly navigate to a specific LSN offset.
- **Interactive List**: Click to select, Right-click for context actions (e.g., Show Hexdump).
- **Responsive Design**: Resizable panels for record list and hex view.

## Building the Project

### Prerequisites
- CMake (version 3.10 or higher)
- Make (optional, for Makefile usage)
- A C++ compiler (GCC or Clang)
- Dependencies (often included or fetched):
  - GLFW
  - OpenGL
  - Dear ImGui (included in source)

### Build Instructions

1.  **Clone the repository** (if you haven't already):
    ```bash
    git clone <repository_url>
    cd wal_viewer
    ```

2.  **Create a build directory**:
    ```bash
    mkdir build
    cd build
    ```

3.  **Run CMake**:
    ```bash
    cmake ..
    ```

4.  **Build**:
    ```bash
    make
    ```

### Running the Application

After building, run the executable from the `build` directory:

```bash
./wal_viewer
```

Make sure you have a `pg_wal` directory accessible or configure the application to point to your WAL files.

## Usage

1.  **Launch**: the application will scan `pg_wal` and open the first available file.
2.  **Inspect**: Click on records in the parsed list to highlight their raw bytes in the hex editor.
3.  **Filter**: Use the input fields to filter by LSN or record type.

## License

MIT License
