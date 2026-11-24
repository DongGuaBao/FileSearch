#include "linux_store.h"
#include <sqlite3.h>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <stdexcept>
#include <iostream>
using namespace fsengine;
void Require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() {
    const auto base = std::getenv("FILESEARCH_TEST_ROOT");
    Require(base != nullptr, "FILESEARCH_TEST_ROOT is required");
    std::filesystem::create_directories(base);
    std::string pattern = std::string(base) + "/size-store-XXXXXX";
    Require(mkdtemp(pattern.data()) != nullptr, "fixture creation");
    struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{pattern};
    const auto file = pattern + "/catalog.db";
    sqlite3* db = nullptr; Require(sqlite3_open(file.c_str(), &db) == SQLITE_OK, "old database open");
    Require(sqlite3_exec(db, "CREATE TABLE entries(path BLOB PRIMARY KEY,name BLOB NOT NULL,inode INTEGER NOT NULL,device INTEGER NOT NULL,directory INTEGER NOT NULL,symlink INTEGER NOT NULL) WITHOUT ROWID; INSERT INTO entries VALUES(X'2F6F6C64',X'6F6C64',1,1,0,0)", nullptr,nullptr,nullptr) == SQLITE_OK, "old schema fixture"); sqlite3_close(db);
    std::string error;
    {
        LinuxStore store; Require(store.Open(file,error),error.c_str());
        std::vector<LinuxScanEntry> loaded; Require(store.Load(loaded,error),error.c_str());
        Require(loaded.size()==1 && !loaded[0].metadata_known, "legacy metadata must remain unknown");
        LinuxScanEntry entry; entry.path="/tree/file"; entry.name="file"; entry.id=5;
        entry.logical_size=12345; entry.allocated_size=16384; entry.modified_ns=1234567890123; entry.checked_at_ms=1234567890; entry.regular=true; entry.metadata_known=true;
        Require(store.Replace({entry},error,"/tree"),error.c_str());
        auto snapshot=store.Snapshot("/tree",true,true,error); Require(!!snapshot,error.c_str());
        auto page=snapshot->NextPage(2); Require(page.entries.size()==1 && page.entries[0].logical_size==12345,"snapshot size");
        entry.logical_size=0; entry.allocated_size=0; Require(store.Apply({entry},{},error),error.c_str());
    }
    LinuxStore reopened; Require(reopened.Open(file,error),error.c_str());
    std::vector<LinuxScanEntry> loaded; Require(reopened.Load(loaded,error),error.c_str());
    Require(loaded.size()==1 && loaded[0].metadata_known && loaded[0].logical_size==0 && loaded[0].regular && loaded[0].modified_ns==1234567890123,"zero-byte metadata must survive restart");
    std::cout<<"size migration and persistence passed\n";
}
