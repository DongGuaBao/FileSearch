#include "linux_query_cache.h"
#include <chrono>
#include <stdexcept>
#include <memory>
#include <filesystem>
namespace fsengine {
namespace {
void Check(sqlite3* db, int code) { if (code!=SQLITE_OK && code!=SQLITE_DONE && code!=SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db)); }
void Exec(sqlite3* db,const char* sql) { Check(db,sqlite3_exec(db,sql,nullptr,nullptr,nullptr)); }
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db,const char* sql) { sqlite3_stmt* p=nullptr;Check(db,sqlite3_prepare_v2(db,sql,-1,&p,nullptr));return {p,sqlite3_finalize}; }
void Bind(sqlite3* db,sqlite3_stmt* p,int n,const std::string& value) { Check(db,sqlite3_bind_blob(p,n,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)); }
std::string Text(sqlite3_stmt* p,int n) { const auto* value=sqlite3_column_blob(p,n);return value?std::string(static_cast<const char*>(value),sqlite3_column_bytes(p,n)):std::string{}; }
bool Within(const std::string& p,const std::string& root) { return root=="/"||p==root||p.starts_with(root+"/"); }
}
LinuxQueryCache::LinuxQueryCache(const std::string& database,const std::string& root) : database_(database) {
    Check(db_,sqlite3_open_v2(database.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr));
    try {
        sqlite3_busy_timeout(db_,250);
        Exec(db_,"PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS metadata(root BLOB); CREATE TABLE IF NOT EXISTS sizes(path BLOB PRIMARY KEY,logical INTEGER,allocated INTEGER,files INTEGER,unknown INTEGER,checked INTEGER,complete INTEGER) WITHOUT ROWID;");
        auto statement=Prepare(db_,"SELECT root FROM metadata LIMIT 1");
        const auto code=sqlite3_step(statement.get());Check(db_,code);
        const bool same=code==SQLITE_ROW&&Text(statement.get(),0)==root;statement.reset();
        if(!same) {
            Exec(db_,"BEGIN; DELETE FROM sizes; DELETE FROM metadata;");
            auto insert=Prepare(db_,"INSERT INTO metadata VALUES(?)");Bind(db_,insert.get(),1,root);Check(db_,sqlite3_step(insert.get()));Exec(db_,"COMMIT");
        }
    } catch(...) {sqlite3_close(db_);db_=nullptr;throw;}
}
LinuxQueryCache::~LinuxQueryCache(){if(db_)sqlite3_close(db_);}
void LinuxQueryCache::Clear(){std::lock_guard lock(mutex_);Exec(db_,"DELETE FROM sizes");}
std::vector<LinuxQueryCache::Size> LinuxQueryCache::Read(const std::vector<std::string>& paths){
    sqlite3* reader=nullptr;
    const auto opened=sqlite3_open_v2(database_.c_str(),&reader,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(reader,sqlite3_close);Check(reader,opened);sqlite3_busy_timeout(reader,100);
    std::vector<Size> result;
    auto statement=Prepare(reader,"SELECT logical,allocated,files,unknown,checked,complete FROM sizes WHERE path=?");
    for(const auto& path:paths){
        sqlite3_reset(statement.get());sqlite3_clear_bindings(statement.get());Bind(reader,statement.get(),1,path);
        const auto code=sqlite3_step(statement.get());Check(reader,code);Size item;
        if(code==SQLITE_ROW){LinuxDirectorySize value;value.logical_bytes=sqlite3_column_int64(statement.get(),0);value.allocated_bytes=sqlite3_column_int64(statement.get(),1);value.files=sqlite3_column_int64(statement.get(),2);value.unknown=sqlite3_column_int64(statement.get(),3);value.checked_at_ms=sqlite3_column_int64(statement.get(),4);item={value,sqlite3_column_int(statement.get(),5)!=0};}
        result.push_back(item);
    }return result;
}
void LinuxQueryCache::Write(const std::vector<std::pair<std::string,std::optional<LinuxDirectorySize>>>& changes,bool complete,bool replace){
    if(changes.empty()&&!replace)return;
    std::lock_guard lock(mutex_);Exec(db_,"BEGIN IMMEDIATE");
    try{
        if(replace)Exec(db_,"DELETE FROM sizes");
        auto statement=Prepare(db_,"INSERT OR REPLACE INTO sizes VALUES(?,?,?,?,?,?,?)");
        auto remove=Prepare(db_,"DELETE FROM sizes WHERE path=?");
        for(const auto& [path,value]:changes){
            auto* p=value?statement.get():remove.get();sqlite3_reset(p);sqlite3_clear_bindings(p);Bind(db_,p,1,path);
            if(value){sqlite3_bind_int64(p,2,value->logical_bytes);sqlite3_bind_int64(p,3,value->allocated_bytes);sqlite3_bind_int64(p,4,value->files);sqlite3_bind_int64(p,5,value->unknown);sqlite3_bind_int64(p,6,value->checked_at_ms);sqlite3_bind_int(p,7,complete&&!value->unknown);}
            Check(db_,sqlite3_step(p));
        }Exec(db_,"COMMIT");
    }catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
std::vector<LinuxScanEntry> SearchLinuxStored(const std::string& database,const std::string& root,const std::string& scope,const std::string& query,std::size_t limit,bool& interrupted){
    interrupted=false;if(!Within(scope,root)||scope.empty()||scope.front()!='/')throw std::runtime_error("invalid search scope");
    sqlite3* raw=nullptr;const auto code=sqlite3_open_v2(database.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);Check(raw,code);sqlite3_busy_timeout(raw,100);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    sqlite3_progress_handler(raw,1000,[](void* context){return std::chrono::steady_clock::now()>*static_cast<std::chrono::steady_clock::time_point*>(context)?1:0;},&deadline);
    auto metadata=Prepare(raw,"SELECT value FROM catalog_metadata WHERE key='root'");
    const auto root_code=sqlite3_step(metadata.get());Check(raw,root_code);
    if(root_code!=SQLITE_ROW||Text(metadata.get(),0)!=root)return {};metadata.reset();
    const auto prefix=scope=="/"?scope:scope+"/";auto end=prefix;end.back()++;
    auto statement=Prepare(raw,"SELECT path,name,directory,symlink,logical_size,metadata_known FROM entries WHERE path>=? AND path<? AND instr(lower(CAST(name AS TEXT)),lower(CAST(? AS TEXT)))>0 ORDER BY path LIMIT ?");
    Bind(raw,statement.get(),1,prefix);Bind(raw,statement.get(),2,end);Bind(raw,statement.get(),3,query);sqlite3_bind_int64(statement.get(),4,limit);
    std::vector<LinuxScanEntry> results;
    for(;;){const auto step=sqlite3_step(statement.get());if(step==SQLITE_INTERRUPT){interrupted=true;break;}Check(raw,step);if(step==SQLITE_DONE)break;LinuxScanEntry entry;entry.path=Text(statement.get(),0);entry.name=Text(statement.get(),1);entry.directory=sqlite3_column_int(statement.get(),2);entry.symlink=sqlite3_column_int(statement.get(),3);entry.logical_size=sqlite3_column_int64(statement.get(),4);entry.metadata_known=sqlite3_column_int(statement.get(),5);results.push_back(std::move(entry));}
    return results;
}
}
