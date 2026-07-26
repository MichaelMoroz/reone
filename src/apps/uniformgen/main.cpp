/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Turns Slang reflection data into static assertions over the uniform structs in
 * reone/graphics/uniforms.h.
 *
 * The std140 layout of those structs has to agree byte for byte with the shader
 * uniform blocks, and nothing used to check it - a mismatch renders garbage
 * rather than failing. Slang computes the offsets; this emits them as assertions
 * the compiler enforces.
 *
 * Reads the JSON produced by slangc -reflection-json from slang/uniforms.slang.
 */

#include <boost/json.hpp>
#include <boost/program_options.hpp>

#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace json = boost::json;

namespace {

constexpr int kStd140BlockAlignment = 16;

int alignUp(int value, int alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

struct NestedStruct {
    std::string name;
    const json::array *fields;
    int stride;
};

/**
 * Emits assertions for one struct's members, and collects any array-of-struct
 * members so their element types can be emitted afterwards.
 *
 * @return the offset one past the last member, before block padding
 */
int emitFields(std::ostream &out,
               const std::string &structName,
               const json::array &fields,
               std::vector<NestedStruct> &nested) {
    int end = 0;
    for (const auto &fieldValue : fields) {
        const auto &field = fieldValue.as_object();
        auto name = json::value_to<std::string>(field.at("name"));
        const auto &binding = field.at("binding").as_object();
        if (!binding.contains("offset")) {
            continue;
        }
        int offset = static_cast<int>(binding.at("offset").as_int64());
        int size = static_cast<int>(binding.at("size").as_int64());
        end = std::max(end, offset + size);

        out << "static_assert(offsetof(" << structName << ", " << name << ") == " << offset
            << ", \"" << structName << "::" << name << " moved; shader layout disagrees\");\n";

        const auto &type = field.at("type").as_object();
        if (json::value_to<std::string>(type.at("kind")) != "array") {
            continue;
        }
        const auto &elementType = type.at("elementType").as_object();
        if (json::value_to<std::string>(elementType.at("kind")) != "struct") {
            continue;
        }
        int stride = static_cast<int>(binding.at("elementStride").as_int64());
        nested.push_back({json::value_to<std::string>(elementType.at("name")),
                          &elementType.at("fields").as_array(),
                          stride});
    }
    return end;
}

} // namespace

int main(int argc, char **argv) {
    try {
        boost::program_options::options_description description;
        description.add_options()                                                  //
            ("reflection", boost::program_options::value<std::string>()->required()) //
            ("output", boost::program_options::value<std::string>()->required());

        boost::program_options::positional_options_description positional;
        positional.add("reflection", 1);
        positional.add("output", 1);

        auto parsed = boost::program_options::command_line_parser(argc, argv)
                          .options(description)
                          .positional(positional)
                          .run();
        boost::program_options::variables_map vars;
        boost::program_options::store(parsed, vars);
        boost::program_options::notify(vars);

        std::ifstream reflectionStream(vars["reflection"].as<std::string>());
        if (!reflectionStream.good()) {
            throw std::runtime_error("Cannot open reflection file: " + vars["reflection"].as<std::string>());
        }
        std::stringstream buffer;
        buffer << reflectionStream.rdbuf();
        auto root = json::parse(buffer.str()).as_object();

        std::ostringstream out;
        out << "/*\n"
            << " * GENERATED FILE - do not edit.\n"
            << " *\n"
            << " * Regenerate with: cmake --build build --target uniformlayout\n"
            << " * Source of truth: slang/uniforms.slang\n"
            << " *\n"
            << " * Asserts that the std140 layout Slang computes for the shader uniform\n"
            << " * blocks matches the C++ structs the engine uploads. A failure here means\n"
            << " * the two have drifted, which at runtime would silently render garbage.\n"
            << " */\n\n"
            << "#pragma once\n\n"
            << "#include <cstddef>\n\n"
            << "#include \"reone/graphics/uniforms.h\"\n\n"
            << "namespace reone {\n\n"
            << "namespace graphics {\n\n";

        std::vector<NestedStruct> nested;
        std::set<std::string> emittedNested;
        int blockCount = 0;

        for (const auto &parameterValue : root.at("parameters").as_array()) {
            const auto &parameter = parameterValue.as_object();
            const auto &type = parameter.at("type").as_object();
            if (json::value_to<std::string>(type.at("kind")) != "constantBuffer") {
                continue;
            }
            const auto &elementType = type.at("elementType").as_object();
            auto structName = json::value_to<std::string>(elementType.at("name"));

            out << "// " << structName << "\n";
            int end = emitFields(out, structName, elementType.at("fields").as_array(), nested);
            out << "static_assert(sizeof(" << structName << ") == " << alignUp(end, kStd140BlockAlignment)
                << ", \"" << structName << " is not the size std140 expects\");\n\n";
            ++blockCount;
        }

        for (const auto &element : nested) {
            if (!emittedNested.insert(element.name).second) {
                continue;
            }
            out << "// " << element.name << ", an array element - its size is the array stride\n";
            emitFields(out, element.name, *element.fields, nested);
            out << "static_assert(sizeof(" << element.name << ") == " << element.stride
                << ", \"" << element.name << " does not match the std140 array stride\");\n\n";
        }

        out << "} // namespace graphics\n\n"
            << "} // namespace reone\n";

        std::ofstream outputStream(vars["output"].as<std::string>());
        if (!outputStream.good()) {
            throw std::runtime_error("Cannot write: " + vars["output"].as<std::string>());
        }
        outputStream << out.str();

        std::cout << "uniformgen: " << blockCount << " blocks, "
                  << emittedNested.size() << " element structs" << std::endl;
        return 0;

    } catch (const std::exception &e) {
        std::cerr << "uniformgen: " << e.what() << std::endl;
        return 1;
    }
}
