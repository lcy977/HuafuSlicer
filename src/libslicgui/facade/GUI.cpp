#include "GUI.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/any.hpp>

namespace Slic3r {
namespace GUI {

boost::filesystem::path into_path(const QString& str)
{
#ifdef _WIN32
    // 与 admesh 中 boost::nowide::fopen 一致：后续用 narrow(wstring) 得到 UTF-8 供 nowide 打开。
    return boost::filesystem::path(str.toStdWString());
#else
    return boost::filesystem::path(str.toUtf8().constData());
#endif
}

} // namespace GUI
} // namespace Slic3r
