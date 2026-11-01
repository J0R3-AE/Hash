#pragma once

// Entry point shared by the standalone CLI and the GUI.
//
// The standalone executable provides main() separately.
// The GUI can call this function directly without launching
// a second executable.
int ytdl_cli_main(int argc, char* argv[]);