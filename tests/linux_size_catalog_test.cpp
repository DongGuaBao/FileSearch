#include "linux_catalog.h"
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
using namespace fsengine;
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){
 const char* base=std::getenv("FILESEARCH_TEST_ROOT");Require(base,"test root required");std::filesystem::create_directories(base);
 std::string pattern=std::string(base)+"/size-catalog-XXXXXX";Require(mkdtemp(pattern.data()),"fixture creation");
 struct Cleanup{std::string path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{pattern};
 const auto root=pattern+"/tree",state=pattern+"/state";std::filesystem::create_directories(root+"/empty");std::filesystem::create_directories(state);
 std::string error;LinuxScanEntry directory;directory.path=root+"/empty";directory.name="empty";directory.directory=true;directory.id=1;directory.metadata_known=true;
 LinuxScanEntry file;file.path=root+"/file";file.name="file";file.id=2;file.regular=true;file.metadata_known=true;file.logical_size=123;file.allocated_size=4096;
 {LinuxStore store;Require(store.Open(state+"/catalog.db",error),error.c_str());Require(store.Replace({directory,file},error,root),error.c_str());}
 LinuxQueryCache persisted(state+"/directory-sizes.db",root);
 LinuxCatalog cache;cache.SetQueryCache(&persisted);Require(cache.Start(root,error,state+"/catalog.db",true),error.c_str());
 auto persistedSizes=persisted.Read({root,root+"/empty"});Require(persistedSizes[0].complete&&persistedSizes[0].value->logical_bytes==123&&persistedSizes[1].complete,"startup did not persist directory totals independently");
 auto sizes=cache.DirectorySizes({root,root+"/empty",state});Require(sizes[0].state=="stale"&&sizes[0].size&&sizes[0].size->logical_bytes==123,"cached size must remain usable while validating");Require(sizes[1].size&&sizes[1].size->logical_bytes==0,"cached empty directory");Require(sizes[2].state=="excluded"&&!sizes[2].size,"out-of-root lookup");
 LinuxCatalog cold;const auto coldstate=pattern+"/cold";std::filesystem::create_directory(coldstate);Require(cold.Start(root,error,coldstate+"/catalog.db",true),error.c_str());auto unknown=cold.DirectorySizes({root});Require(unknown[0].state=="calculating"&&!unknown[0].size,"cold root must not appear as an empty directory");
 bool rejected=false;try{cache.DirectorySizes({"relative"});}catch(const std::exception&){rejected=true;}Require(rejected,"relative scope accepted");rejected=false;try{cache.DirectorySizes(std::vector<std::string>(129,root));}catch(const std::exception&){rejected=true;}Require(rejected,"oversized batch accepted");
 std::cout<<"cached/cold directory query states passed\n";
}
