#include "esm/query.hpp"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <limits>
#include <string>
#include <vector>
namespace esm {
namespace {
bool starts_with(std::wstring_view value,std::wstring_view prefix){return value.size()>=prefix.size()&&value.substr(0,prefix.size())==prefix;}
bool truthy(std::wstring_view value,bool& result){auto v=fold_case(value);if(v==L"1"||v==L"yes"||v==L"true"||v==L"on"){result=true;return true;}if(v==L"0"||v==L"no"||v==L"false"||v==L"off"){result=false;return true;}return false;}
struct Lexeme { enum class Kind { word,left,right,and_op,or_op,not_op } kind{Kind::word}; std::wstring text; };
std::vector<Lexeme> tokenize(std::wstring_view input, bool& ok,
                             std::wstring& error) {
    std::vector<Lexeme> out;
    std::wstring current;
    bool quote = false;
    const auto flush = [&] {
        if (current.empty()) return;
        const auto folded = fold_case(current);
        Lexeme::Kind kind = Lexeme::Kind::word;
        if (folded == L"and" || current == L"&&") {
            kind = Lexeme::Kind::and_op;
        } else if (folded == L"or" || current == L"||") {
            kind = Lexeme::Kind::or_op;
        } else if (folded == L"not" || current == L"!") {
            kind = Lexeme::Kind::not_op;
        }
        out.push_back({kind, std::move(current)});
        current.clear();
    };
    for (std::size_t index = 0; index < input.size(); ++index) {
        const wchar_t ch = input[index];
        if (ch == L'"') {
            quote = !quote;
            continue;
        }
        if (!quote && ch == L'|') {
            flush();
            if (index + 1 < input.size() && input[index + 1] == L'|') ++index;
            out.push_back({Lexeme::Kind::or_op, {}});
            continue;
        }
        const bool angle_left = !quote && ch == L'<' && current.empty();
        const bool angle_right = !quote && ch == L'>' &&
            current.find(L':') == std::wstring::npos;
        if (!quote && (std::iswspace(ch) || ch == L'(' || ch == L')' ||
                       angle_left || angle_right)) {
            flush();
            if (ch == L'(' || angle_left) {
                out.push_back({Lexeme::Kind::left, {}});
            } else if (ch == L')' || angle_right) {
                out.push_back({Lexeme::Kind::right, {}});
            }
            continue;
        }
        if (!quote && ch == L'!' && current.empty()) {
            if (index + 1 < input.size() && !std::iswspace(input[index + 1]) &&
                input[index + 1] != L'(' && input[index + 1] != L')' &&
                input[index + 1] != L'<') {
                current.push_back(ch);
                continue;
            }
            flush();
            out.push_back({Lexeme::Kind::not_op, {}});
            continue;
        }
        if (ch == L'\\' && index + 1 < input.size() &&
            input[index + 1] == L'"') {
            current.push_back(L'"');
            ++index;
            continue;
        }
        current.push_back(ch);
    }
    if (quote) {
        ok = false;
        error = L"unterminated quote";
        return {};
    }
    flush();
    return out;
}
bool parse_unsigned(std::wstring_view text,std::uint64_t& value){if(text.empty())return false;value=0;for(wchar_t ch:text){if(ch<L'0'||ch>L'9')return false;auto digit=(std::uint64_t)(ch-L'0');if(value>(std::numeric_limits<std::uint64_t>::max()-digit)/10)return false;value=value*10+digit;}return true;}
bool split_comparison(std::wstring_view value,NumericComparison& comparison,std::wstring_view& rest){comparison=NumericComparison::equal;if(starts_with(value,L">=")){comparison=NumericComparison::greater_equal;rest=value.substr(2);}else if(starts_with(value,L"<=")){comparison=NumericComparison::less_equal;rest=value.substr(2);}else if(starts_with(value,L">") ){comparison=NumericComparison::greater;rest=value.substr(1);}else if(starts_with(value,L"<")){comparison=NumericComparison::less;rest=value.substr(1);}else if(starts_with(value,L"=")){rest=value.substr(1);}else rest=value;return !rest.empty();}
bool parse_size(std::wstring_view input,std::uint64_t& bytes){auto folded=fold_case(input);std::size_t number_end=0;while(number_end<folded.size()&&folded[number_end]>=L'0'&&folded[number_end]<=L'9')++number_end;if(number_end==0)return false;std::uint64_t number{};if(!parse_unsigned(std::wstring_view(folded).substr(0,number_end),number))return false;auto unit=std::wstring_view(folded).substr(number_end);std::uint64_t multiplier=1;if(unit.empty()||unit==L"b")multiplier=1;else if(unit==L"k"||unit==L"kb"||unit==L"kib")multiplier=1024ULL;else if(unit==L"m"||unit==L"mb"||unit==L"mib")multiplier=1024ULL*1024;else if(unit==L"g"||unit==L"gb"||unit==L"gib")multiplier=1024ULL*1024*1024;else if(unit==L"t"||unit==L"tb"||unit==L"tib")multiplier=1024ULL*1024*1024*1024;else return false;if(number>std::numeric_limits<std::uint64_t>::max()/multiplier)return false;bytes=number*multiplier;return true;}
bool parse_date(std::wstring_view input,std::uint64_t& value){SYSTEMTIME st{};if(input.size()<10||input[4]!=L'-'||input[7]!=L'-')return false;std::uint64_t y{},m{},d{};if(!parse_unsigned(input.substr(0,4),y)||!parse_unsigned(input.substr(5,2),m)||!parse_unsigned(input.substr(8,2),d))return false;st.wYear=(WORD)y;st.wMonth=(WORD)m;st.wDay=(WORD)d;if(input.size()>10){if(input.size()<16||(input[10]!=L'T'&&input[10]!=L' ')||input[13]!=L':')return false;std::uint64_t hour{},minute{};if(!parse_unsigned(input.substr(11,2),hour)||!parse_unsigned(input.substr(14,2),minute))return false;st.wHour=(WORD)hour;st.wMinute=(WORD)minute;}FILETIME local{},utc{};if(!SystemTimeToFileTime(&st,&local))return false;if(!LocalFileTimeToFileTime(&local,&utc))utc=local;value=(std::uint64_t(utc.dwHighDateTime)<<32U)|utc.dwLowDateTime;return true;}
bool local_system_time_to_file_time(const SYSTEMTIME& local_time,
                                    std::uint64_t& value) {
    FILETIME local{};
    FILETIME utc{};
    if (!SystemTimeToFileTime(&local_time, &local)) return false;
    if (!LocalFileTimeToFileTime(&local, &utc)) utc = local;
    value = (std::uint64_t(utc.dwHighDateTime) << 32U) | utc.dwLowDateTime;
    return true;
}

void clear_local_time(SYSTEMTIME& value) {
    value.wHour = 0;
    value.wMinute = 0;
    value.wSecond = 0;
    value.wMilliseconds = 0;
}

bool add_local_days(SYSTEMTIME& value, int days) {
    FILETIME file_time{};
    if (!SystemTimeToFileTime(&value, &file_time)) return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    constexpr std::uint64_t ticks_per_day = 864000000000ULL;
    const auto magnitude_days = days < 0
        ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(days))
        : static_cast<std::uint64_t>(days);
    if (magnitude_days > std::numeric_limits<std::uint64_t>::max() /
                             ticks_per_day) return false;
    const auto delta = magnitude_days * ticks_per_day;
    if (days < 0) {
        if (delta > ticks.QuadPart) return false;
        ticks.QuadPart -= delta;
    } else {
        if (delta > std::numeric_limits<std::uint64_t>::max() -
                        ticks.QuadPart) return false;
        ticks.QuadPart += delta;
    }
    file_time.dwLowDateTime = ticks.LowPart;
    file_time.dwHighDateTime = ticks.HighPart;
    return FileTimeToSystemTime(&file_time, &value) != FALSE;
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    static constexpr int lengths[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) return 29;
    return lengths[month - 1];
}

bool shift_local_months(SYSTEMTIME& value, std::int64_t months,
                        bool preserve_day) {
    const auto total_month = static_cast<std::int64_t>(value.wYear) * 12 +
        static_cast<std::int64_t>(value.wMonth) - 1 + months;
    if (total_month < 12 || total_month > 119999) return false;
    const int year = static_cast<int>(total_month / 12);
    const int month = static_cast<int>(total_month % 12) + 1;
    const int day = preserve_day
        ? std::min<int>(value.wDay, days_in_month(year, month)) : 1;
    value.wYear = static_cast<WORD>(year);
    value.wMonth = static_cast<WORD>(month);
    value.wDay = static_cast<WORD>(day);
    return true;
}

bool shift_local_years(SYSTEMTIME& value, std::int64_t years) {
    const auto year = static_cast<std::int64_t>(value.wYear) + years;
    if (year < 1 || year > 9999) return false;
    value.wYear = static_cast<WORD>(year);
    value.wDay = static_cast<WORD>(std::min<int>(
        value.wDay, days_in_month(value.wYear, value.wMonth)));
    return true;
}

int first_day_of_week() {
    wchar_t value[4]{};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK,
                        value, static_cast<int>(std::size(value))) > 0 &&
        value[0] >= L'0' && value[0] <= L'6') {
        // LOCALE_IFIRSTDAYOFWEEK uses Monday=0; SYSTEMTIME uses Sunday=0.
        return ((value[0] - L'0') + 1) % 7;
    }
    return 1;
}

