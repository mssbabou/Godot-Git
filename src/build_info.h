#pragma once

// Where this library was built, so a user can tell a CI release (verifiable against the public
// build log) from a local build. Filled in by SConstruct; plain C strings (no global Strings).
namespace godot_git {

const char *build_version(); // "v0.3.0", "dev-5cbbd8d", or `git describe` for a local build.
const char *build_commit(); // Full commit hash of a CI build, "" for a local build.
const char *build_url(); // The CI run that built it (its log and attestations), "" for a local build.

} // namespace godot_git
