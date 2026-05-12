#ifndef slic3r_GUI_AppEnvironment_hpp_
#define slic3r_GUI_AppEnvironment_hpp_

#include <string>

namespace Slic3r {
namespace GUI {

/// 与 Orca `GUI_App::init_app_config` 一致的策略：若 exe 同目录下已有 `data_dir` 则作为便携数据目录，否则使用系统用户数据目录下的 `HuafuSlicer`。
void bootstrap_data_dir();

/// 在未设置 `resources_dir()` 时，按常见安装/构建布局搜索包含 `profiles` 子目录的 resources；也可用环境变量 `HUAFU_RESOURCES` 覆盖。
void bootstrap_resources_dir();

/// 将 `resources_dir()/profiles` 下的 `*.json` 同步到 `data_dir()/system`（缺失或源文件更新时复制）。对齐 Orca 中 resources → system 的安装语义。
bool sync_resource_profiles_to_system(std::string* error_summary);

} // namespace GUI
} // namespace Slic3r

#endif
