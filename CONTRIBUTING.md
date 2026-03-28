# Contributing to Ferrous

Thanks for your interest in contributing! Ferrous is a personal project and contributions are welcome on a case-by-case basis.

## Before You Start

Please open an issue before submitting a pull request. This avoids wasted effort if the change doesn't align with the project's direction.

## Development Setup

See the [installation guide](docs/INSTALL.md) for build prerequisites and the [development guide](docs/DEVELOPMENT.md) for building, testing, and debugging.

## Submitting Changes

1. Fork the repository and create a feature branch.
2. Make your changes. Follow existing code style and naming conventions.
3. Run the full test suite: `./scripts/run-tests.sh`. All checks must pass with zero warnings.
4. Submit a pull request with a clear description of the change and its motivation.

## Code Style

- Rust: `cargo fmt`, `cargo clippy -- -D clippy::pedantic`
- C++: match existing style (C++20, `m_` member prefix, `camelCase` methods)
- QML: match existing style

## License

By contributing, you agree that your contributions will be licensed under the [GNU General Public License v3.0](LICENSE).
