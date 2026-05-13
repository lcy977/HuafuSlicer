#ifndef libslicgui_GUI_hpp_
#define libslicgui_GUI_hpp_

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include <QString>

namespace boost { class any; }
namespace boost::filesystem { class path; }

namespace Slic3r {
namespace GUI {

boost::filesystem::path into_path(const QString& str);

} // namespace GUI
} // namespace Slic3r

#endif
