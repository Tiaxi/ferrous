// SPDX-License-Identifier: GPL-3.0-or-later

use super::{AnalysisCommand, AnalysisEvent};
use crossbeam_channel::{SendTimeoutError, Sender};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    Arc,
};
use std::time::Duration;

pub(super) trait AnalysisEventOutput: Send + Sync {
    fn send(&self, event: AnalysisEvent) -> Result<(), ()>;

    /// Whether spectral chunks are waiting for the consumer to take them.
    fn spectral_backpressured(&self) -> bool {
        false
    }
}

impl AnalysisEventOutput for Sender<AnalysisEvent> {
    fn send(&self, event: AnalysisEvent) -> Result<(), ()> {
        Sender::send(self, event).map_err(|_| ())
    }
}

/// Keep waveform/control updates independent of a backpressured spectral stream.
#[derive(Clone)]
pub(super) struct AnalysisOutputs {
    pub snapshots: Sender<AnalysisEvent>,
    pub spectra: Sender<AnalysisEvent>,
    pub generation: Arc<AtomicU64>,
    pub pending_resets: Arc<AtomicUsize>,
    /// Set when the engine shuts down, so a send waiting for the spectral
    /// stream gives up instead of holding the thread past shutdown.
    pub closed: Arc<AtomicBool>,
}

impl AnalysisEventOutput for AnalysisOutputs {
    fn send(&self, mut event: AnalysisEvent) -> Result<(), ()> {
        let generation = match &event {
            AnalysisEvent::Snapshot(_) | AnalysisEvent::WorkerFailed(_) => {
                return self.snapshots.send(event).map_err(|_| ());
            }
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => chunk.generation,
        };
        loop {
            if self.closed.load(Ordering::Relaxed) {
                return Err(());
            }
            if self.pending_resets.load(Ordering::Relaxed) > 0 {
                return Err(());
            }
            // Every chunk carries the generation of the session it belongs
            // to, so a chunk waiting here stops waiting once it is stale.
            if generation != self.generation.load(Ordering::Relaxed) {
                return Err(());
            }
            match self.spectra.send_timeout(event, Duration::from_millis(20)) {
                Ok(()) => return Ok(()),
                Err(SendTimeoutError::Disconnected(_)) => return Err(()),
                Err(SendTimeoutError::Timeout(pending)) => event = pending,
            }
        }
    }

    fn spectral_backpressured(&self) -> bool {
        self.spectra.is_full()
    }
}

pub(super) fn resets_spectral_stream(command: &AnalysisCommand) -> bool {
    matches!(
        command,
        AnalysisCommand::RestartCurrentTrack { .. }
            | AnalysisCommand::SetTrack { gapless: false, .. }
            | AnalysisCommand::SetFftSize(_)
            | AnalysisCommand::SetSpectrogramActive(false)
    )
}

impl AnalysisOutputs {
    pub fn begin_command(&self, command: &AnalysisCommand) {
        if resets_spectral_stream(command) {
            saturating_decrement(&self.pending_resets);
        }
    }
}

/// Decrement `counter` unless it is already zero.
fn saturating_decrement(counter: &AtomicUsize) {
    let mut count = counter.load(Ordering::Relaxed);
    while count > 0 {
        match counter.compare_exchange_weak(count, count - 1, Ordering::Relaxed, Ordering::Relaxed)
        {
            Ok(_) => return,
            Err(current) => count = current,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn outputs(pending_resets: usize) -> AnalysisOutputs {
        let (snapshots, _) = crossbeam_channel::unbounded();
        let (spectra, _) = crossbeam_channel::unbounded();
        AnalysisOutputs {
            snapshots,
            spectra,
            generation: Arc::new(AtomicU64::new(0)),
            pending_resets: Arc::new(AtomicUsize::new(pending_resets)),
            closed: Arc::new(AtomicBool::new(false)),
        }
    }

    #[test]
    fn a_reset_command_takes_one_pending_reset_and_never_goes_below_zero() {
        let outputs = outputs(2);
        outputs.begin_command(&AnalysisCommand::SetFftSize(512));
        assert_eq!(outputs.pending_resets.load(Ordering::Relaxed), 1);
        outputs.begin_command(&AnalysisCommand::SetFftSize(512));
        assert_eq!(outputs.pending_resets.load(Ordering::Relaxed), 0);
        outputs.begin_command(&AnalysisCommand::SetFftSize(512));
        assert_eq!(outputs.pending_resets.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn a_command_that_keeps_the_stream_leaves_pending_resets_alone() {
        let outputs = outputs(1);
        outputs.begin_command(&AnalysisCommand::SetSpectrogramActive(true));
        assert_eq!(outputs.pending_resets.load(Ordering::Relaxed), 1);
    }
}
