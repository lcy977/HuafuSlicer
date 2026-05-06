#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

/// Qt GUI 层持有的「文档」：几何模型与完整打印配置（与 Slic3r / Orca 中 Plater 所管数据同构的简化版）。
struct Slic3rDocument
{
    Model                 model;
    DynamicPrintConfig    print_config{DynamicPrintConfig::full_print_config()};

    void clear()
    {
        model = Model();
        print_config = DynamicPrintConfig::full_print_config();
    }
};

} // namespace Slic3r