bool start_of_local_week(const SYSTEMTIME& now, SYSTEMTIME& start) {
    start = now;
    clear_local_time(start);
    const int days_since_start =
        (static_cast<int>(now.wDayOfWeek) - first_day_of_week() + 7) % 7;
    return add_local_days(start, -days_since_start);
}

void set_lower_bound(QueryTerm& term, std::uint64_t value) {
    term.has_lower_bound = true;
    term.lower_bound = value;
    term.lower_inclusive = true;
}

void set_upper_bound(QueryTerm& term, std::uint64_t value) {
    term.has_upper_bound = true;
    term.upper_bound = value;
    term.upper_inclusive = false;
}

bool set_date_interval(QueryTerm& term, const SYSTEMTIME& start,
                       const SYSTEMTIME& end) {
    std::uint64_t lower{};
    std::uint64_t upper{};
    if (!local_system_time_to_file_time(start, lower) ||
        !local_system_time_to_file_time(end, upper) || lower >= upper) {
        return false;
    }
    set_lower_bound(term, lower);
    set_upper_bound(term, upper);
    return true;
}

bool set_rolling_date_interval(QueryTerm& term, const SYSTEMTIME& now,
                               const SYSTEMTIME& boundary, bool future) {
    std::uint64_t now_value{};
    std::uint64_t boundary_value{};
    if (!local_system_time_to_file_time(now, now_value) ||
        !local_system_time_to_file_time(boundary, boundary_value)) {
        return false;
    }
    if (future) {
        if (boundary_value <= now_value) return false;
        set_lower_bound(term, now_value);
        set_upper_bound(term, boundary_value);
    } else {
        if (boundary_value >= now_value) return false;
        // Everything 1.4 intentionally leaves the upper side open for
        // last/past/prev rolling constants, so future-dated records match.
        set_lower_bound(term, boundary_value);
    }
    return true;
}

