#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
struct FerrousFfiBridge;

FerrousFfiBridge *ferrous_ffi_bridge_create();
void ferrous_ffi_bridge_destroy(FerrousFfiBridge *handle);
bool ferrous_ffi_bridge_send_binary(
    FerrousFfiBridge *handle,
    const std::uint8_t *cmd_ptr,
    std::size_t cmd_len);
bool ferrous_ffi_bridge_poll(FerrousFfiBridge *handle, std::uint32_t max_events);
int ferrous_ffi_bridge_wakeup_fd(FerrousFfiBridge *handle);
void ferrous_ffi_bridge_ack_wakeup(FerrousFfiBridge *handle);
std::uint8_t *ferrous_ffi_bridge_pop_binary_event(FerrousFfiBridge *handle, std::size_t *len_out);
void ferrous_ffi_bridge_free_binary_event(std::uint8_t *ptr, std::size_t len);
std::uint8_t *ferrous_ffi_bridge_pop_analysis_frame(FerrousFfiBridge *handle, std::size_t *len_out);
void ferrous_ffi_bridge_free_analysis_frame(std::uint8_t *ptr, std::size_t len);
std::uint8_t *ferrous_ffi_bridge_pop_precomputed_spectrogram(
    FerrousFfiBridge *handle, std::size_t *len_out);
void ferrous_ffi_bridge_free_precomputed_spectrogram(std::uint8_t *ptr, std::size_t len);
std::uint8_t *ferrous_ffi_bridge_pop_library_tree(
    FerrousFfiBridge *handle,
    std::size_t *len_out,
    std::uint32_t *version_out);
void ferrous_ffi_bridge_free_library_tree(std::uint8_t *ptr, std::size_t len);
std::uint8_t *ferrous_ffi_bridge_pop_search_results(
    FerrousFfiBridge *handle,
    std::size_t *len_out,
    std::uint32_t *seq_out);
void ferrous_ffi_bridge_free_search_results(std::uint8_t *ptr, std::size_t len);
bool ferrous_ffi_bridge_refresh_edited_paths(
    FerrousFfiBridge *handle,
    const std::uint8_t *paths_ptr,
    std::size_t paths_len);
std::uint8_t *ferrous_ffi_bridge_rename_edited_files(
    FerrousFfiBridge *handle,
    const std::uint8_t *rename_ptr,
    std::size_t rename_len,
    std::size_t *len_out);
std::uint8_t *ferrous_ffi_tag_editor_load(
    const std::uint8_t *paths_ptr,
    std::size_t paths_len,
    std::size_t *len_out);
std::uint8_t *ferrous_ffi_tag_editor_save(
    const std::uint8_t *save_ptr,
    std::size_t save_len,
    std::size_t *len_out);
void ferrous_ffi_tag_editor_free_buffer(std::uint8_t *ptr, std::size_t len);
double ferrous_ffi_fuzzy_match_score(
    const std::uint8_t *candidate_album_ptr, std::size_t candidate_album_len,
    const std::uint8_t *candidate_artist_ptr, std::size_t candidate_artist_len,
    const std::uint8_t *wanted_album_ptr, std::size_t wanted_album_len,
    const std::uint8_t *wanted_artist_ptr, std::size_t wanted_artist_len);
}
