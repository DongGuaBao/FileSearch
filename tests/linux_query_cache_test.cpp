#include "linux_query_cache.h"
#include "linux_store.h"
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <thread>
#include <iostream>
#include <stdexcept>
using namespace fsengine;
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){
 const char* base=std::getenv("FILESEARCH_TEST_ROOT");Require(base,"fixture root missing");std::filesystem::create_directories(base);
 std::string pattern=std::string(base)+"/query-cache-XXXXXX";Require(mkdtemp(pattern.data()),"fixture creation");
 struct Cleanup{std::string path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{pattern};
 const auto root=pattern+"/tree";const auto database=pattern+"/directory-sizes.db";
 LinuxDirectorySize value;value.logical_bytes=123;value.files=1;value.checked_at_ms=1234;
 {LinuxQueryCache cache(database,root);cache.Write({{root,value}},false);auto result=cache.Read({root,root+"/none"});Require(result[0].value&&result[0].value->logical_bytes==123&&!result[0].complete&&!result[1].value,"partial or missing cache");cache.Write({{root,value}},true);}
 {LinuxQueryCache cache(database,root);Require(cache.Read({root})[0].complete,"restart lost complete cache");
  std::thread writer([&]{for(int i=0;i<50;++i)cache.Write({{root,value}},true);});for(int i=0;i<50;++i)Require(cache.Read({root})[0].value.has_value(),"concurrent cache read failed");writer.join();
  cache.Write({{root,std::nullopt}},true);Require(!cache.Read({root})[0].value,"deleted directory retained");}
 {LinuxQueryCache cache(database,root+"/changed");Require(!cache.Read({root})[0].value,"another root reused cache");}
 LinuxScanEntry a;a.path=root+"/docs/Hello_%.txt";a.name="Hello_%.txt";a.id=1;a.metadata_known=true;a.regular=true;a.logical_size=4096;
 LinuxScanEntry b=a;b.path=root+"/docs2/Hello_%.txt";b.id=2;
 LinuxScanEntry c=a;c.path=root+"/docs/证书.pem";c.name="证书.pem";c.id=3;
 std::string error;const auto catalog=pattern+"/catalog.db";{LinuxStore store;Require(store.Open(catalog,error),error.c_str());Require(store.Replace({a,b,c},error,root),error.c_str());}
 bool interrupted=false;auto results=SearchLinuxStored(catalog,root,root+"/docs","hello_%",20,interrupted);Require(!interrupted&&results.size()==1&&results[0].path==a.path&&results[0].logical_size==4096,"literal search, scope boundary or metadata");
 results=SearchLinuxStored(catalog,root,root,"证书",20,interrupted);Require(results.size()==1&&results[0].path==c.path,"Chinese search");
 results=SearchLinuxStored(catalog,root+"/changed",root+"/changed","hello",20,interrupted);Require(results.empty(),"another root reused search catalog");
 LinuxDirectorySizes sizes;sizes.Reset(root);LinuxScanEntry dir;dir.path=root+"/docs";dir.directory=true;sizes.Add(dir);sizes.Add(a);auto changes=sizes.TakeChanges();Require(changes.size()==2,"file paths must not inflate directory-cache writes");Require(sizes.TakeChanges().empty(),"changes not drained");sizes.Remove(a);sizes.Remove(dir);changes=sizes.TakeChanges();Require(changes.size()==2,"directory deletion change missing");
 std::cout<<"query cache persistence, concurrent reads, literal search and scope checks passed\n";
}
