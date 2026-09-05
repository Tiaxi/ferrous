// SPDX-License-Identifier: GPL-3.0-or-later

use gstreamer::{self as gst, prelude::*};

/// Own shutdown from the first state change, including failed negotiation.
/// A successfully opened streaming decoder can take over this responsibility.
pub(super) struct PipelineShutdown(Option<gst::Pipeline>);

impl PipelineShutdown {
    pub(super) fn new(pipeline: &gst::Pipeline) -> Self {
        Self(Some(pipeline.clone()))
    }

    pub(super) fn disarm(mut self) {
        self.0 = None;
    }
}

impl Drop for PipelineShutdown {
    fn drop(&mut self) {
        if let Some(pipeline) = &self.0 {
            let _ = pipeline.set_state(gst::State::Null);
        }
    }
}
