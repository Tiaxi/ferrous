// SPDX-License-Identifier: GPL-3.0-or-later

use super::{AnalysisCommand, AnalysisEvent};
use crossbeam_channel::{SendTimeoutError, Sender};
use std::sync::{
    atomic::{AtomicU64, AtomicUsize, Ordering},
    Arc,
};
use std::time::Duration;

pub(super) trait AnalysisEventOutput: Send + Sync {
    fn send(&self, event: AnalysisEvent) -> Result<(), ()>;
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
}

impl AnalysisEventOutput for AnalysisOutputs {
    fn send(&self, mut event: AnalysisEvent) -> Result<(), ()> {
        let generation = match &event {
            AnalysisEvent::Snapshot(_) => return self.snapshots.send(event).map_err(|_| ()),
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => chunk.generation,
        };
        loop {
            if self.pending_resets.load(Ordering::Relaxed) > 0 {
                return Err(());
            }
            if generation != 0 && generation != self.generation.load(Ordering::Relaxed) {
                return Err(());
            }
            match self.spectra.send_timeout(event, Duration::from_millis(20)) {
                Ok(()) => return Ok(()),
                Err(SendTimeoutError::Disconnected(_)) => return Err(()),
                Err(SendTimeoutError::Timeout(pending)) => event = pending,
            }
        }
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
            let _ =
                self.pending_resets
                    .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |count| {
                        Some(count.saturating_sub(1))
                    });
        }
    }
}
