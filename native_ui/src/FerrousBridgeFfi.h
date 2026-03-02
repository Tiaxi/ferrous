#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
struct FerrousFfiBridge;

FerrousFfiBridge *ferrous_ffi_bridge_create();
void ferrous_ffi_bridge_destroy(FerrousFfiBridge *handle);
bool ferrous_ffi_bridge_send_json(FerrousFfiBridge *handle, const char *cmd_json);
bool ferrous_ffi_bridge_poll(FerrousFfiBridge *handle, std::uint32_t max_events);
char *ferrous_ffi_bridge_pop_json_event(FerrousFfiBridge *handle);
void ferrous_ffi_bridge_free_json_event(char *ptr);
std::uint8_t *ferrous_ffi_bridge_pop_analysis_frame(FerrousFfiBridge *handle, std::size_t *len_out);
void ferrous_ffi_bridge_free_analysis_frame(std::uint8_t *ptr, std::size_t len);
}
