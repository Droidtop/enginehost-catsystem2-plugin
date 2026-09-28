# Changelog

All notable changes to this plugin are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Unstable and testing stay moving pointers to whichever build was last
published to each; every build they ever point at also gets a permanent
release of its own (`<bundle>-build.<run>`), which is never overwritten.

## [Unreleased]

### Added

- A permanent, never-overwritten release for every bundle build published
  to any channel, so a version that was once installable stays that way in
  the release history even after the next push moves the channel pointers.

## [0.1] - 2026-09-26

This is the plugin's first version-history entry: there was no changelog
before this release, so this section summarizes what already exists.

### Added

- Runs CatSystem2 (`.cst`/`.int`) visual novels through a native wrapper
  around the engine, with the JNI glue and API stubs living on this
  plugin's own `plugin-core` line.
- Sandbox layer 2 support: isolated launches get their own audio ring and
  file broker bridge, and native registration exports correctly when
  running isolated instead of unsandboxed.
- Console screen shows only the rows that changed and reports where a
  frame went, with a pad the view can drive itself and one selection
  shared between pointer and focus.
- Signed bundle releases on the unstable and testing channels, verified
  file by file against this repository's pinned key at install time.

### Fixed

- `jni.c` compiled without including `unistd.h` for `dup()`.
- This repo's own `compileOnly` API stubs were missing `EngineFileBroker`.
- The static initializer's `UnsatisfiedLinkError` is now only swallowed
  when the launch is actually isolated.
