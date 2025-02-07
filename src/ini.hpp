#pragma once

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

class IniParser {
   public:
    bool load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open file: " << filename << '\n';
            return false;
        }

        std::string line, section;
        while (std::getline(file, line)) {
            line = trim(line);

            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            if (line[0] == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                // std::cout << "Section: " << section << '\n';
            } else {
                auto delimiterPos = line.find('=');
                if (delimiterPos == std::string::npos) continue;

                std::string key = trim(line.substr(0, delimiterPos));
                std::string value = trim(line.substr(delimiterPos + 1));
                auto commentPos = value.find_first_of(";#");
                if (commentPos != std::string::npos) {
                    value = trim(value.substr(0, commentPos));
                }
                config_[section][key] = value;
                // std::cout << "Key: " << key << ", Value: " << value << '\n';
            }
        }
        return true;
    }

    std::string get(const std::string& section, const std::string& key) const {
        try {
            return config_.at(section).at(key);
        } catch (const std::out_of_range&) {
            std::cerr << "Error: Key not found [" << section << "][" << key
                      << "]\n";
            return "";  // by default, return an empty string
        }
    }

    int getInt(const std::string& section, const std::string& key) const {
        auto ret = get(section, key);
        try {
            return std::stoi(ret);
        } catch (const std::exception& e) {
            std::cerr << "Error: Conversion to integer for the key " << key
                      << " is failed.\n";
            return -1;
        }
    }

   private:
    std::map<std::string, std::map<std::string, std::string>> config_;

    static std::string trim(const std::string& str) {
        const char* whitespace = " \t\n\r\f\v";
        size_t start = str.find_first_not_of(whitespace);
        size_t end = str.find_last_not_of(whitespace);
        return (start == std::string::npos)
                   ? ""
                   : str.substr(start, end - start + 1);
    }
};
