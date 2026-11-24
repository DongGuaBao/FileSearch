#include "linux_catalog.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/fanotify.h>
#include <thread>
#include <iostream>
#include <stdexcept>
using namespace fsengine;
void Require(bool ok,const std::string& text){if(!ok)throw std::runtime_error(text);}
namespace fsengine {
struct LinuxCatalogTestAccess {
 static bool Publish(LinuxCatalog& catalog,const std::string& root,std::string& error,bool provisional=false){
  std::ifstream input("/proc/self/mountinfo");auto scan=ScanLinuxTree(root,ReadLinuxMounts(input));if(!scan.Complete())return false;
  if(provisional)for(const auto& entry:scan.entries)catalog.directory_sizes_.Add(entry);
  if(!catalog.AssignIds(scan.entries,error)||!catalog.store_->Replace(scan.entries,error,root))return false;
  catalog.recovering_=true;return catalog.PublishSnapshot(scan.entries,scan.root_object_key,error);
 }
 static void Activate(LinuxCatalog& catalog,const std::string& root){
  catalog.recovering_=false;catalog.startup_cache_=false;
  catalog.directory_paths_[LinuxFanotify::DirectoryHandle(root)].insert(root);
  for(const auto& [path,entry]:catalog.entries_)if(entry.directory)catalog.directory_paths_[entry.object_key].insert(path);
 }
 static bool Apply(LinuxCatalog& catalog,const std::vector<LinuxNotification>& notifications,std::string& error){return catalog.ApplyNotifications(notifications,error);}
 static void Queue(LinuxCatalog& catalog,const std::string& path){std::lock_guard lock(catalog.mutex_);catalog.QueueRepairLocked(path);}
 static void Repair(LinuxCatalog& catalog,const std::string& path){std::lock_guard lock(catalog.mutex_);catalog.QueueRepairLocked(path,true);}
};
}
static void Write(const std::filesystem::path& path,std::size_t size){std::ofstream file(path);file<<std::string(size,'x');}
int main(){
 const char* base=std::getenv("FILESEARCH_TEST_ROOT");Require(base,"fixture root missing");std::filesystem::create_directories(base);
 std::string fixture=std::string(base)+"/local-repair-XXXXXX";Require(mkdtemp(fixture.data()),"fixture creation");
 struct Cleanup{std::string path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{fixture};
 const auto root=fixture+"/tree",state=fixture+"/state";std::filesystem::create_directories(root+"/d");std::filesystem::create_directory(state);
 Write(root+"/a",100);Write(root+"/keep",50);Write(root+"/d/old",10);
 std::ifstream input("/proc/self/mountinfo");auto mounts=ReadLinuxMounts(input);auto scan=ScanLinuxTree(root,mounts);Require(scan.Complete(),"fixture scan failed");
 std::uint64_t next=1;for(auto& entry:scan.entries)entry.id=next++;
 std::string error;{LinuxStore store;Require(store.Open(state+"/catalog.db",error),error);Require(store.Replace(scan.entries,error,root),error);}
 LinuxQueryCache cached(state+"/directory-sizes.db",root);LinuxCatalog catalog;catalog.SetQueryCache(&cached);Require(catalog.Start(root,error,state+"/catalog.db",true),error);LinuxCatalogTestAccess::Activate(catalog,root);
 auto epoch=catalog.Position().epoch;const auto keep=catalog.Search("keep",root,10)[0].id;
 auto check=[&]{
  Require(catalog.Ready()&&catalog.ScanRetryCount()==0&&catalog.Position().epoch==epoch,"ordinary path repair triggered global recovery");
  Require(catalog.Search("keep",root,10)[0].id==keep,"unrelated file identity changed");
  std::uint64_t expected=0;for(const auto& item:std::filesystem::recursive_directory_iterator(root))if(!item.is_symlink()&&item.is_regular_file())expected+=item.file_size();
  auto sizes=catalog.DirectorySizes({root});Require(sizes[0].size&&sizes[0].size->logical_bytes==expected,"directory aggregate differs from independent filesystem sum");
 };
 const auto parent=LinuxFanotify::DirectoryHandle(root);
 auto apply=[&](std::uint64_t mask,const std::string& name,const std::string& handle){LinuxNotification event;event.mask=mask;event.name=name;event.object_handle=handle;event.directory_handle=parent;Require(LinuxCatalogTestAccess::Apply(catalog,{event},error),error);check();};
 const auto original=catalog.Search("a",root,10)[0].id;
 apply(FAN_CREATE,"a","obsolete-generation");Require(catalog.Search("a",root,10)[0].id==original,"already indexed replacement was recreated");
 Write(root+"/replacement",999);std::filesystem::rename(root+"/replacement",root+"/a");apply(FAN_CREATE,"a","obsolete-generation");Require(catalog.Search("a",root,10)[0].id!=original,"replacement reused old entry ID");
 apply(FAN_DELETE,"a","obsolete-generation");Require(!catalog.Search("a",root,10).empty(),"old delete removed the live replacement");
 std::filesystem::remove(root+"/a");apply(FAN_DELETE,"a","obsolete-generation");Require(catalog.Search("a",root,10).empty(),"absent path was not removed locally");
 Write(root+"/a",77);apply(FAN_CREATE,"a","obsolete-generation");Write(root+"/b",33);
 LinuxNotification rename;rename.mask=FAN_RENAME;rename.name="b";rename.old_name="a";rename.directory_handle=rename.old_directory_handle=parent;rename.object_handle="obsolete-generation";
 Require(LinuxCatalogTestAccess::Apply(catalog,{rename},error),error);check();Require(!catalog.Search("a",root,10).empty()&&!catalog.Search("b",root,10).empty(),"old rename moved the replacement source");
 std::filesystem::rename(root+"/d",fixture+"/retired");std::filesystem::create_directory(root+"/d");Write(root+"/d/new",200);apply(FAN_CREATE|FAN_ONDIR,"d","obsolete-directory");Require(catalog.Search("old",root,10).empty()&&!catalog.Search("new",root,10).empty(),"directory repair did not replace the scoped namespace");
 Write(root+"/d/extra",11);LinuxCatalogTestAccess::Queue(catalog,root+"/d");LinuxCatalogTestAccess::Queue(catalog,root+"/d/extra");Require(LinuxCatalogTestAccess::Apply(catalog,{},error),error);check();Require(!catalog.Search("extra",root,10).empty(),"parent repair swallowed a child repair");
 Write(root+"/d/extra",12);LinuxCatalogTestAccess::Queue(catalog,root+"/d/extra");LinuxCatalogTestAccess::Queue(catalog,root+"/d");Require(LinuxCatalogTestAccess::Apply(catalog,{},error),error);check();
 bool moved=false;LinuxScanOptions options;options.observed=[&](const LinuxScanEntry&){if(!moved){moved=true;std::filesystem::rename(root+"/d",fixture+"/raced");std::filesystem::create_directory(root+"/d");}};catalog.SetScanOptions(options);LinuxCatalogTestAccess::Repair(catalog,root+"/d");Require(LinuxCatalogTestAccess::Apply(catalog,{},error),error);Require(catalog.Ready()&&catalog.ScanRetryCount()==0,"local scan race escalated to global recovery");
 catalog.SetScanOptions({});std::this_thread::sleep_for(std::chrono::milliseconds(300));Require(LinuxCatalogTestAccess::Apply(catalog,{},error),error);check();Require(catalog.Search("new",root,10).empty(),"deferred subtree repair did not converge");
 std::filesystem::remove(root+"/d");std::filesystem::create_directory_symlink(fixture+"/retired",root+"/d");apply(FAN_CREATE,"d","obsolete-directory");Require(catalog.Search("old",root,10).empty(),"directory symlink traversed during local repair");
 {LinuxStore store;std::vector<LinuxScanEntry> entries;Require(store.Open(state+"/catalog.db",error),error);Require(store.Load(entries,error),error);bool found=false;for(const auto& entry:entries){if(entry.path==root+"/a")found=entry.logical_size==77;Require(entry.path!=root+"/d/old","obsolete durable descendant retained");}Require(found,"local file repair not persisted");}
 // Differential whole-catalog publication must preserve unchanged IDs/names
 // while updating added, removed and replacement paths without a second index.
 Write(root+"/new-full",88);std::filesystem::remove(root+"/b");Write(root+"/full-temp",66);std::filesystem::rename(root+"/full-temp",root+"/a");
 Require(LinuxCatalogTestAccess::Publish(catalog,root,error),error);epoch=catalog.Position().epoch;check();Require(catalog.Search("b",root,10).empty()&&!catalog.Search("new-full",root,10).empty(),"differential publication missed namespace changes");Require(cached.Read({root})[0].complete,"published cache not complete");
 const auto coldstate=fixture+"/cold";std::filesystem::create_directory(coldstate);LinuxQueryCache coldcache(coldstate+"/sizes.db",root);LinuxCatalog cold;cold.SetQueryCache(&coldcache);Require(cold.Start(root,error,coldstate+"/catalog.db",true),error);Require(LinuxCatalogTestAccess::Publish(cold,root,error,true),error);Require(cold.DirectorySizes({root})[0].size->logical_bytes==catalog.DirectorySizes({root})[0].size->logical_bytes,"provisional totals counted twice at publication");
 std::cout<<"obsolete create/delete/rename, replacements, scoped/raced repair, symlink fences and durable sizes passed\n";
}
