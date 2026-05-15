#include "string_utils.h"

namespace MGEN
{
    std::string ToUpperCase( std::string_view str_view )
    {
        std::string str { str_view };
        std::transform( str.begin(), str.end(), str.begin(),
                    [] (unsigned char c)
                    {
                        return std::toupper(c);
                    }
                );
        return str;
    }

    std::string ConvertToCamelCase( std::string_view str_view )
    {
        if( str_view.empty() )
            return "";

        std::string result;
        bool next_upper = false;

        for( size_t i = 0; i < str_view.size(); ++i )
        {
            unsigned char c = str_view[i];
            if( c == '_' )
            {
                next_upper = true;
            }
            else
            {
                if( next_upper )
                {
                    result += static_cast<char>(std::toupper(c));
                    next_upper = false;
                }
                // 첫 글자는 소문자로
                else if( i == 0 )
                {
                    result += static_cast<char>(std::tolower(c));
                }
                // 기존 케이스 유지
                else
                {
                    result += static_cast<char>(c);
                }
            }
        }
        return result;
    }

    std::string ConvertToPascalCase( std::string_view str_view )
    {
        if( str_view.empty() ){
            return "";
        }

        std::string result;
        result.reserve(str_view.size());

        bool next_upper = true; // 첫 글자를 대문자로 만들기 위해 true로 시작

        for( char c : str_view )
        {
            unsigned char uc = static_cast<unsigned char>(c);

            if( uc == '_' )
            {
                next_upper = true;
            }
            else
            {
                if( next_upper )
                {
                    result += static_cast<char>(std::toupper(uc));
                    next_upper = false;
                }
                else
                {
                    result += static_cast<char>(std::tolower(uc));
                }
            }
        }
        return result;
    }

    std::string ConvertToSnakeCase( std::string_view str_view )
    {
        std::string result;
        for( size_t i = 0; i < str_view.size(); ++i )
        {
            unsigned char c = str_view[i];
            if( std::isupper(c) && i > 0 )
            {
                result += '_';
            }
            result += static_cast<char>(std::tolower(c));
        }
        return result;
    }
}
