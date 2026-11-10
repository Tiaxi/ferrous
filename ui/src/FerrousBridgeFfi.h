// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
struct FerrousFfiBridge;
struct FerrousFfiRenameGate;

std::uint64_t ferrous_ffi_spectrogram_ring_capacity(
    std::uint32_t width, std::uint32_t sampleRate, std::uint32_t hop,
    std::uint32_t bins, std::uint32_t channels, double zoom, bool centered);
// The column hop of spectrogram data of audio at sampleRate at zoom; 0 on
// failure.
std::uint32_t ferrous_ffi_spectrogram_column_hop(std::uint32_t sampleRate, float zoom);
// The column hop of spectrogram data of audio at sampleRate at zoom 1,
// which zoom levels are relative to.
std::uint32_t ferrous_ffi_spectrogram_reference_hop(std::uint32_t sampleRate);
// The zoom to draw spectrogram data of audio at sampleRate whose columns
// are hop samples apart at, when zoom was asked for: a column a pixel
// unless the hop is the reference hop. The centered window and the ring
// capacity reckon with the same zoom.
double ferrous_ffi_spectrogram_render_zoom(
    std::uint32_t sampleRate, std::uint32_t hop, double zoom);
// The zoom to draw spectrogram data of audio at sampleRate whose columns
// are hop samples apart at, at renderZoom: renderZoom * hop / the reference
// hop, which the decoder reckons the centered view's window with too.
double ferrous_ffi_spectrogram_effective_zoom(
    std::uint32_t sampleRate, std::uint32_t hop, double renderZoom);
// The column after the last that a session at zoom, with fftSize-sample
// windows, decodes of a file of frames frames at sampleRate: where the
// centered view ends once the session has decoded the file's end, as the
// decoder places its views. 0 for a file shorter than half a window.
std::uint64_t ferrous_ffi_spectrogram_decoded_end_column(
    std::uint64_t frames, std::uint32_t fftSize, float zoom, std::uint32_t sampleRate);
// The zoom at which the columns a session decodes of a file of frames
// frames at sampleRate, with fftSize-sample windows, fill a view width
// logical pixels wide; 1 when they fill it at zoom 1 or a value is unknown
// (0).
float ferrous_ffi_spectrogram_fit_zoom(
    std::uint64_t frames, std::uint32_t fftSize, std::uint32_t sampleRate, std::uint32_t width);
// The zoom the spectrogram moves to when requested is asked of it at
// current; current if it keeps its zoom.
float ferrous_ffi_spectrogram_zoom_after_request(float current, float requested);

FerrousFfiBridge *ferrous_ffi_bridge_create();
// Blocks until the backend has saved its state, for a few seconds at most.
// Returns false if it had not finished by then, could not save it all, or
// left a file open that it could not close.
bool ferrous_ffi_bridge_destroy(FerrousFfiBridge *handle);
bool ferrous_ffi_bridge_send_binary(
    FerrousFfiBridge *handle,
    const std::uint8_t *cmd_ptr,
    std::size_t cmd_len);
int ferrous_ffi_bridge_wakeup_fd(FerrousFfiBridge *handle);
// Call after draining the queues: the wake fd is ready again at once if
// anything is still queued.
void ferrous_ffi_bridge_ack_wakeup(FerrousFfiBridge *handle);
// How many times analysis has turned the spectrogram off, and the decode
// generation the latest time made: a spectral chunk popped after it that is
// not newer is stale. False, writing nothing, for a null argument.
bool ferrous_ffi_bridge_spectrogram_stops(
    FerrousFfiBridge *handle,
    std::uint64_t *count_out,
    std::uint64_t *generation_out);
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
FerrousFfiRenameGate *ferrous_ffi_bridge_rename_gate(FerrousFfiBridge *handle);
void ferrous_ffi_rename_gate_free(FerrousFfiRenameGate *gate);
// Renames files on disk and waits for another rename: call it from a worker thread.
std::uint8_t *ferrous_ffi_rename_gate_rename(
    const FerrousFfiRenameGate *gate,
    const std::uint8_t *data,
    std::size_t size,
    std::size_t *len_out);
std::uint8_t *ferrous_ffi_tag_editor_load(
    const std::uint8_t *paths_ptr,
    std::size_t paths_len,
    std::size_t *len_out);
std::uint8_t *ferrous_ffi_tag_editor_save(
    const std::uint8_t *save_ptr,
    std::size_t save_len,
    std::size_t *len_out);
std::uint8_t *ferrous_ffi_tag_editor_rename(
    const std::uint8_t *data, std::size_t size, std::size_t *len_out);
std::uint8_t *ferrous_ffi_tag_editor_preview_rename(
    const std::uint8_t *data, std::size_t size, std::size_t *len_out);
void ferrous_ffi_tag_editor_free_buffer(std::uint8_t *ptr, std::size_t len);
std::uint8_t *ferrous_ffi_waveform_window_cancellable(
    const std::uint8_t *path_ptr, std::size_t path_len,
    double start_seconds, double end_seconds, std::uint32_t max_points,
    std::size_t *len_out, bool (*cancelled)(const void *), const void *context);
void ferrous_ffi_waveform_window_free(std::uint8_t *ptr, std::size_t len);
void ferrous_ffi_waveform_reader_retain(
    const std::uint8_t *path_ptr, std::size_t path_len, std::uint32_t kind);
void ferrous_ffi_waveform_reader_release(
    const std::uint8_t *path_ptr, std::size_t path_len, std::uint32_t kind);
void ferrous_ffi_waveform_release_unread();
double ferrous_ffi_fuzzy_match_score(
    const std::uint8_t *candidate_album_ptr, std::size_t candidate_album_len,
    const std::uint8_t *candidate_artist_ptr, std::size_t candidate_artist_len,
    const std::uint8_t *wanted_album_ptr, std::size_t wanted_album_len,
    const std::uint8_t *wanted_artist_ptr, std::size_t wanted_artist_len);
}
