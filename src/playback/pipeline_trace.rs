// SPDX-License-Identifier: GPL-3.0-or-later

//! What the pipeline of a test does, for the report of a wait that timed
//! out.

use std::collections::VecDeque;
use std::fmt::Write;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crossbeam_channel::bounded;
use gst::prelude::*;
use gstreamer as gst;

/// Bus messages that a report lists.
const TRACED_MESSAGES: usize = 24;
/// How long a report waits for the pipeline to answer its queries.
const REPORT_TIMEOUT: Duration = Duration::from_secs(2);

/// Records the bus messages of a pipeline, to report them with its state
/// when a wait of a test times out.
pub(crate) struct PipelineTrace {
    started: Instant,
    pipeline: gst::Element,
    /// The latest bus messages as they were posted, described, with the
    /// time since the test started. Without the state changes of the
    /// pipeline's elements and the status of its streaming threads.
    ///
    /// The log holds descriptions rather than the messages: the bus owns
    /// the handler that fills the log, and a message would keep its source,
    /// and so the pipeline and its bus, alive.
    messages: Arc<Mutex<VecDeque<(Duration, String)>>>,
}

impl PipelineTrace {
    pub(crate) fn new(pipeline: &gst::Element) -> Self {
        let started = Instant::now();
        let messages = Arc::new(Mutex::new(VecDeque::new()));
        let bus = pipeline.bus().expect("pipeline has a bus");
        bus.enable_sync_message_emission();
        let log = Arc::clone(&messages);
        let weak = pipeline.downgrade();
        // Runs on the posting thread, so it only describes the message.
        bus.connect_sync_message(None, move |_, message| {
            let noise = match message.view() {
                gst::MessageView::StreamStatus(_) => true,
                gst::MessageView::StateChanged(_) => weak
                    .upgrade()
                    .is_none_or(|pipeline| message.src() != Some(pipeline.upcast_ref())),
                _ => false,
            };
            if noise {
                return;
            }
            let entry = (started.elapsed(), describe_message(message));
            if let Ok(mut log) = log.lock() {
                if log.len() == TRACED_MESSAGES {
                    log.pop_front();
                }
                log.push_back(entry);
            }
        });
        Self {
            started,
            pipeline: pipeline.clone(),
            messages,
        }
    }

    /// Report `what` the test waited for, `details` from the test, and what
    /// the pipeline does. The pipeline is queried on another thread, so a
    /// pipeline that blocks is reported too.
    pub(crate) fn report(&self, what: &str, details: &str) -> String {
        let (tx, rx) = bounded(1);
        let pipeline = self.pipeline.clone();
        let messages = Arc::clone(&self.messages);
        std::thread::spawn(move || {
            let report = pipeline_report(&pipeline, &messages);
            // Before the report arrives, so a test that drops the pipeline
            // after it frees the pipeline.
            drop(pipeline);
            let _ = tx.send(report);
        });
        let pipeline = rx.recv_timeout(REPORT_TIMEOUT).unwrap_or_else(|_| {
            format!("  the pipeline did not answer its queries within {REPORT_TIMEOUT:?}\n")
        });
        format!(
            "{what}\n  waited until {:.3} s after the test started\n{details}{pipeline}",
            self.started.elapsed().as_secs_f64(),
        )
    }
}

/// Describe the state of `pipeline` and its elements, and the latest bus
/// `messages`.
fn pipeline_report(
    pipeline: &gst::Element,
    messages: &Mutex<VecDeque<(Duration, String)>>,
) -> String {
    let mut report = String::new();
    let (result, current, pending) = pipeline.state(Some(gst::ClockTime::ZERO));
    let _ = writeln!(
        report,
        "  pipeline: {current:?}, pending {pending:?} ({result:?}), position {:?}",
        pipeline.query_position::<gst::ClockTime>()
    );
    let unsettled: Vec<_> = pipeline
        .downcast_ref::<gst::Bin>()
        .map(|bin| {
            bin.iterate_recurse()
                .into_iter()
                .filter_map(Result::ok)
                .filter_map(|element| {
                    let (current, pending) = (element.current_state(), element.pending_state());
                    (pending != gst::State::VoidPending)
                        .then(|| format!("{} {current:?} → {pending:?}", element.name()))
                })
                .collect()
        })
        .unwrap_or_default();
    let _ = writeln!(report, "  elements changing state: {unsettled:?}");
    let _ = writeln!(report, "  latest bus messages:");
    let messages: Vec<_> = messages
        .lock()
        .map(|log| log.clone())
        .unwrap_or_default()
        .into();
    for (at, message) in messages {
        let _ = writeln!(report, "    {:8.3} s {message}", at.as_secs_f64());
    }
    report
}

fn describe_message(message: &gst::Message) -> String {
    let detail = match message.view() {
        gst::MessageView::StateChanged(change) => format!(
            "{:?} → {:?}, pending {:?}",
            change.old(),
            change.current(),
            change.pending()
        ),
        gst::MessageView::StreamStart(start) => format!("group {:?}", start.group_id()),
        gst::MessageView::AsyncDone(done) => format!("running time {:?}", done.running_time()),
        gst::MessageView::Error(error) => format!("{} ({:?})", error.error(), error.debug()),
        gst::MessageView::Warning(warning) => {
            format!("{} ({:?})", warning.error(), warning.debug())
        }
        gst::MessageView::Buffering(buffering) => format!("{} %", buffering.percent()),
        _ => message
            .structure()
            .map(|structure| structure.name().to_string())
            .unwrap_or_default(),
    };
    format!(
        "{:?} from {}: {detail}",
        message.type_(),
        message
            .src()
            .map_or_else(|| "?".to_string(), |src| src.path_string().to_string())
    )
}

#[cfg(test)]
mod tests {
    use super::PipelineTrace;
    use gst::prelude::*;
    use gstreamer as gst;

    #[test]
    fn a_traced_pipeline_is_freed_once_the_test_drops_it() {
        gst::init().unwrap();
        let pipeline = gst::Pipeline::new();
        let source = gst::ElementFactory::make("fakesrc")
            .property("num-buffers", 1i32)
            .build()
            .unwrap();
        let sink = gst::ElementFactory::make("fakesink").build().unwrap();
        pipeline.add_many([&source, &sink]).unwrap();
        source.link(&sink).unwrap();
        let trace = PipelineTrace::new(pipeline.upcast_ref());
        pipeline.set_state(gst::State::Playing).unwrap();
        let bus = pipeline.bus().unwrap();
        bus.timed_pop_filtered(gst::ClockTime::from_seconds(5), &[gst::MessageType::Eos])
            .expect("the pipeline plays to its end");
        pipeline.set_state(gst::State::Null).unwrap();
        let report = trace.report("a report", "  details\n");
        assert!(report.contains("  details\n"), "{report}");
        assert!(report.contains("Eos from"), "{report}");

        let weak = pipeline.downgrade();
        drop((trace, bus, source, sink, pipeline));
        assert!(
            weak.upgrade().is_none(),
            "the messages in the log keep the pipeline alive"
        );
    }
}
