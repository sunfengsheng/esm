#pragma once

#include <cstddef>
#include <cstdint>

namespace esm {
constexpr std::uint64_t interactive_search_initial_debounce_ms = 15;
constexpr std::uint64_t interactive_search_burst_debounce_ms = 60;
constexpr std::uint64_t interactive_search_burst_window_ms = 150;

[[nodiscard]] constexpr std::uint64_t interactive_search_debounce_ms(
    std::uint64_t now, std::uint64_t last_input_tick) noexcept {
    return last_input_tick != 0 &&
                   now - last_input_tick <= interactive_search_burst_window_ms
               ? interactive_search_burst_debounce_ms
               : interactive_search_initial_debounce_ms;
}
[[nodiscard]] constexpr bool interactive_search_response_is_complete(
    bool requested_final_results, std::size_t result_count,
    std::size_t requested_limit) noexcept {
    return requested_final_results || result_count < requested_limit;
}
} // namespace esm