int month_constant(std::wstring_view value) {
    static constexpr std::wstring_view names[] = {
        L"january", L"february", L"march", L"april", L"may", L"june",
        L"july", L"august", L"september", L"october", L"november",
        L"december"};
    static constexpr std::wstring_view short_names[] = {
        L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
        L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    for (int index = 0; index < 12; ++index) {
        if (value == names[index] || value == short_names[index]) return index + 1;
    }
    return 0;
}

int weekday_constant(std::wstring_view value) {
    static constexpr std::wstring_view names[] = {
        L"sunday", L"monday", L"tuesday", L"wednesday", L"thursday",
        L"friday", L"saturday"};
    static constexpr std::wstring_view short_names[] = {
        L"sun", L"mon", L"tue", L"wed", L"thu", L"fri", L"sat"};
    for (int index = 0; index < 7; ++index) {
        if (value == names[index] || value == short_names[index]) return index;
    }
    return -1;
}

bool parse_rolling_date(std::wstring_view folded, const SYSTEMTIME& now,
                        QueryTerm& term) {
    bool future = false;
    std::wstring_view rest;
    if (starts_with(folded, L"coming")) {
        future = true;
        rest = folded.substr(6);
    } else if (starts_with(folded, L"next")) {
        future = true;
        rest = folded.substr(4);
    } else if (starts_with(folded, L"last")) {
        rest = folded.substr(4);
    } else if (starts_with(folded, L"past")) {
        rest = folded.substr(4);
    } else if (starts_with(folded, L"prev")) {
        rest = folded.substr(4);
    } else {
        return false;
    }

    std::size_t number_end = 0;
    while (number_end < rest.size() && rest[number_end] >= L'0' &&
           rest[number_end] <= L'9') {
        ++number_end;
    }
    if (number_end == 0 || number_end == rest.size()) return false;

    std::uint64_t amount{};
    if (!parse_unsigned(rest.substr(0, number_end), amount) || amount == 0 ||
        amount > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto unit = rest.substr(number_end);
    SYSTEMTIME boundary = now;
    const auto signed_amount = static_cast<std::int64_t>(amount) *
        (future ? 1 : -1);
    if (unit == L"years") {
        if (!shift_local_years(boundary, signed_amount)) return false;
    } else if (unit == L"months") {
        if (!shift_local_months(boundary, signed_amount, true)) return false;
    } else if (unit == L"weeks") {
        if (amount > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max() / 7)) return false;
        if (!add_local_days(boundary,
                static_cast<int>(signed_amount * 7))) return false;
    } else if (unit == L"days") {
        if (!add_local_days(boundary, static_cast<int>(signed_amount))) return false;
    } else {
        std::uint64_t seconds_per_unit{};
        if (unit == L"hours") seconds_per_unit = 60 * 60;
        else if (unit == L"minutes" || unit == L"mins") seconds_per_unit = 60;
        else if (unit == L"seconds" || unit == L"secs") seconds_per_unit = 1;
        else return false;

        if (amount > std::numeric_limits<std::uint64_t>::max() /
                         seconds_per_unit) return false;
        const auto seconds = amount * seconds_per_unit;
        constexpr std::uint64_t ticks_per_second = 10000000ULL;
        if (seconds > std::numeric_limits<std::uint64_t>::max() /
                          ticks_per_second) return false;
        std::uint64_t now_value{};
        if (!local_system_time_to_file_time(now, now_value)) return false;
        const auto delta = seconds * ticks_per_second;
        if ((!future && delta > now_value) ||
            (future && delta > std::numeric_limits<std::uint64_t>::max() -
                                    now_value)) return false;
        if (future) {
            set_lower_bound(term, now_value);
            set_upper_bound(term, now_value + delta);
        } else {
            set_lower_bound(term, now_value - delta);
        }
        return true;
    }
    return set_rolling_date_interval(term, now, boundary, future);
}

bool parse_relative_date(std::wstring_view input, QueryTerm& term) {
    const auto folded = fold_case(input);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    SYSTEMTIME start = now;
    clear_local_time(start);
    SYSTEMTIME end = start;

    if (folded == L"today") {
        if (!add_local_days(end, 1)) return false;
    } else if (folded == L"yesterday") {
        if (!add_local_days(start, -1)) return false;
    } else if (folded == L"thisweek" || folded == L"currentweek") {
        if (!start_of_local_week(now, start)) return false;
        end = now;
        clear_local_time(end);
        if (!add_local_days(end, 1)) return false;
    } else if (folded == L"lastweek" || folded == L"prevweek") {
        if (!start_of_local_week(now, end)) return false;
        start = end;
        if (!add_local_days(start, -7)) return false;
    } else if (folded == L"pastweek") {
        SYSTEMTIME boundary = now;
        if (!add_local_days(boundary, -7)) return false;
        return set_rolling_date_interval(term, now, boundary, false);
    } else if (folded == L"comingweek" || folded == L"nextweek") {
        if (!start_of_local_week(now, start) || !add_local_days(start, 7)) {
            return false;
        }
        end = start;
        if (!add_local_days(end, 7)) return false;
    } else if (folded == L"thismonth" || folded == L"currentmonth") {
        start.wDay = 1;
        end = now;
        clear_local_time(end);
        if (!add_local_days(end, 1)) return false;
    } else if (folded == L"lastmonth" || folded == L"prevmonth") {
        start.wDay = 1;
        end = start;
        if (!shift_local_months(start, -1, false)) return false;
    } else if (folded == L"pastmonth") {
        SYSTEMTIME boundary = now;
        if (!shift_local_months(boundary, -1, true)) return false;
        return set_rolling_date_interval(term, now, boundary, false);
    } else if (folded == L"comingmonth" || folded == L"nextmonth") {
        start.wDay = 1;
        if (!shift_local_months(start, 1, false)) return false;
        end = start;
        if (!shift_local_months(end, 1, false)) return false;
    } else if (folded == L"thisyear" || folded == L"currentyear") {
        start.wMonth = 1;
        start.wDay = 1;
        end = now;
        clear_local_time(end);
        if (!add_local_days(end, 1)) return false;
    } else if (folded == L"lastyear" || folded == L"prevyear") {
        start.wMonth = 1;
        start.wDay = 1;
        end = start;
        if (!shift_local_years(start, -1)) return false;
    } else if (folded == L"pastyear") {
        SYSTEMTIME boundary = now;
        if (!shift_local_years(boundary, -1)) return false;
        return set_rolling_date_interval(term, now, boundary, false);
    } else if (folded == L"comingyear" || folded == L"nextyear") {
        start.wMonth = 1;
        start.wDay = 1;
        if (!shift_local_years(start, 1)) return false;
        end = start;
        if (!shift_local_years(end, 1)) return false;
    } else if (const int month = month_constant(folded); month != 0) {
        start.wMonth = static_cast<WORD>(month);
        start.wDay = 1;
        end = start;
        if (!shift_local_months(end, 1, false)) return false;
    } else if (const int weekday = weekday_constant(folded); weekday >= 0) {
        if (!start_of_local_week(now, start)) return false;
        const int offset = (weekday - first_day_of_week() + 7) % 7;
        if (!add_local_days(start, offset)) return false;
        end = start;
        if (!add_local_days(end, 1)) return false;
    } else if (parse_rolling_date(folded, now, term)) {
        return true;
    } else {
        return false;
    }
    return set_date_interval(term, start, end);
}
bool parse_date_term(std::wstring_view input, QueryTerm& term) {
    if (parse_relative_date(input, term)) return true;
    if (input.size() == 10 && input[4] == L'-' && input[7] == L'-') {
        SYSTEMTIME start{};
        std::uint64_t year{};
        std::uint64_t month{};
        std::uint64_t day{};
        if (!parse_unsigned(input.substr(0, 4), year) ||
            !parse_unsigned(input.substr(5, 2), month) ||
            !parse_unsigned(input.substr(8, 2), day)) {
            return false;
        }
        start.wYear = static_cast<WORD>(year);
        start.wMonth = static_cast<WORD>(month);
        start.wDay = static_cast<WORD>(day);
        SYSTEMTIME end = start;
        if (!add_local_days(end, 1)) return false;
        return set_date_interval(term, start, end);
    }
    std::wstring_view rest;
    return split_comparison(input, term.comparison, rest) &&
        parse_date(rest, term.numeric_value);
}

std::uint32_t attribute_flag(std::wstring_view name){auto v=fold_case(name);if(v==L"readonly"||v==L"ro")return FILE_ATTRIBUTE_READONLY;if(v==L"hidden"||v==L"h")return FILE_ATTRIBUTE_HIDDEN;if(v==L"system"||v==L"s")return FILE_ATTRIBUTE_SYSTEM;if(v==L"archive"||v==L"a")return FILE_ATTRIBUTE_ARCHIVE;if(v==L"compressed"||v==L"c")return FILE_ATTRIBUTE_COMPRESSED;if(v==L"encrypted"||v==L"e")return FILE_ATTRIBUTE_ENCRYPTED;if(v==L"offline"||v==L"o")return FILE_ATTRIBUTE_OFFLINE;if(v==L"reparse"||v==L"junction"||v==L"symlink")return FILE_ATTRIBUTE_REPARSE_POINT;return 0;}
int precedence(Lexeme::Kind kind){if(kind==Lexeme::Kind::not_op)return 3;if(kind==Lexeme::Kind::and_op)return 2;if(kind==Lexeme::Kind::or_op)return 1;return 0;}
QueryOpcode opcode(Lexeme::Kind kind){if(kind==Lexeme::Kind::not_op)return QueryOpcode::logical_not;if(kind==Lexeme::Kind::or_op)return QueryOpcode::logical_or;return QueryOpcode::logical_and;}
struct ParsedWord {
    bool directive{};
    QueryTerm term;
};

template <typename Parser>
bool parse_range(std::wstring_view input, QueryTerm& term, Parser parser) {
    auto delimiter = input.find(L"..");
    std::size_t delimiter_length = 2;
    if (delimiter == std::wstring_view::npos) {
        delimiter = input.find(L'-');
        delimiter_length = 1;
    }
    if (delimiter == std::wstring_view::npos || delimiter == 0 ||
        delimiter + delimiter_length >= input.size()) {
        return false;
    }
    std::uint64_t lower{};
    std::uint64_t upper{};
    if (!parser(input.substr(0, delimiter), lower) ||
        !parser(input.substr(delimiter + delimiter_length), upper) ||
        lower > upper) {
        return false;
    }
    term.has_lower_bound = true;
    term.lower_bound = lower;
    term.lower_inclusive = true;
    term.has_upper_bound = true;
    term.upper_bound = upper;
    term.upper_inclusive = true;
    return true;
}

bool parse_numeric_term(std::wstring_view input, QueryTerm& term) {
    if (parse_range(input, term, parse_unsigned)) return true;
    std::wstring_view rest;
    return split_comparison(input, term.comparison, rest) &&
        parse_unsigned(rest, term.numeric_value);
}

bool parse_size_term(std::wstring_view input, QueryTerm& term) {
    const auto folded = fold_case(input);
    const auto set_range = [&](std::uint64_t lower, bool lower_inclusive,
                               std::uint64_t upper, bool upper_inclusive) {
        term.has_lower_bound = true;
        term.lower_bound = lower;
        term.lower_inclusive = lower_inclusive;
        term.has_upper_bound = true;
        term.upper_bound = upper;
        term.upper_inclusive = upper_inclusive;
    };
    if (folded == L"empty") {
        term.comparison = NumericComparison::equal;
        term.numeric_value = 0;
        return true;
    }
    if (folded == L"tiny") {
        set_range(0, false, 10ULL * 1024, true);
        return true;
    }
    if (folded == L"small") {
        set_range(10ULL * 1024, false, 100ULL * 1024, true);
        return true;
    }
    if (folded == L"medium") {
        set_range(100ULL * 1024, false, 1024ULL * 1024, true);
        return true;
    }
    if (folded == L"large") {
        set_range(1024ULL * 1024, false, 16ULL * 1024 * 1024, true);
        return true;
    }
    if (folded == L"huge") {
        set_range(16ULL * 1024 * 1024, false, 128ULL * 1024 * 1024, true);
        return true;
    }
    if (folded == L"gigantic") {
        term.has_lower_bound = true;
        term.lower_bound = 128ULL * 1024 * 1024;
        term.lower_inclusive = false;
        return true;
    }
    if (parse_range(input, term, parse_size)) return true;
    std::wstring_view rest;
    return split_comparison(input, term.comparison, rest) &&
        parse_size(rest, term.numeric_value);
}

ParsedWord parse_word(std::wstring token, ParsedQuery& query) {
    ParsedWord result;
    if (!token.empty() && token.front() == L'!') {
        result.term.excluded = true;
        token.erase(token.begin());
    }
    const auto folded = fold_case(token);
    bool flag{};

    if (folded == L"file:" || folded == L"files:") {
        query.directories_only = false;
        result.directive = true;
        return result;
    }
    if (folded == L"folder:" || folded == L"dir:") {
        query.directories_only = true;
        result.directive = true;
        return result;
    }
    if (starts_with(folded, L"case:")) {
        if (!truthy(std::wstring_view(token).substr(5), flag)) {
            query.valid = false;
            query.error = L"invalid case option";
        } else {
            query.case_sensitive = flag;
        }
        result.directive = true;
        return result;
    }
    if (starts_with(folded, L"wholeword:")) {
        if (!truthy(std::wstring_view(token).substr(10), flag)) {
            query.valid = false;
            query.error = L"invalid whole-word option";
        } else {
            query.whole_word = flag;
        }
        result.directive = true;
        return result;
    }
    if (starts_with(folded, L"count:")) {
        std::uint64_t count{};
        if (!parse_unsigned(std::wstring_view(token).substr(6), count) ||
            count > std::numeric_limits<std::size_t>::max()) {
            query.valid = false;
            query.error = L"invalid result count";
        } else {
            query.max_results = static_cast<std::size_t>(count);
        }
        result.directive = true;
        return result;
    }
    if (folded == L"root:") {
        result.term.target = MatchTarget::path_depth;
        result.term.comparison = NumericComparison::equal;
        result.term.numeric_value = 0;
        return result;
    }
    if (folded == L"empty:") {
        result.term.target = MatchTarget::direct_child_count;
        result.term.comparison = NumericComparison::equal;
        result.term.numeric_value = 0;
        return result;
    }

    if (starts_with(folded, L"dupe:")) {
        const auto mode = folded.substr(5);
        if (mode == L"name") query.duplicate_mode = DuplicateMode::name;
        else if (mode == L"size") query.duplicate_mode = DuplicateMode::size;
        else if (mode == L"name-size" || mode == L"name_size") {
            query.duplicate_mode = DuplicateMode::name_and_size;
        } else {
            query.valid = false;
            query.error = L"invalid duplicate mode";
        }
        result.directive = true;
        return result;
    }

    const auto scoped = [&](std::wstring_view prefix, MatchTarget target) {
        if (!starts_with(folded, prefix)) return false;
        result.term.target = target;
        result.term.value = token.substr(prefix.size());
        return true;
    };

    if (scoped(L"name:", MatchTarget::name) ||
        scoped(L"path:", MatchTarget::path) ||
        scoped(L"ext:", MatchTarget::extension) ||
        scoped(L"startwith:", MatchTarget::name_prefix) ||
        scoped(L"endwith:", MatchTarget::name_suffix) ||
        scoped(L"parent:", MatchTarget::parent_path) ||
        scoped(L"infolder:", MatchTarget::parent_path) ||
        scoped(L"nosubfolders:", MatchTarget::parent_path) ||
        scoped(L"child:", MatchTarget::child_name)) {
    } else if (scoped(L"regex:", MatchTarget::any)) {
        result.term.regex = true;
    } else if (scoped(L"name-regex:", MatchTarget::name)) {
        result.term.regex = true;
    } else if (scoped(L"path-regex:", MatchTarget::path)) {
        result.term.regex = true;
    } else if (starts_with(folded, L"len:")) {
        result.term.target = MatchTarget::filename_length;
        if (!parse_numeric_term(std::wstring_view(token).substr(4), result.term)) {
            query.valid = false;
            query.error = L"invalid filename length expression";
        }
        return result;
    } else if (starts_with(folded, L"depth:") ||
               starts_with(folded, L"parents:")) {
        const std::size_t offset = starts_with(folded, L"depth:") ? 6 : 8;
        result.term.target = MatchTarget::path_depth;
        if (!parse_numeric_term(std::wstring_view(token).substr(offset),
                                result.term)) {
            query.valid = false;
            query.error = L"invalid path depth expression";
        }
        return result;
    } else if (starts_with(folded, L"childcount:") ||
               starts_with(folded, L"childfilecount:") ||
               starts_with(folded, L"childfoldercount:")) {
        std::size_t offset = 11;
        result.term.target = MatchTarget::direct_child_count;
        if (starts_with(folded, L"childfilecount:")) {
            offset = 15;
            result.term.target = MatchTarget::child_file_count;
        } else if (starts_with(folded, L"childfoldercount:")) {
            offset = 17;
            result.term.target = MatchTarget::child_folder_count;
        }
        if (!parse_numeric_term(std::wstring_view(token).substr(offset),
                                result.term)) {
            query.valid = false;
            query.error = L"invalid child count expression";
        }
        return result;
    } else if (starts_with(folded, L"size:")) {
        result.term.target = MatchTarget::size;
        if (!parse_size_term(std::wstring_view(token).substr(5), result.term)) {
            query.valid = false;
            query.error = L"invalid size expression";
        }
        return result;
    } else if (starts_with(folded, L"date:") ||
               starts_with(folded, L"dm:") ||
               starts_with(folded, L"modified:") ||
               starts_with(folded, L"datemodified:")) {
        const std::size_t offset = starts_with(folded, L"date:") ? 5 :
            (starts_with(folded, L"dm:") ? 3 :
             (starts_with(folded, L"modified:") ? 9 : 13));
        result.term.target = MatchTarget::last_write_time;
        if (!parse_date_term(std::wstring_view(token).substr(offset),
                             result.term)) {
            query.valid = false;
            query.error = L"invalid date expression";
        }
        return result;
    } else if (starts_with(folded, L"attr:") ||
               starts_with(folded, L"attrib:")) {
        const std::size_t offset = starts_with(folded, L"attr:") ? 5 : 7;
        auto value = std::wstring_view(token).substr(offset);
        if (!value.empty() && value.front() == L'!') {
            result.term.attribute_absent = true;
            value.remove_prefix(1);
        }
        result.term.target = MatchTarget::attributes;
        std::size_t part_start = 0;
        while (part_start <= value.size()) {
            const auto comma = value.find(L',', part_start);
            const auto part = value.substr(
                part_start, comma == std::wstring_view::npos
                    ? value.size() - part_start : comma - part_start);
            const auto bit = attribute_flag(part);
            if (!bit) {
                query.valid = false;
                query.error = L"invalid attribute name";
                break;
            }
            result.term.attribute_mask |= bit;
            if (comma == std::wstring_view::npos) break;
            part_start = comma + 1;
        }
        return result;
    } else {
        result.term.target = MatchTarget::any;
        result.term.value = std::move(token);
    }

    if (result.term.target == MatchTarget::extension) {
        std::size_t part_start = 0;
        while (part_start <= result.term.value.size()) {
            const auto separator = result.term.value.find(L';', part_start);
            auto part = result.term.value.substr(
                part_start, separator == std::wstring::npos
                    ? std::wstring::npos : separator - part_start);
            if (!part.empty() && part.front() == L'.') part.erase(part.begin());
            if (part.empty()) {
                query.valid = false;
                query.error = L"empty extension list item";
                return result;
            }
            result.term.alternatives.push_back(std::move(part));
            if (separator == std::wstring::npos) break;
            part_start = separator + 1;
        }
        result.term.value = result.term.alternatives.front();
        result.term.wildcard = std::any_of(
            result.term.alternatives.begin(), result.term.alternatives.end(),
            [](const std::wstring& value) {
                return value.find_first_of(L"*?") != std::wstring::npos;
            });
        return result;
    }
    if (result.term.value.empty()) {
        query.valid = false;
        query.error = L"empty scoped query term";
    }
    result.term.wildcard =
        result.term.value.find_first_of(L"*?") != std::wstring::npos;
    return result;
}
}
std::wstring fold_case(std::wstring_view text){std::wstring result;result.reserve(text.size());for(wchar_t ch:text)result.push_back((wchar_t)std::towlower(ch));return result;}
ParsedQuery parse_query(std::wstring_view input){ParsedQuery query;bool lex_ok=true;auto tokens=tokenize(input,lex_ok,query.error);if(!lex_ok){query.valid=false;return query;}std::vector<Lexeme::Kind> operators;bool have_operand=false;bool expect_operand=true;
    auto emit_operator=[&](Lexeme::Kind kind){query.program.push_back({opcode(kind),0});};
    auto push_operator=[&](Lexeme::Kind kind){while(!operators.empty()&&operators.back()!=Lexeme::Kind::left&&precedence(operators.back())>=precedence(kind)){emit_operator(operators.back());operators.pop_back();}operators.push_back(kind);};
    for(auto& token:tokens){if(token.kind==Lexeme::Kind::word){auto parsed=parse_word(std::move(token.text),query);if(!query.valid)return query;if(parsed.directive)continue;if(!expect_operand)push_operator(Lexeme::Kind::and_op);query.terms.push_back(std::move(parsed.term));query.program.push_back({QueryOpcode::term,query.terms.size()-1});if(query.terms.back().excluded)query.program.push_back({QueryOpcode::logical_not,0});have_operand=true;expect_operand=false;continue;}
        if(token.kind==Lexeme::Kind::left){if(!expect_operand)push_operator(Lexeme::Kind::and_op);operators.push_back(token.kind);expect_operand=true;continue;}
        if(token.kind==Lexeme::Kind::right){if(expect_operand){query.valid=false;query.error=L"unexpected closing parenthesis";return query;}bool found=false;while(!operators.empty()){auto top=operators.back();operators.pop_back();if(top==Lexeme::Kind::left){found=true;break;}emit_operator(top);}if(!found){query.valid=false;query.error=L"unmatched closing parenthesis";return query;}expect_operand=false;continue;}
        if(token.kind==Lexeme::Kind::not_op){if(!expect_operand)push_operator(Lexeme::Kind::and_op);operators.push_back(token.kind);expect_operand=true;continue;}
        if(expect_operand){query.valid=false;query.error=L"binary operator without left operand";return query;}push_operator(token.kind);expect_operand=true;
    }
    if(have_operand&&expect_operand){query.valid=false;query.error=L"query ends with an operator";return query;}while(!operators.empty()){auto top=operators.back();operators.pop_back();if(top==Lexeme::Kind::left){query.valid=false;query.error=L"unmatched opening parenthesis";return query;}emit_operator(top);}return query;
}
bool wildcard_match(std::wstring_view pattern,std::wstring_view value){std::size_t p=0,v=0,star=std::wstring_view::npos,checkpoint=0;while(v<value.size()){if(p<pattern.size()&&(pattern[p]==L'?'||pattern[p]==value[v])){++p;++v;}else if(p<pattern.size()&&pattern[p]==L'*'){star=p++;checkpoint=v;}else if(star!=std::wstring_view::npos){p=star+1;v=++checkpoint;}else return false;}while(p<pattern.size()&&pattern[p]==L'*')++p;return p==pattern.size();}
int natural_compare(std::wstring_view left,std::wstring_view right,bool sensitive){std::size_t a=0,b=0;while(a<left.size()&&b<right.size()){if(std::iswdigit(left[a])&&std::iswdigit(right[b])){std::size_t ae=a,be=b;while(ae<left.size()&&left[ae]==L'0')++ae;while(be<right.size()&&right[be]==L'0')++be;std::size_t ax=ae,bx=be;while(ax<left.size()&&std::iswdigit(left[ax]))++ax;while(bx<right.size()&&std::iswdigit(right[bx]))++bx;auto al=ax-ae,bl=bx-be;if(al!=bl)return al<bl?-1:1;for(std::size_t i=0;i<al;++i)if(left[ae+i]!=right[be+i])return left[ae+i]<right[be+i]?-1:1;std::size_t ar=ax-a,br=bx-b;if(ar!=br)return ar<br?-1:1;a=ax;b=bx;continue;}wchar_t ac=sensitive?left[a]:(wchar_t)std::towlower(left[a]);wchar_t bc=sensitive?right[b]:(wchar_t)std::towlower(right[b]);if(ac!=bc)return ac<bc?-1:1;++a;++b;}if(a==left.size()&&b==right.size())return 0;return a==left.size()?-1:1;}
}
