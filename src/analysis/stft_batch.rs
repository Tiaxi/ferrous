// SPDX-License-Identifier: GPL-3.0-or-later

//! Spectrogram columns of a decoded batch, computed one channel at a time,
//! with the channels of multichannel streams split over helper threads.
//!
//! Every channel has its own STFT and peak-hold resampler, and they all get
//! the same audio frames, so each channel's columns depend on that channel
//! alone. A batch takes the same rows from every channel and runs groups of
//! channels on different threads; the decode thread then interleaves the
//! columns in channel order, as computing them a column at a time would.
//! The output is the same whichever thread computed a channel.
//!
//! The helper threads belong to the decode thread that started them and
//! live as long as it does. They wait on a channel between batches and the
//! decode thread waits for every batch it hands out, so a batch ends before
//! the session can be cancelled, as one packet's rows always did.

use std::cell::RefCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::thread::JoinHandle;

use crossbeam_channel::{bounded, Receiver, Sender};

use super::fft::{PeakHoldResampler, StftComputer};
use super::quantize::append_quantized_row;

/// The most threads one batch runs on, the decode thread included: enough
/// to cut a 5.1 or 7.1 stream's time to fill its lookahead by more than
/// half, without taking more of a smaller machine.
const MAX_GROUPS: usize = 3;

/// The fewest channels a thread gets: below this, waking a helper costs
/// more than it saves, so stereo streams stay on the decode thread.
const MIN_CHANNELS_PER_GROUP: usize = 2;

/// The most columns of one batch, which bounds the batch's buffers when a
/// decode thread drains the rows it kept when it parked.
pub(super) const MAX_BATCH_COLUMNS: usize = 128;

/// The rows per channel a decode thread queues before it hands a batch to
/// helpers. Waking a helper costs about 10 µs; at FFT 2048, 64 rows of two
/// channels take some 0.7 ms. A packet has 4 to 32 rows, and handing out
/// each made the helpers cost a tenth of the work. Larger batches measured
/// no cheaper, and delay a session's first columns.
pub(super) const PARALLEL_BATCH_ROWS: usize = 64;

/// What one batch may compute.
#[derive(Debug, Clone, Copy)]
struct Limits {
    /// Rows every channel has; a channel takes no more.
    rows: usize,
    /// Columns each channel stops at.
    columns: usize,
    bins: usize,
    fft_size: usize,
}

/// Take up to `limits.rows` rows from each channel and quantize the
/// columns the resamplers emit into the channel's `levels`, until each has
/// `limits.columns`. Returns how many columns every channel has.
///
/// A channel without a resampler emits every row. The resamplers of a
/// session are made, reset and fed together, so they emit at the same rows
/// and every channel ends with the same number of columns.
fn run_group(
    stfts: &mut [StftComputer],
    resamplers: &mut [PeakHoldResampler],
    levels: &mut [Vec<u8>],
    limits: Limits,
) -> usize {
    let mut least = usize::MAX;
    for (channel, (stft, levels)) in stfts.iter_mut().zip(levels.iter_mut()).enumerate() {
        levels.clear();
        let mut resampler = resamplers.get_mut(channel);
        let mut columns = 0;
        for _ in 0..limits.rows {
            if columns == limits.columns {
                break;
            }
            let Some(row) = stft.take_row() else {
                break;
            };
            let output = match resampler.as_deref_mut() {
                Some(resampler) => resampler.push(row),
                None => Some(row),
            };
            if let Some(output) = output {
                append_quantized_row(levels, output, limits.bins, limits.fft_size);
                columns += 1;
            }
        }
        debug_assert!(least == usize::MAX || least == columns);
        least = least.min(columns);
    }
    if least == usize::MAX {
        0
    } else {
        least
    }
}

/// A group of channels a helper computes, owned by the job while it runs.
struct Job {
    stfts: Vec<StftComputer>,
    resamplers: Vec<PeakHoldResampler>,
    levels: Vec<Vec<u8>>,
    limits: Limits,
    /// The group's columns, or `None` if computing them panicked.
    columns: Option<usize>,
}

