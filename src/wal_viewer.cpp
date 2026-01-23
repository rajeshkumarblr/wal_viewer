#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include "wal_parser.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <wal_file_path>" << std::endl;
        return 1;
    }

    const char* file_path = argv[1];
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);

    if (!file) {
        std::cerr << "Error: Failed to open file: " << file_path << std::endl;
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> file_data(size);
    if (!file.read((char*)file_data.data(), size)) {
        std::cerr << "Error: Failed to read file data." << std::endl;
        return 1;
    }

    std::vector<WalRecordInfo> wal_records;
    WalParser wal_parser;
    
    std::cout << "Parsing WAL file: " << file_path << " (" << size << " bytes)" << std::endl;

    wal_parser.Parse(file_data.data(), size, wal_records);

    if (wal_records.empty()) {
        std::cout << "No WAL records found or file format not recognized." << std::endl;
        return 0;
    }

    std::cout << "Found " << wal_records.size() << " records:" << std::endl;
    std::cout << std::left 
              << std::setw(16) << "LSN" 
              << std::setw(10) << "Offset" 
              << std::setw(15) << "Type" 
              << std::setw(8) << "Length" 
              << std::setw(8) << "XID" 
              << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    for (const auto& rec : wal_records) {
        std::cout << std::left 
                  << std::hex << std::uppercase << std::setw(16) << rec.LSN 
                  << std::setw(10) << rec.Offset 
                  << std::setw(15) << rec.Description 
                  << std::dec << std::setw(8) << rec.Length 
                  << std::setw(8) << rec.XID 
                  << std::endl;
    }

    return 0;
}
