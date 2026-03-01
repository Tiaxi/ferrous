use eframe::egui;
use ferrous::app::FerrousApp;
use tracing_subscriber::{fmt, EnvFilter};

fn main() -> eframe::Result<()> {
    fmt()
        .with_env_filter(EnvFilter::from_default_env())
        .without_time()
        .init();

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1600.0, 980.0])
            .with_min_inner_size([1280.0, 780.0]),
        ..Default::default()
    };
    eframe::run_native(
        "Ferrous",
        options,
        Box::new(|cc| Ok(Box::new(FerrousApp::new(cc)))),
    )
}
