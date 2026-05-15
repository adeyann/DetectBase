#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <type_traits>
namespace MGEN
{
    // Usage :
    // if constexpr (is_string_like<Key>::value) { std::string k { key }; }
    template<typename T>
    struct is_string_like : std::disjunction<
        std::is_convertible<T, std::string>,
        std::is_same<std::decay_t<T>, std::string_view>,
        std::is_same<std::decay_t<T>, const char*>
    > {};

    // 문자열을 대문자로 변환
    std::string ToUpperCase( std::string_view str_view );

    // snake/pascal 문자열을 CamelCase로 변환
    std::string ConvertToCamelCase( std::string_view str_view );

    // snake/camel 문자열을 PascalCase로 변환
    std::string ConvertToPascalCase( std::string_view str_view );

    // CamelCase/PascalCase 문자열을 snake_case로 변환
    std::string ConvertToSnakeCase( std::string_view str_view );
}

#endif // STRING_UTILS_HPP
