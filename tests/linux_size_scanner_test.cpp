#include "linux_scanner.h"
#include "linux_mounts.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <iostream>
#include <limits>
using namespace fsengine;
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){
 const char* base=std::getenv("FILESEARCH_TEST_ROOT");Require(base,"test root is required");std::filesystem::create_directories(base);
 std::string pattern=std::string(base)+"/size-scan-XXXXXX";Require(mkdtemp(pattern.data()),"fixture creation");
 struct Cleanup{std::string path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{pattern};
 auto root=std::filesystem::path(pattern)/"tree";std::filesystem::create_directories(root/"empty");
 {std::ofstream file(root/"sparse");file.seekp(1048575);file.put('x');}
 std::filesystem::create_symlink(root,root/"link");
 std::ifstream input("/proc/self/mountinfo");auto mounts=ReadLinuxMounts(input);
 std::size_t observed=0;LinuxScanOptions progress;progress.observed=[&](const LinuxScanEntry& entry){Require(entry.metadata_known,"progress before metadata");++observed;};
 auto scan=ScanLinuxTree(root.string(),mounts,{},false,progress);Require(observed==3,"incremental observations missing");Require(scan.Complete(),"scan must complete");Require(scan.entries.size()==3,"symlink traversal or missing entries");
 bool sparse=false;for(const auto& entry:scan.entries){Require(entry.metadata_known,"metadata missing");if(entry.name=="sparse"){struct stat st{};Require(lstat(entry.path.c_str(),&st)==0,"stat fixture");Require(entry.logical_size==1048576 && entry.allocated_size==static_cast<std::uint64_t>(st.st_blocks)*512,"sparse allocation mismatch");sparse=true;}}
 Require(sparse,"missing sparse entry");LinuxScanOptions options;options.cancelled=[] {return true;};auto cancelled=ScanLinuxTree(root.string(),mounts,{},false,options);Require(cancelled.cancelled&&!cancelled.Complete(),"cancelled scan must not publish a complete snapshot");
 struct stat future{};future.st_mode=S_IFREG;future.st_mtim.tv_sec=std::numeric_limits<std::int64_t>::max();LinuxScanEntry edge;UpdateLinuxMetadata(edge,future);Require(edge.modified_ns==std::numeric_limits<std::int64_t>::max(),"far-future mtime overflow");
 std::cout<<"scanner metadata, symlink and cancellation passed\n";
}
