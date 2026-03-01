mod analysis;
mod app;
mod metadata;
mod playback;
mod ui;

use app::FerrousApp;
use eframe::egui;
use tracing_subscriber::{fmt, EnvFilter};

fn main() -> eframe::Result<()> {
    fmt()
        .with_env_filter(EnvFilter::from_default_env())
        .without_time()
        .init();

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1520.0, 860.0])
            .with_min_inner_size([1200.0, 700.0]),
        ..Default::default()
    };
    eframe::run_native(
        "Ferrous",
        options,
        Box::new(|cc| Ok(Box::new(FerrousApp::new(cc)))),
    )
}
