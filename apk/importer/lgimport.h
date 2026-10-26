// Let's Go NSP importer: the player's own prod.keys + NSP -> exefs/ + romfs.bin
// in the game data folder. Streams the data, verifies where possible.
// Key values never reach logs or error messages.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lgi {

struct InputFile {
    std::string name;  // file name: *.nsp, prod.keys, title.keys
    int fd = -1;       // readable with pread; the importer does not close it
};

struct Progress {
    std::string phase;  // "scan", "exefs", "romfs", "done"
    uint64_t done = 0, total = 0;
    std::string item;
};

struct Options {
    std::string out_dir;       // game data folder (contains extracted/)
    uint64_t title_id = 0;     // 0x0100187003A36000 for Eevee, 0 = auto-detect
    std::atomic<bool>* cancel = nullptr;
    std::function<void(const Progress&)> on_progress;
    std::function<void(const std::string&)> on_log;
};

// 0 on success; else error message in `error` (no key material).
int run_import(const std::vector<InputFile>& inputs, const Options& opt, std::string& error);

}  // namespace lgi
