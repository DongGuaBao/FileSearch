#include "linux_mounts.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    std::istringstream fixture(
        "1 0 8:1 / / rw - ext4 /dev/sda rw\n"
        "2 1 0:2 / /mnt/c rw - 9p C: rw,aname=drvfs\n"
        "3 1 8:2 / /mnt/d rw - ext4 /dev/test rw\n"
        "4 1 0:3 / /proc rw - proc proc rw\n"
        "5 1 8:1 / /mnt/wslg/distro rw - ext4 /dev/sda rw\n"
        "6 1 8:3 / /data\\040disk rw - xfs /dev/sdb rw\n");
    auto mounts = fsengine::ReadLinuxMounts(fixture);
    Require(fsengine::IsLinuxCatalogPath(mounts, "/home/user"), "native path rejected");
    Require(!fsengine::IsLinuxCatalogPath(mounts, "/mnt/c/Windows"), "host C scanned");
    Require(!fsengine::IsLinuxCatalogPath(mounts, "/mnt/d/data"), "host D scanned");
    Require(!fsengine::IsLinuxCatalogPath(mounts, "/mnt/c/../d/data"), "normalized host path scanned");
    Require(!fsengine::IsLinuxCatalogPath(mounts, "/proc/self"), "proc scanned");
    Require(!fsengine::IsLinuxCatalogPath(mounts, "/mnt/wslg/distro/home"), "duplicate distro scanned");
    Require(fsengine::IsLinuxCatalogPath(mounts, "/mnt/data"), "prefix sibling rejected");
    Require(fsengine::IsLinuxCatalogPath(mounts, "/data disk/file"), "escaped mount rejected");
    Require(fsengine::SelectLinuxCatalogMounts(mounts).size() == 2, "unexpected selected mounts");
    std::ifstream live("/proc/self/mountinfo");
    Require(live.good(), "cannot read mountinfo");
    mounts = fsengine::ReadLinuxMounts(live);
    Require(fsengine::IsLinuxCatalogPath(mounts, "/home"), "live native root rejected");
    for (const char* path : {"/mnt/c", "/mnt/d", "/mnt/e", "/proc", "/sys", "/dev"})
        Require(!fsengine::IsLinuxCatalogPath(mounts, path), "live excluded mount accepted");
    for (const auto& mount : fsengine::SelectLinuxCatalogMounts(mounts))
        std::cout << "selected " << mount.path << " " << mount.filesystem << '\n';
    std::cout << "linux_mounts_test passed\n";
}