struct Helper {
    jobs: Sender<Job>,
    done: Receiver<Job>,
    thread: Option<JoinHandle<()>>,
}

impl Helper {
    fn spawn(index: usize) -> std::io::Result<Self> {
        let (jobs, job_rx) = bounded::<Job>(1);
        let (done_tx, done) = bounded::<Job>(1);
        let thread = std::thread::Builder::new()
            .name(format!("ferrous-stft-{index}"))
            .spawn(move || {
                crate::worker_thread::lower_current_thread_priority();
                while let Ok(mut job) = job_rx.recv() {
                    let Job {
                        stfts,
                        resamplers,
                        levels,
                        limits,
                        ..
                    } = &mut job;
                    job.columns = catch_unwind(AssertUnwindSafe(|| {
                        run_group(stfts, resamplers, levels, *limits)
                    }))
                    .ok();
                    if done_tx.send(job).is_err() {
                        break;
                    }
                }
            })?;
        Ok(Self {
            jobs,
            done,
            thread: Some(thread),
        })
    }
}

impl Drop for Helper {
    fn drop(&mut self) {
        // A helper waits for its next job whenever its owner is not
        // waiting for it, so closing its jobs ends it at once.
        self.jobs = bounded(0).0;
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Computes batches of columns, and keeps the helper threads and the
/// buffers they fill between batches.
pub(super) struct ColumnBatcher {
    max_groups: usize,
    helpers: Vec<Helper>,
    /// Each channel's levels of the last batch, `columns * bins` bytes.
    levels: Vec<Vec<u8>>,
    bins: usize,
}

impl ColumnBatcher {
    /// A batcher that runs a batch on at most `max_groups` threads,
    /// starting helpers only once a stream has the channels for them.
    pub(super) fn new(max_groups: usize) -> Self {
        Self {
            max_groups: max_groups.max(1),
            helpers: Vec::new(),
            levels: Vec::new(),
            bins: 0,
        }
    }

    /// How many threads a batch of `channels` channels runs on, starting
    /// the helpers it needs that are not running yet.
    fn groups_for(&mut self, channels: usize) -> usize {
        let wanted = (channels / MIN_CHANNELS_PER_GROUP).clamp(1, self.max_groups);
        while self.helpers.len() + 1 < wanted {
            match Helper::spawn(self.helpers.len() + 1) {
                Ok(helper) => self.helpers.push(helper),
                Err(error) => {
                    // Not fatal: the decode thread computes the rest.
                    eprintln!("[ferrous] failed to start a spectrogram helper thread: {error}");
                    self.max_groups = self.helpers.len() + 1;
                    break;
                }
            }
        }
        wanted.min(self.helpers.len() + 1)
    }

    /// Whether batches of `channels` channels run on helper threads.
    pub(super) fn runs_in_parallel(&mut self, channels: usize) -> bool {
        self.groups_for(channels) > 1
    }

    /// Compute up to `max_columns` columns of every channel from the rows
    /// they all have, on helper threads too if `helpers`, and return how
    /// many there are; [`Self::append_column`] gives them. Zero when a
    /// channel needs more audio first.
    ///
    /// # Errors
    ///
    /// Fails if computing a channel panicked on a helper thread. The STFT
    /// state is then not to be trusted, and the session has to end.
    pub(super) fn compute(
        &mut self,
        stfts: &mut Vec<StftComputer>,
        resamplers: &mut Vec<PeakHoldResampler>,
        max_columns: usize,
        bins: usize,
        fft_size: usize,
        helpers: bool,
    ) -> Result<usize, String> {
        let channels = stfts.len();
        self.bins = bins;
        self.levels.resize_with(channels, Vec::new);
        for levels in &mut self.levels {
            levels.clear();
        }
        let rows = stfts
            .iter()
            .map(StftComputer::available_rows)
            .min()
            .unwrap_or(0);
        if rows == 0 || max_columns == 0 {
            return Ok(0);
        }
        let limits = Limits {
            rows,
            columns: max_columns,
            bins,
            fft_size,
        };
        let groups = if helpers {
            self.groups_for(channels)
        } else {
            1
        };
        match groups {
            1 => Ok(run_group(stfts, resamplers, &mut self.levels, limits)),
            groups => self.compute_in_groups(stfts, resamplers, groups, limits),
        }
    }

    fn compute_in_groups(
        &mut self,
        stfts: &mut Vec<StftComputer>,
        resamplers: &mut Vec<PeakHoldResampler>,
        groups: usize,
        limits: Limits,
    ) -> Result<usize, String> {
        let channels = stfts.len();
        // The decode thread keeps the first, smallest group, as it also
        // interleaves the batch afterwards.
        let own = channels / groups;
        let mut rest_stfts = stfts.split_off(own);
        let mut rest_resamplers = resamplers.split_off(own.min(resamplers.len()));
        let mut rest_levels = self.levels.split_off(own);
        let mut handed_out = Vec::with_capacity(groups - 1);
        for (index, helper) in self.helpers.iter().take(groups - 1).enumerate() {
            // The helpers' groups split the rest as evenly as they can.
            let helpers_left = groups - 1 - index;
            let size = rest_stfts.len().div_ceil(helpers_left);
            let job = Job {
                stfts: take_front(&mut rest_stfts, size),
                resamplers: take_front(&mut rest_resamplers, size),
                levels: take_front(&mut rest_levels, size),
                limits,
                columns: None,
            };
            // A helper that ended computes nothing; this thread does.
            handed_out.push(
                helper
                    .jobs
                    .send(job)
                    .map(|()| helper)
                    .map_err(crossbeam_channel::SendError::into_inner),
            );
        }
        let mut columns = run_group(stfts, resamplers, &mut self.levels, limits);
        let mut failure = None;
        for handed in handed_out {
            let mut job = match handed {
                Ok(helper) => {
                    let Ok(job) = helper.done.recv() else {
                        failure = Some("a spectrogram helper thread ended".to_string());
                        continue;
                    };
                    job
                }
                Err(mut job) => {
                    job.columns = Some(run_group(
                        &mut job.stfts,
                        &mut job.resamplers,
                        &mut job.levels,
                        limits,
                    ));
                    job
                }
            };
            match job.columns {
                Some(group_columns) => {
                    debug_assert_eq!(group_columns, columns);
                    columns = columns.min(group_columns);
                }
                None => failure = Some("computing its spectrogram failed".to_string()),
            }
            stfts.append(&mut job.stfts);
            resamplers.append(&mut job.resamplers);
            self.levels.append(&mut job.levels);
        }
        if let Some(failure) = failure {
            // Start over with fresh helpers next time.
            self.helpers.clear();
            return Err(failure);
        }
        Ok(columns)
    }

    /// Append column `column` of the last batch, every channel's levels in
    /// channel order.
    pub(super) fn append_column(&self, column: usize, out: &mut Vec<u8>) {
        let range = column * self.bins..(column + 1) * self.bins;
        for levels in &self.levels {
            out.extend_from_slice(&levels[range.clone()]);
        }
    }
}

/// Move the first `count` items of `items` into a vector of their own.
fn take_front<T>(items: &mut Vec<T>, count: usize) -> Vec<T> {
    let rest = items.split_off(count.min(items.len()));
    std::mem::replace(items, rest)
}

thread_local! {
    /// The batcher of the decode thread, whose helpers last as long as it.
    static BATCHER: RefCell<ColumnBatcher> = RefCell::new(ColumnBatcher::new(
        std::thread::available_parallelism().map_or(1, |cores| cores.get().min(MAX_GROUPS)),
    ));
}

/// Run `work` with this thread's batcher.
pub(super) fn with_thread_batcher<R>(work: impl FnOnce(&mut ColumnBatcher) -> R) -> R {
    BATCHER.with_borrow_mut(work)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `channels` channels of `frames` frames: a different pair of tones in
    /// each, with noise, so that columns in the wrong channel would show.
    fn signal(channels: usize, frames: usize) -> Vec<Vec<f32>> {
        let mut state = 0x9e37_79b9_7f4a_7c15_u64;
        (0..channels)
            .map(|channel| {
                let low = 0.01 * (channel + 1) as f32;
                let high = 0.13 + 0.017 * channel as f32;
                (0..frames)
                    .map(|frame| {
                        state ^= state << 13;
                        state ^= state >> 7;
                        state ^= state << 17;
                        let noise = (state >> 40) as f32 / (1 << 24) as f32 - 0.5;
                        let t = frame as f32;
                        0.5 * (low * t).sin() + 0.2 * (high * t).sin() + 0.01 * noise
                    })
                    .collect()
            })
            .collect()
    }

    /// Packet sizes that do not line up with the hop or the FFT.
    const PACKETS: [usize; 5] = [1_000, 4_096, 37, 2_500, 8_192];

    fn computers(
        channels: usize,
        fft_size: usize,
        hop_size: usize,
        interval: Option<f64>,
    ) -> (Vec<StftComputer>, Vec<PeakHoldResampler>) {
        let stfts = (0..channels)
            .map(|_| StftComputer::new(fft_size, hop_size))
            .collect();
        let resamplers = interval.map_or_else(Vec::new, |interval| {
            (0..channels)
                .map(|_| {
                    let mut resampler = PeakHoldResampler::new(interval);
                    resampler.reset_at_column(3, hop_size, 1_024);
                    resampler
                })
                .collect()
        });
        (stfts, resamplers)
    }

    /// Feed `audio` packet by packet and collect every column, as the
    /// session did before batches: a column at a time across channels.
    fn columns_one_at_a_time(
        audio: &[Vec<f32>],
        fft_size: usize,
        hop_size: usize,
        interval: Option<f64>,
    ) -> Vec<u8> {
        let bins = fft_size / 2 + 1;
        let (mut stfts, mut resamplers) = computers(audio.len(), fft_size, hop_size, interval);
        let mut out = Vec::new();
        let mut start = 0;
        for size in PACKETS.iter().cycle() {
            if start >= audio[0].len() {
                break;
            }
            let end = (start + size).min(audio[0].len());
            for (stft, samples) in stfts.iter_mut().zip(audio) {
                stft.enqueue_samples(&samples[start..end]);
            }
            start = end;
            while stfts.iter().all(StftComputer::has_row) {
                let column = out.len();
                let mut emitted = true;
                for (channel, stft) in stfts.iter_mut().enumerate() {
                    let row = stft.take_row().unwrap();
                    let output = resamplers
                        .get_mut(channel)
                        .map_or(Some(row), |resampler| resampler.push(row));
                    match output {
                        Some(output) => append_quantized_row(&mut out, output, bins, fft_size),
                        None => emitted = false,
                    }
                }
                if !emitted {
                    out.truncate(column);
                }
            }
        }
        out
    }

    fn columns_in_batches(
        batcher: &mut ColumnBatcher,
        audio: &[Vec<f32>],
        fft_size: usize,
        hop_size: usize,
        interval: Option<f64>,
        max_columns: usize,
    ) -> Vec<u8> {
        let bins = fft_size / 2 + 1;
        let (mut stfts, mut resamplers) = computers(audio.len(), fft_size, hop_size, interval);
        let mut out = Vec::new();
        let mut start = 0;
        for size in PACKETS.iter().cycle() {
            if start >= audio[0].len() {
                break;
            }
            let end = (start + size).min(audio[0].len());
            for (stft, samples) in stfts.iter_mut().zip(audio) {
                stft.enqueue_samples(&samples[start..end]);
            }
            start = end;
            loop {
                let columns = batcher
                    .compute(
                        &mut stfts,
                        &mut resamplers,
                        max_columns,
                        bins,
                        fft_size,
                        true,
                    )
                    .expect("batch");
                if columns == 0 {
                    break;
                }
                assert!(columns <= max_columns);
                for column in 0..columns {
                    batcher.append_column(column, &mut out);
                }
            }
            assert!(!stfts.iter().all(StftComputer::has_row));
        }
        assert_eq!(stfts.len(), audio.len(), "every channel came back");
        out
    }

    #[test]
    fn batches_on_any_number_of_threads_match_columns_one_at_a_time() {
        for channels in [1, 2, 3, 6, 8] {
            let audio = signal(channels, 40_000);
            for (fft_size, hop_size, interval) in [
                (512, 128, Some(4.0)),
                (2_048, 256, Some(2.7)),
                (256, 1_024, None),
            ] {
                let expected = columns_one_at_a_time(&audio, fft_size, hop_size, interval);
                assert!(!expected.is_empty());
                for max_groups in [1, 2, 3] {
                    let mut batcher = ColumnBatcher::new(max_groups);
                    for max_columns in [1, 5, MAX_BATCH_COLUMNS] {
                        let batched = columns_in_batches(
                            &mut batcher,
                            &audio,
                            fft_size,
                            hop_size,
                            interval,
                            max_columns,
                        );
                        assert!(
                            batched == expected,
                            "{channels} channels, FFT {fft_size}, hop {hop_size}, \
                             {max_groups} threads, {max_columns} columns a batch"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn stereo_stays_on_the_calling_thread_and_surround_uses_helpers() {
        let mut batcher = ColumnBatcher::new(MAX_GROUPS);
        assert_eq!(batcher.groups_for(1), 1);
        assert_eq!(batcher.groups_for(2), 1);
        assert!(batcher.helpers.is_empty());
        assert_eq!(batcher.groups_for(4), 2);
        assert_eq!(batcher.groups_for(6), 3);
        assert_eq!(batcher.groups_for(8), 3);
        assert_eq!(batcher.helpers.len(), MAX_GROUPS - 1);
        let mut single = ColumnBatcher::new(1);
        assert_eq!(single.groups_for(8), 1);
        assert!(single.helpers.is_empty());
    }

    #[test]
    fn a_batch_takes_only_the_rows_every_channel_has() {
        let mut batcher = ColumnBatcher::new(MAX_GROUPS);
        let (mut stfts, mut resamplers) = computers(6, 512, 128, None);
        for (channel, stft) in stfts.iter_mut().enumerate() {
            // Channel 4 has two rows; the others five.
            let frames = if channel == 4 {
                512 + 128
            } else {
                512 + 4 * 128
            };
            stft.enqueue_samples(&vec![0.25; frames]);
        }
        let columns = batcher
            .compute(
                &mut stfts,
                &mut resamplers,
                MAX_BATCH_COLUMNS,
                257,
                512,
                true,
            )
            .expect("batch");
        assert_eq!(columns, 2);
        let rows: Vec<_> = stfts.iter().map(StftComputer::available_rows).collect();
        assert_eq!(rows, [3, 3, 3, 3, 0, 3]);
        let mut column = Vec::new();
        batcher.append_column(1, &mut column);
        assert_eq!(column.len(), 6 * 257);
    }

    #[test]
    fn a_helper_that_ended_leaves_its_channels_to_the_calling_thread() {
        let audio = signal(6, 20_000);
        let expected = columns_one_at_a_time(&audio, 512, 128, Some(4.0));
        let mut batcher = ColumnBatcher::new(MAX_GROUPS);
        assert_eq!(batcher.groups_for(6), 3);
        // End the second helper as if its thread had gone: its jobs fail
        // to send, and this thread computes them.
        let helper = &mut batcher.helpers[1];
        helper.jobs = bounded(0).0;
        if let Some(thread) = helper.thread.take() {
            thread.join().expect("helper");
        }
        let batched = columns_in_batches(&mut batcher, &audio, 512, 128, Some(4.0), 7);
        assert!(batched == expected);
    }
}
