#pragma once
#include <iostream>
#include <string>

namespace std
{
    inline bool start_with(const std::string &str, const std::string &prefix)
    {
        return str.rfind(prefix, 0) == 0;
    }
    inline bool have_str(const std::string &str, const std::string &target)
    {
        return str.find(target) != std::string::npos;
    }

    inline std::string sub_str(std::string str, const std::string &target)
    {
        size_t pos = str.find(target);
        if (pos != std::string::npos)
        {
            str.erase(pos, target.length());
        }
        return str;
    }
    inline std::string get_str_until(const std::string &str, const std::string &until_target)
    {
        if (until_target.empty() || until_target == " ")
        {
            size_t pos = str.find(' ');
            if (pos != std::string::npos)
            {
                return str.substr(0, pos);
            }
            return str;
        }

        size_t pos = str.find(until_target);
        if (pos != std::string::npos)
        {
            return str.substr(0, pos);
        }

        return str;
    }
}