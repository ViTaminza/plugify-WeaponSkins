#pragma once
#include "models.hpp"
#include <functional>
#include <memory>
#include <plgdb/db.hpp>
namespace ws {
using LoadCallback = std::function<void(Snapshot, std::string)>;
using SaveCallback = std::function<void(bool, std::string)>;
class Storage {
public:
    Storage(PisexConfig config, std::unique_ptr<plgdb::Database> database);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    void LoadPlayer(uint64_t steamId, LoadCallback callback);
    // Writes only the counter, preserving simultaneous website changes. team is engine 2/3.
    void SaveStatTrak(uint64_t steamId, int team, uint16_t definition, int delta, SaveCallback callback = {});
    size_t Poll();
    size_t Pending() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ws
