# Contributing to Ferrous

Thanks for your interest in contributing! Ferrous is a personal project and contributions are welcome on a case-by-case basis.

## Before You Start

Please open an issue before submitting a pull request. This avoids wasted effort if the change doesn't align with the project's direction.

## Development Setup

See the [installation guide](docs/INSTALL.md) for build prerequisites and the [development guide](docs/DEVELOPMENT.md) for building, testing, and debugging.

## Submitting Changes

1. Fork the repository and create a branch named `<type>/<branch_name>` (for example, `fix/queue-restore`).
2. Make your changes. Follow existing code style and naming conventions.
3. Add tests for new behavior and regression tests for fixes. For changes spanning Rust and Qt, cover both sides. Use self-contained fixtures, without network access or personal media files.
4. Validate using the [development guide](docs/DEVELOPMENT.md#validation). Introduce no warnings; report any checks you could not run.
5. Use lowercase conventional commit subjects, such as `fix: restore queue selection`.
6. Submit a pull request describing the problem, resulting behavior, and validation.

## Code Style

- Rust: rustfmt and strict Clippy through `./scripts/run-tests.sh`
- C++: match existing style (C++20, `m_` member prefix, `camelCase` methods)
- QML: match existing style

## License

By contributing, you agree that your contributions will be licensed under the [GNU General Public License v3.0](LICENSE).
