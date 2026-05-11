#include "GUI.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/any.hpp>

namespace Slic3r { 
    namespace GUI {
        boost::filesystem::path into_path(const QString &str){
            return boost::filesystem::path(str.toUtf8().data());
        }
    }
}