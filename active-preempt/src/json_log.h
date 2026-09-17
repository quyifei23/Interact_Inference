#pragma once
#include <string>
#include <sstream>
#include <iomanip>
namespace ap {
inline std::string json_string(const std::string& value) {
    std::ostringstream s; s << '"';
    for (unsigned char c : value) {
        if (c=='"' || c=='\\') s << '\\' << c;
        else if (c<32) s << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else s << c;
    }
    return s.str()+'"';
}
}
